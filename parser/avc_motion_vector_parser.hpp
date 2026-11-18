// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/*
 * Stateful H.264 motion-vector extractor.
 *
 * The extractor owns only parser state (the MV-DPB and the current picture's
 * bookkeeping).  When a consumer supplies a buffer through MotionVectorSink,
 * the dense 4x4 grid is written into it exactly once and, when the ready queue
 * is enabled, the same buffer lease is retained by the deferred queue until
 * delivery; reference pictures retain the buffer lease until they leave the
 * DPB.  If no buffer is supplied for a picture, the extractor falls back to an
 * internal grid so that later temporal-direct prediction remains correct.
 *
 * The DPB models decoded reference frames, complementary reference field
 * pairs and non-paired reference fields.  Frame pictures (including MBAFF)
 * occupy one entry whose two field views alias the same frame grid; PAFF
 * fields are stored separately and combined lazily when a frame picture
 * references a field pair.  Marking follows the sliding-window and adaptive
 * MMCO processes of H.264 8.2.5, and reference lists are built with the
 * FrameNumWrap/PicNum and field-parity rules of 8.2.4.
 *
 * Monochrome pictures fill the same grid.  With separate_colour_plane_flag
 * each plane is an independent picture with its own MotionPlaneExtractor,
 * MV-DPB and decode order.  The public facade delivers one buffer per plane,
 * tagged by colour_plane_id/plane_count, and coordinates deferred output so
 * all planes of an access unit are emitted in 0/1/2 order.
 */

#include "../bitstream/rbsp_extract.hpp"
#include "../bitstream/rbsp_reader.hpp"
#include "../syntax/avc_motion_vector.hpp"
#include "../syntax/avc_pps.hpp"
#include "../syntax/avc_slice_header.hpp"
#include "../syntax/avc_sps.hpp"
#include "../utils/ObjectPool.h"
#include "avc_cell_simd.hpp"
#include "avc_mb_parser.hpp"
#include "avc_nal_unit_parser.hpp"
#include "avc_slice_parser.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace bs {
namespace avc {

class MotionPlaneExtractor {
   public:
    struct MotionGrid {
        std::vector<MvCell> cells;
        void reset() noexcept { cells.clear(); }
    };

    struct MotionGridPolicy {
        [[nodiscard]] MotionGrid* create() { return new MotionGrid; }
        void reset(MotionGrid* grid) noexcept { grid->reset(); }
        void destroy(MotionGrid* grid) noexcept { delete grid; }
    };

    using MotionGridPool = bs::utils::ObjectPool<MotionGrid, MotionGridPolicy>;

    MotionPlaneExtractor()
        : grid_pool_(std::make_unique<MotionGridPool>(
                  MotionGridPolicy{},
                  MotionGridPool::Config{2, 32, 32})) {
        /*
         * mv-dpb entries and the deferred queue are bounded by the stream
         * syntax (SPS max_num_ref_frames is capped at 16, plus field pairs);
         * reserving avoids the first-picture reallocations.
         */
        refs_.reserve(32);
        pending_.reserve(16);
        rbsp_.reserve(1u << 16);
    }

    void set_sink(const MotionVectorSink& sink) { sink_ = sink; }
    void clear_sink() { sink_ = MotionVectorSink{}; }

    /* Configure the identity written into every delivered buffer. */
    void set_colour_plane(std::uint8_t colour_plane_id, std::uint8_t plane_count) noexcept {
        colour_plane_id_ = colour_plane_id > 2 ? 2 : colour_plane_id;
        plane_count_ = plane_count == 0 ? 1 : (plane_count > 3 ? 3 : plane_count);
    }

    void reset() {
        refs_.clear();
        current_ = CurrentPicture{};
        pending_.clear();
        status_ = MotionVectorStatus::Ok;
        max_long_term_frame_idx_ = -1;
        next_frame_id_ = 1;
        prev_ref_frame_num_ = 0;
        prev_ref_frame_num_valid_ = false;
        gap_poc_state_ = GapPocState{};
        last_picture_valid_ = false;
        last_picture_field_ = false;
        last_picture_bottom_ = false;
        last_picture_frame_num_ = 0;
        last_frame_id_ = 0;
        if (grid_pool_) {
            grid_pool_->trim();
        }
    }

    [[nodiscard]] MotionVectorStatus status() const noexcept { return status_; }

    /*
     * Number of reference frames/pairs/non-paired fields currently held in
     * the MV-DPB.  A PAFF pair counts once, as specified by 8.2.5.1.
     */
    [[nodiscard]] std::size_t referencePictureCount() const noexcept { return refs_.size(); }

    /*
     * Diagnostic accessor for the DPB marker.  Intended for tests and
     * consumers that need to know whether the current DPB uses long-term
     * indices; it is not required for motion-vector extraction.
     */
    [[nodiscard]] int maxLongTermFrameIdx() const noexcept { return max_long_term_frame_idx_; }

    void on_slice(
            const NalUnit& nal,
            const SequenceParameterSet& sps,
            const PictureParameterSet& pps,
            const SliceHeader& header,
            bool idr,
            std::uint8_t nal_ref_idc,
            std::size_t header_bit_offset = 0) {
        try {
            on_slice_impl(nal, sps, pps, header, idr, nal_ref_idc, header_bit_offset);
        } catch (...) {
            status_ = MotionVectorStatus::Malformed;
            try {
                flush();
            } catch (...) {
                current_ = CurrentPicture{};
            }
        }
    }

    /*
     * Flush the picture currently being accumulated.  The module treats one
     * first_mb_in_slice == 0 as one picture; multi-slice pictures accumulate
     * until the next picture or the parse-end flush.
     *
     * With MotionVectorOptions::defer_ready (the default), the completed
     * picture is queued with the consumer buffer it was parsed into instead of
     * being handed to ready() immediately.  finish() drains that queue at the
     * end of parsing; an IDR with no_output_of_prior_pics_flag == 1 discards
     * it.
     */
    void flush() {
        if (!current_.started) {
            return;
        }

        /*
         * Complete cell coverage before the grid is captured by the DPB or
         * delivered to the consumer: fill macroblocks no slice touched.
         */
        fillUncoveredCells();

        finalizeSliceReferences();

        /*
         * Make sure the per-slice table and macroblock map fit before the DPB
         * entry captures the cell pointer.  Growing reallocates the consumer
         * buffer and copies the grid once; it is rare (multi-slice/FMO with
         * more slots than the initial reservation), and doing it here keeps the
         * DPB and the delivered buffer identical for the deferred path too.
         */
        if (current_.sink_buffer.data != nullptr) {
            const MotionVectorStatus status_before = current_.status;
            const MotionVectorStatus extractor_status_before = status_;
            if (!ensureAuxCapacity(
                        static_cast<std::uint32_t>(current_.slice_refs.size()),
                        static_cast<std::uint32_t>(current_.slice_map.size())) &&
                sink_.options.defer_ready) {
                /*
                 * A failed grow is transient in deferred mode: the consumer
                 * may satisfy the exact-size request when the queue drains.
                 * The old deferred path never grew at flush time, so keep its
                 * status and spill this picture to the copy queue.
                 */
                current_.status = status_before;
                status_ = extractor_status_before;
                current_.buffer_unavailable = true;
            }
        }

        /*
         * A deferred picture whose buffer acquire (or table-capacity grow)
         * failed only because the queue is holding earlier buffers is still
         * complete: spill it to the copy queue and let drain re-acquire in
         * order after those earlier leases are released.  Its parse status was
         * never a syntax error, so expose Ok if it is delivered.  This is
         * deliberately more complete than the old deferred path, which dropped
         * the output whenever the per-picture acquire failed even though the
         * picture itself had parsed fully; a drain-time acquire that really
         * fails still reports BufferUnavailable.
         */
        const bool spill =
                sink_.options.defer_ready && current_.buffer_unavailable &&
                current_.sink_buffer.data == nullptr && current_.cells != nullptr &&
                sink_.ready != nullptr;
        if (spill) {
            current_.status = MotionVectorStatus::Ok;
            if (status_ == MotionVectorStatus::BufferUnavailable) {
                status_ = MotionVectorStatus::Ok;
            }
        }

        auto& header = current_header();
        header.status = static_cast<std::uint8_t>(current_.status);
        header.flags = current_.flags;
        header.long_term_frame_idx = current_.long_term
                                             ? static_cast<std::int8_t>(current_.long_term_frame_idx)
                                             : static_cast<std::int8_t>(-1);

        const bool reference = current_.reference;

        /*
         * Keep the current grid alive locally while the consumer inspects it.
         * Marking runs after the ready callback, matching the original
         * delivery contract: a reference picture enters the MV-DPB once the
         * consumer has been handed its buffer.
         */
        RefField entry;
        if (reference) {
            entry = make_current_field();
        }

        if ((current_.sink_buffer.data != nullptr || spill) && sink_.ready != nullptr) {
            if (sink_.options.defer_ready) {
                enqueuePendingOutput();
            } else {
                auto& header_now = current_header();
                writeSliceTables(header_now, current_.slice_refs, current_.slice_map,
                                 current_.slice_map_coverage);
                MotionVectorBufferView view{};
                view.header = &header_now;
                view.data = current_.sink_buffer.data;
                view.bytes = current_.required_bytes;
                view.lease = current_.sink_buffer.lease;
                view.parserRetains = reference;
                try {
                    sink_.ready(sink_.ctx, view);
                } catch (...) {
                    /* Consumer callbacks must not break the parser. */
                }
            }
        }

        if (reference) {
            applyMarking(entry);
        }

        finish_picture_bookkeeping();
        current_.clear();
    }

    /*
     * Finish a parse segment: flush the picture under construction and
     * deliver every deferred picture.  This is distinct from flush() because
     * a mid-stream flush can still be superseded by a later IDR carrying
     * no_output_of_prior_pics_flag == 1.
     */
    void finish() {
        flush();
        deliverPendingOutput();
    }

    /* Facade helpers for the multi-plane access-unit coordinator. */
    void deliverNextPendingPicture() { deliverOnePendingOutput(); }
    void discardPendingPictures() noexcept { discardPendingOutput(); }

    /* Diagnostic: number of completed pictures in the deferred queue. */
    [[nodiscard]] std::size_t deferredPictureCount() const noexcept {
        return pending_.size();
    }

   private:
    /*
     * One field of a decoded reference frame.  For a frame picture both
     * RefField objects alias the same frame grid and have coded_field=false.
     */
    struct RefField {
        bool present = false;
        bool long_term = false;
        /* Frame inferred by the frame_num gap process (H.264 8.2.5.2). */
        bool non_existing = false;
        std::uint32_t long_term_frame_idx = 0;
        std::int32_t poc = 0;
        bool bottom = false;
        bool coded_field = false;  // came from a field_pic_flag == 1 picture
        MotionVectorBufferLease lease;
        MotionGridPool::Lease owned_grid;
        const MvCell* cells = nullptr;
        std::uint32_t width4x4 = 0;
        std::uint32_t height4x4 = 0;
        std::array<std::int32_t, 32> ref_poc[2]{};
        std::uint8_t num_ref_idx[2]{0, 0};
        std::uint32_t non_existing_mask[2]{0, 0};
        /*
         * Per-slice reference table of this reference field.  Kept as an
         * owned copy so MotionReferencePicture pointers remain valid while
         * the extractor builds later reference lists.
         */
        std::vector<MotionSliceReference> slice_refs;
        /* Exact macroblock-to-slice map of the reference grid. */
        std::vector<MotionSliceRun> slice_map;
        std::uint32_t slice_map_coverage = 0;
        bool mbaff = false;

        void release() noexcept {
            present = false;
            lease.reset();
            owned_grid.reset();
            cells = nullptr;
            width4x4 = 0;
            height4x4 = 0;
            slice_refs.clear();
            slice_map.clear();
            slice_map_coverage = 0;
            mbaff = false;
        }
    };

    struct RefFrame {
        std::uint32_t frame_id = 0;
        std::uint32_t frame_num = 0;
        std::int32_t poc_top = 0;
        std::int32_t poc_bottom = 0;
        bool frame_picture = false;
        RefField top;
        RefField bottom;

        [[nodiscard]] bool hasAnyField() const noexcept {
            return top.present || bottom.present;
        }

        [[nodiscard]] bool hasBothFields() const noexcept {
            return top.present && bottom.present;
        }

        [[nodiscard]] bool shortTermGroup() const noexcept {
            return hasBothFields() && !top.long_term && !bottom.long_term;
        }

        [[nodiscard]] bool longTermGroup() const noexcept {
            return hasBothFields() && top.long_term && bottom.long_term;
        }

        [[nodiscard]] bool nonExisting() const noexcept {
            const bool top_missing = !top.present || top.non_existing;
            const bool bottom_missing = !bottom.present || bottom.non_existing;
            return hasAnyField() && top_missing && bottom_missing;
        }

        [[nodiscard]] std::int32_t poc() const noexcept {
            if (top.present && bottom.present) return std::min(top.poc, bottom.poc);
            if (top.present) return top.poc;
            if (bottom.present) return bottom.poc;
            return std::min(poc_top, poc_bottom);
        }
    };

    struct FieldRef {
        RefFrame* frame = nullptr;
        bool bottom = false;
    };

    struct GroupRef {
        RefFrame* frame = nullptr;
        std::int32_t poc = 0;
        std::int32_t frame_num_wrap = 0;
    };

    struct CurrentPicture {
        bool started = false;
        bool reference = false;
        bool idr = false;
        bool field_pic = false;
        bool bottom_field = false;
        bool mbaff = false;
        bool pair_second = false;
        bool adaptive_marking = false;
        bool long_term = false;
        bool has_inter_slice = false;
        /*
         * The per-picture acquire (or a later table-capacity grow) failed even
         * though the picture was eligible for a consumer buffer.  In deferred
         * mode the queue may have transiently exhausted the pool, so flush
         * spills this picture to the legacy cell-copy queue; drain can then
         * re-acquire after earlier queued buffers were released.
         */
        bool buffer_unavailable = false;
        std::uint32_t long_term_frame_idx = 0;
        std::uint32_t frame_id = 0;
        std::uint32_t frame_num = 0;
        std::uint32_t frame_num_max = 0;
        std::int32_t poc = 0;
        std::int32_t poc_top = 0;
        std::int32_t poc_bottom = 0;
        std::uint32_t width4x4 = 0;
        std::uint32_t height4x4 = 0;
        std::uint32_t mb_width = 0;
        std::uint32_t mb_height = 0;
        std::size_t required_bytes = 0;
        std::vector<detail::MbData> mbs;
        std::uint32_t next_slice_id = 0;
        MotionGridPool::Lease owned_grid;
        MotionVectorBuffer sink_buffer;
        MvCell* cells = nullptr;
        /* Used only if a mid-picture buffer growth cannot be satisfied. */
        std::vector<MvCell> fallback_cells;
        MotionVectorBufferHeader owned_header{};
        MotionVectorStatus status = MotionVectorStatus::Ok;
        std::uint32_t flags = 0;
        std::vector<MmcoOperation> mmco_operations;
        std::vector<MotionReferencePicture> list0;
        std::vector<MotionReferencePicture> list1;
        /*
         * Result of ref_pic_list_reordering/active-count processing for each
         * slice of this picture, in slice syntax order.
         */
        std::vector<MotionSliceReference> slice_refs;
        /*
         * Index in slice_refs (before finalizeSliceReferences sorts it) of the
         * slice that coded a given SliceDataParser slice_id.  Used to build
         * the exact mb_addr -> slice-ref map for FMO pictures.
         */
        std::vector<std::uint32_t> slice_ref_by_id;
        /*
         * Exact macroblock-address -> slice_refs index map, built once all
         * slices of the picture have been parsed.
         */
        std::vector<MotionSliceRun> slice_map;
        std::uint32_t slice_map_coverage = 0;
        std::uint32_t slice_map_capacity = 0;
        std::uint32_t slice_capacity = 0;
        /*
         * Frames synthesized from a PAFF field pair for frame-picture
         * reference lists.  Kept for the lifetime of the current picture so
         * MotionReferencePicture.cells stays valid.
         */
        std::vector<std::vector<MvCell>> merged_refs;
        std::vector<std::uint32_t> merged_ref_ids;

        /*
         * Reset the picture bookkeeping while keeping the macroblock array and
         * the small per-picture vectors allocated.  `mbs` is deliberately not
         * cleared: the next picture only needs `available` reset (parsed
         * macroblocks call MbData::reset() themselves), which avoids
         * re-initialising several megabytes per frame.
         */
        void clear() {
            started = false;
            reference = false;
            idr = false;
            field_pic = false;
            bottom_field = false;
            mbaff = false;
            pair_second = false;
            adaptive_marking = false;
            long_term = false;
            has_inter_slice = false;
            buffer_unavailable = false;
            long_term_frame_idx = 0;
            frame_id = 0;
            frame_num = 0;
            frame_num_max = 0;
            poc = 0;
            poc_top = 0;
            poc_bottom = 0;
            width4x4 = 0;
            height4x4 = 0;
            mb_width = 0;
            mb_height = 0;
            required_bytes = 0;
            next_slice_id = 0;
            owned_grid.reset();
            sink_buffer = MotionVectorBuffer{};
            cells = nullptr;
            fallback_cells.clear();
            owned_header = MotionVectorBufferHeader{};
            status = MotionVectorStatus::Ok;
            flags = 0;
            mmco_operations.clear();
            list0.clear();
            list1.clear();
            slice_refs.clear();
            slice_ref_by_id.clear();
            slice_map.clear();
            slice_map_coverage = 0;
            slice_map_capacity = 0;
            slice_capacity = 0;
            merged_refs.clear();
            merged_ref_ids.clear();
        }
    };

    /*
     * A completed picture waiting for the next IDR (or end of parsing) to
     * decide whether no_output_of_prior_pics_flag suppresses it.
     *
     * The normal path keeps the consumer buffer acquired while the picture was
     * parsed: the cells and slice tables were written into it exactly once and
     * the lease moves here, so draining only patches the header and calls
     * ready().  If the buffer cannot be grown to hold a multi-slice/FMO table,
     * the legacy fallback copies the grid out and re-acquires a buffer at
     * drain time.
     */
    struct PendingOutput {
        MotionVectorBuffer buffer;
        MotionVectorBufferHeader header{};
        std::vector<MotionSliceReference> slice_refs;
        std::vector<MotionSliceRun> slice_map;
        std::vector<MvCell> cells;
        std::size_t required_bytes = 0;
        bool reference = false;

        [[nodiscard]] bool zeroCopy() const noexcept { return buffer.data != nullptr; }
    };

    [[nodiscard]] MotionVectorBufferHeader& current_header() noexcept {
        if (current_.sink_buffer.data != nullptr) {
            return *reinterpret_cast<MotionVectorBufferHeader*>(current_.sink_buffer.data);
        }
        return current_.owned_header;
    }

    [[nodiscard]] static std::size_t align_up(std::size_t value, std::size_t alignment) noexcept {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    static constexpr std::uint32_t kInitialSliceCapacity = 8;

    [[nodiscard]] static std::size_t sliceTableOffset() noexcept {
        return align_up(sizeof(MotionVectorBufferHeader), alignof(MotionSliceReference));
    }

    [[nodiscard]] static std::size_t sliceMapOffsetFor(std::uint32_t slice_capacity) noexcept {
        return align_up(sliceTableOffset() +
                                static_cast<std::size_t>(slice_capacity) *
                                        sizeof(MotionSliceReference),
                        alignof(MotionSliceRun));
    }

    [[nodiscard]] static std::size_t cellsOffsetFor(
            std::uint32_t slice_capacity,
            std::uint32_t slice_map_capacity) noexcept {
        return align_up(sliceMapOffsetFor(slice_capacity) +
                                static_cast<std::size_t>(slice_map_capacity) *
                                        sizeof(MotionSliceRun),
                        64);
    }

    [[nodiscard]] static std::size_t requiredBytesFor(
            std::uint32_t slice_capacity,
            std::uint32_t slice_map_capacity,
            std::size_t cell_count) noexcept {
        return align_up(cellsOffsetFor(slice_capacity, slice_map_capacity) +
                                cell_count * sizeof(MvCell),
                        64);
    }

    [[nodiscard]] std::uint32_t currentMbCount() const noexcept {
        return current_.mb_width * current_.mb_height;
    }

    /*
     * Give every slice a macroblock-address range and sort the entries so a
     * cell coordinate can be resolved with a binary search.  The final range
     * of a slice ends where the next slice begins; conforming non-FMO
     * pictures present slices in increasing first_mb_in_slice order.
     *
     * Then build the exact macroblock map from the slice_id that the
     * macroblock parser stamped on every macroblock.  This is what makes a
     * non-contiguous FMO slice group resolvable per macroblock: the begin/end
     * ranges above only describe syntax order, while the map records the
     * actual slice of every coded macroblock.
     */
    void finalizeSliceReferences() {
        auto& refs = current_.slice_refs;
        const std::uint32_t mb_count = currentMbCount();

        /*
         * Resolve the slice-id -> slice-ref-index mapping while the table is
         * still in slice syntax order.  The scratch vectors live on the
         * extractor so a picture-sized map is allocated at most once.
         */
        std::vector<std::uint32_t>& per_mb = scratch_per_mb_;
        bool any_mapped = false;
        const bool want_map = refs.size() > 1;
        if (want_map && mb_count > 0 && !current_.mbs.empty()) {
            per_mb.assign(mb_count, kMotionSliceIndexNone);
            const std::size_t mb_entries = std::min<std::size_t>(mb_count, current_.mbs.size());
            for (std::size_t addr = 0; addr < mb_entries; ++addr) {
                const detail::MbData& mb = current_.mbs[addr];
                if (!mb.available ||
                    mb.slice_id >= current_.slice_ref_by_id.size()) {
                    continue;
                }
                const std::uint32_t index =
                        current_.slice_ref_by_id[mb.slice_id];
                if (index >= refs.size()) {
                    continue;
                }
                per_mb[addr] = index;
                any_mapped = true;
            }
        }

        for (MotionSliceReference& ref : refs) {
            if (ref.mb_addr_begin > mb_count) {
                ref.mb_addr_begin = mb_count;
            }
            ref.mb_addr_end = mb_count;
        }
        std::vector<std::uint32_t>& order = scratch_order_;
        order.resize(refs.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            order[i] = static_cast<std::uint32_t>(i);
        }
        std::stable_sort(order.begin(), order.end(),
                         [&refs](std::uint32_t a, std::uint32_t b) {
                             return refs[a].mb_addr_begin < refs[b].mb_addr_begin;
                         });
        std::vector<std::uint32_t>& old_to_new = scratch_old_to_new_;
        old_to_new.assign(refs.size(), 0);
        std::vector<MotionSliceReference>& sorted = scratch_sorted_;
        sorted.clear();
        sorted.reserve(refs.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            old_to_new[order[i]] = static_cast<std::uint32_t>(i);
            sorted.push_back(refs[order[i]]);
        }
        refs.swap(sorted);
        for (std::size_t i = 0; i < refs.size(); ++i) {
            const std::uint32_t end =
                    i + 1 < refs.size() ? refs[i + 1].mb_addr_begin : mb_count;
            refs[i].mb_addr_end = std::max(refs[i].mb_addr_begin, std::min(end, mb_count));
        }

        /*
         * Run-length encode the per-macroblock slice indices.  A contiguous
         * picture produces one run per slice; an FMO picture produces one run
         * per contiguous address span inside a slice group.  The map is
         * omitted when there is at most one slice because the begin/end range
         * alone is already exact.
         */
        current_.slice_map.clear();
        current_.slice_map_coverage = 0;
        if (any_mapped && refs.size() > 1) {
            current_.slice_map.reserve(mb_count);
            for (std::uint32_t& index : per_mb) {
                if (index != kMotionSliceIndexNone) {
                    index = old_to_new[index];
                }
            }
            std::uint32_t run_begin = 0;
            std::uint32_t run_index = per_mb[0];
            for (std::uint32_t addr = 1; addr < mb_count; ++addr) {
                if (per_mb[addr] != run_index) {
                    current_.slice_map.push_back(MotionSliceRun{run_begin, run_index});
                    run_begin = addr;
                    run_index = per_mb[addr];
                }
            }
            current_.slice_map.push_back(MotionSliceRun{run_begin, run_index});
            current_.slice_map_coverage = mb_count;
        }
        current_header().slice_map_coverage =
                current_.slice_map.empty() ? 0 : current_.slice_map_coverage;

        if (!refs.empty()) {
            auto& header = current_header();
            for (int list = 0; list < 2; ++list) {
                header.num_ref_idx[list] = refs[0].num_ref_idx[list];
                header.ref_non_existing_mask[list] = refs[0].non_existing_mask[list];
                for (std::size_t i = 0; i < 32; ++i) {
                    header.ref_poc[list][i] = refs[0].ref_poc[list][i];
                }
            }
        }

        bool has_non_existing = false;
        for (const MotionSliceReference& ref : refs) {
            if ((ref.flags & MvSliceHasNonExistingRef) != 0) {
                has_non_existing = true;
                break;
            }
        }
        if (has_non_existing) {
            current_.flags |= static_cast<std::uint32_t>(MvPictureHasNonExistingRef);
        }
    }

    void writeSliceTables(MotionVectorBufferHeader& header,
                          const std::vector<MotionSliceReference>& refs,
                          const std::vector<MotionSliceRun>& map,
                          std::uint32_t map_coverage) noexcept {
        const std::size_t count = std::min<std::size_t>(
                refs.size(), static_cast<std::size_t>(header.slice_ref_capacity));
        header.slice_ref_stride = sizeof(MotionSliceReference);
        header.slice_ref_count = static_cast<std::uint32_t>(count);
        const std::size_t map_count = std::min<std::size_t>(
                map.size(), static_cast<std::size_t>(header.slice_map_capacity));
        header.slice_map_stride = sizeof(MotionSliceRun);
        header.slice_map_count = static_cast<std::uint32_t>(map_count);
        header.slice_map_coverage = map_count > 0 ? map_coverage : 0;
        if (count < refs.size() || map_count < map.size()) {
            header.status = static_cast<std::uint8_t>(MotionVectorStatus::BufferUnavailable);
            current_.status = MotionVectorStatus::BufferUnavailable;
            status_ = MotionVectorStatus::BufferUnavailable;
        }
        if (count == 0 && map_count == 0) {
            return;
        }
        auto* base = static_cast<std::uint8_t*>(current_.sink_buffer.data);
        if (base == nullptr) {
            return;
        }
        if (count > 0) {
            auto* destination = reinterpret_cast<MotionSliceReference*>(
                    base + header.slice_refs_offset);
            std::copy_n(refs.begin(), count, destination);
        }
        if (map_count > 0) {
            auto* destination = reinterpret_cast<MotionSliceRun*>(
                    base + header.slice_map_offset);
            std::copy_n(map.begin(), map_count, destination);
        }
    }

    /*
     * Grow the consumer buffer when a multi-slice or FMO picture needs more
     * slice-reference or macroblock-map slots than the initial reservation.
     * The grid is copied because the old consumer slot may be reallocated by
     * the acquire callback; this path is rare and keeps the normal
     * single-slice path allocation-free.
     */
    bool ensureAuxCapacity(std::uint32_t needed_slices, std::uint32_t needed_map) {
        if (needed_slices <= current_.slice_capacity &&
            needed_map <= current_.slice_map_capacity) {
            return true;
        }
        if (current_.sink_buffer.data == nullptr || sink_.acquire == nullptr) {
            current_.slice_capacity = std::max(current_.slice_capacity, needed_slices);
            current_.slice_map_capacity =
                    std::max(current_.slice_map_capacity, needed_map);
            return true;
        }

        const std::uint32_t new_slice_capacity =
                needed_slices > current_.slice_capacity
                        ? std::max<std::uint32_t>(
                                  needed_slices,
                                  std::max<std::uint32_t>(
                                          1u, current_.slice_capacity * 2u))
                        : current_.slice_capacity;
        const std::uint32_t new_map_capacity =
                needed_map > current_.slice_map_capacity
                        ? std::max<std::uint32_t>(
                                  needed_map,
                                  std::max<std::uint32_t>(
                                          1u, current_.slice_map_capacity * 2u))
                        : current_.slice_map_capacity;
        const std::size_t cell_count = static_cast<std::size_t>(current_.width4x4) *
                                       current_.height4x4;
        std::vector<MvCell> saved(current_.cells, current_.cells + cell_count);
        const MotionVectorBufferHeader old_header = current_header();
        MotionVectorBufferLease old_lease = std::move(current_.sink_buffer.lease);
        current_.sink_buffer = MotionVectorBuffer{};

        MotionVectorBufferRequest request{};
        request.width4x4 = current_.width4x4;
        request.height4x4 = current_.height4x4;
        request.cell_count = static_cast<std::uint32_t>(cell_count);
        request.cell_stride = sizeof(MvCell);
        request.required_bytes =
                requiredBytesFor(new_slice_capacity, new_map_capacity, cell_count);
        request.alignment = sink_.options.buffer_alignment;
        request.poc = current_.poc;
        request.frame_num = current_.frame_num;
        request.flags = current_.flags;

        MotionVectorBuffer buffer = sink_.acquire(sink_.ctx, request);
        if ((buffer.data == nullptr || buffer.capacity < request.required_bytes) &&
            old_lease) {
            old_lease.reset();
            buffer = sink_.acquire(sink_.ctx, request);
        }
        if (buffer.data == nullptr || buffer.capacity < request.required_bytes) {
            old_lease.reset();
            /*
             * Keep the parser-owned grid valid: a reference picture may stay
             * in the MV-DPB after this picture is delivered, so pointing its
             * entry at CurrentPicture::fallback_cells would dangle when the
             * current picture resets.  Recycle the existing grid through the
             * object pool when possible.
             */
            current_.owned_grid.reset();
            MotionGridPool::Lease grid =
                    grid_pool_ != nullptr ? grid_pool_->tryAcquire()
                                          : MotionGridPool::Lease{};
            if (grid) {
                grid->cells = std::move(saved);
                current_.owned_grid = std::move(grid);
                current_.cells = current_.owned_grid->cells.data();
            } else {
                current_.fallback_cells = std::move(saved);
                current_.cells = current_.fallback_cells.data();
            }
            current_.slice_capacity = new_slice_capacity;
            current_.slice_map_capacity = new_map_capacity;
            current_.required_bytes =
                    requiredBytesFor(new_slice_capacity, new_map_capacity, cell_count);
            current_.status = MotionVectorStatus::BufferUnavailable;
            status_ = MotionVectorStatus::BufferUnavailable;
            return false;
        }

        auto* base = static_cast<std::uint8_t*>(buffer.data);
        current_.cells =
                reinterpret_cast<MvCell*>(base + cellsOffsetFor(new_slice_capacity,
                                                                new_map_capacity));
        std::copy(saved.begin(), saved.end(), current_.cells);
        current_.sink_buffer = std::move(buffer);
        current_.slice_capacity = new_slice_capacity;
        current_.slice_map_capacity = new_map_capacity;
        current_.required_bytes = request.required_bytes;

        MotionVectorBufferHeader header = old_header;
        header.header_bytes = sizeof(MotionVectorBufferHeader);
        header.total_bytes = static_cast<std::uint32_t>(current_.required_bytes);
        header.slice_ref_stride = sizeof(MotionSliceReference);
        header.slice_refs_offset = static_cast<std::uint32_t>(sliceTableOffset());
        header.slice_ref_capacity = new_slice_capacity;
        header.slice_ref_count = 0;
        header.slice_map_stride = sizeof(MotionSliceRun);
        header.slice_map_offset =
                static_cast<std::uint32_t>(sliceMapOffsetFor(new_slice_capacity));
        header.slice_map_capacity = new_map_capacity;
        header.slice_map_count = 0;
        header.slice_map_coverage = 0;
        header.cells_offset = static_cast<std::uint32_t>(
                cellsOffsetFor(new_slice_capacity, new_map_capacity));
        header.cell_count = static_cast<std::uint32_t>(cell_count);
        header.cell_stride = sizeof(MvCell);
        header.block_count = 0;
        header.block_stride = sizeof(MotionBlock);
        header.blocks_offset = 0;
        *reinterpret_cast<MotionVectorBufferHeader*>(base) = header;
        return true;
    }

    bool ensureSliceTableCapacity(std::size_t needed) {
        return ensureAuxCapacity(static_cast<std::uint32_t>(needed),
                                 current_.slice_map_capacity);
    }

    void enqueuePendingOutput() {
        PendingOutput output;
        output.header = current_header();
        output.required_bytes = current_.required_bytes;
        output.reference = current_.reference;

        /*
         * The buffer already holds the cells and (after the capacity check in
         * flush) room for the slice tables.  Write the tables once and move the
         * lease into the deferred queue; drain only patches the header.
         */
        if (current_.sink_buffer.data != nullptr) {
            auto& header = current_header();
            writeSliceTables(header, current_.slice_refs, current_.slice_map,
                             current_.slice_map_coverage);
            output.header = header;
            output.required_bytes = current_.required_bytes;
            output.buffer = std::move(current_.sink_buffer);
        } else {
            /*
             * BufferUnavailable fallback: the grid is parser-owned, so keep
             * the legacy copy and let drain re-acquire a consumer buffer in
             * queue order (earlier zero-copy entries release their slots
             * first).
             */
            output.slice_refs = current_.slice_refs;
            output.slice_map = current_.slice_map;
            if (current_.cells != nullptr) {
                output.cells.assign(
                        current_.cells,
                        current_.cells + static_cast<std::size_t>(current_.width4x4) *
                                                 current_.height4x4);
            }
        }
        pending_.push_back(std::move(output));

        const std::uint32_t limit = sink_.options.max_deferred_pictures;
        while (limit != 0 && pending_.size() > limit) {
            deliverOnePendingOutput();
        }
    }

    void deliverOnePendingOutput() {
        if (pending_.empty()) {
            return;
        }
        PendingOutput output = std::move(pending_.front());
        pending_.erase(pending_.begin());

        if (output.zeroCopy()) {
            /*
             * Cells and slice tables were written when the picture was
             * enqueued.  Only the final header fields are patched here; no
             * data is copied and no new buffer is acquired.
             */
            auto* base = static_cast<std::uint8_t*>(output.buffer.data);
            MotionVectorBufferHeader header = output.header;
            header.magic = kMotionVectorBufferMagic;
            header.version = kMotionVectorBufferVersion;
            header.header_bytes = sizeof(MotionVectorBufferHeader);
            header.total_bytes = static_cast<std::uint32_t>(output.required_bytes);
            *reinterpret_cast<MotionVectorBufferHeader*>(base) = header;

            MotionVectorBufferView view{};
            view.header = reinterpret_cast<const MotionVectorBufferHeader*>(base);
            view.data = base;
            view.bytes = output.required_bytes;
            view.lease = output.buffer.lease;
            /*
             * The buffer is the same allocation the MV-DPB references when
             * this is a reference picture, so the parser still retains it
             * after the callback; a non-reference picture releases its only
             * lease when `output` goes out of scope.
             */
            view.parserRetains = output.reference;
            try {
                sink_.ready(sink_.ctx, view);
            } catch (...) {
                /* Consumer callbacks must not break the parser. */
            }
            return;
        }

        const std::size_t cell_count = output.cells.size();
        const std::uint32_t slice_count =
                static_cast<std::uint32_t>(output.slice_refs.size());
        const std::uint32_t map_count =
                static_cast<std::uint32_t>(output.slice_map.size());
        const std::size_t table_offset = sliceTableOffset();
        const std::size_t map_offset = align_up(
                table_offset + static_cast<std::size_t>(slice_count) *
                                       sizeof(MotionSliceReference),
                alignof(MotionSliceRun));
        const std::size_t cells_offset = align_up(
                map_offset + static_cast<std::size_t>(map_count) *
                                     sizeof(MotionSliceRun),
                64);
        const std::size_t required = align_up(cells_offset + cell_count * sizeof(MvCell), 64);

        MotionVectorBufferRequest request{};
        request.width4x4 = output.header.width4x4;
        request.height4x4 = output.header.height4x4;
        request.cell_count = static_cast<std::uint32_t>(cell_count);
        request.cell_stride = sizeof(MvCell);
        request.required_bytes = required;
        request.alignment = sink_.options.buffer_alignment;
        request.poc = output.header.poc;
        request.frame_num = output.header.frame_num;
        request.flags = output.header.flags;
        request.colour_plane_id = output.header.colour_plane_id;
        request.plane_count = output.header.plane_count;

        MotionVectorBuffer buffer = sink_.acquire != nullptr
                                            ? sink_.acquire(sink_.ctx, request)
                                            : MotionVectorBuffer{};
        if (buffer.data == nullptr || buffer.capacity < required) {
            status_ = MotionVectorStatus::BufferUnavailable;
            return;
        }

        auto* base = static_cast<std::uint8_t*>(buffer.data);
        MotionVectorBufferHeader header = output.header;
        header.magic = kMotionVectorBufferMagic;
        header.version = kMotionVectorBufferVersion;
        header.header_bytes = sizeof(MotionVectorBufferHeader);
        header.total_bytes = static_cast<std::uint32_t>(required);
        header.cell_count = static_cast<std::uint32_t>(cell_count);
        header.cell_stride = sizeof(MvCell);
        header.cells_offset = static_cast<std::uint32_t>(cells_offset);
        header.block_count = 0;
        header.block_stride = sizeof(MotionBlock);
        header.blocks_offset = 0;
        header.slice_ref_count = slice_count;
        header.slice_ref_stride = sizeof(MotionSliceReference);
        header.slice_refs_offset = static_cast<std::uint32_t>(table_offset);
        header.slice_ref_capacity = slice_count;
        header.slice_map_count = map_count;
        header.slice_map_stride = sizeof(MotionSliceRun);
        header.slice_map_offset = static_cast<std::uint32_t>(map_offset);
        header.slice_map_capacity = map_count;
        header.slice_map_coverage = map_count > 0 ? output.header.slice_map_coverage : 0;
        *reinterpret_cast<MotionVectorBufferHeader*>(base) = header;

        if (slice_count > 0) {
            auto* slices = reinterpret_cast<MotionSliceReference*>(base + table_offset);
            std::copy(output.slice_refs.begin(), output.slice_refs.end(), slices);
        }
        if (map_count > 0) {
            auto* runs = reinterpret_cast<MotionSliceRun*>(base + map_offset);
            std::copy(output.slice_map.begin(), output.slice_map.end(), runs);
        }
        auto* cells = reinterpret_cast<MvCell*>(base + cells_offset);
        std::copy(output.cells.begin(), output.cells.end(), cells);

        MotionVectorBufferView view{};
        view.header = reinterpret_cast<const MotionVectorBufferHeader*>(base);
        view.data = base;
        view.bytes = required;
        view.lease = buffer.lease;
        /*
         * The delivered buffer is a fresh copy; the parser does not retain
         * this particular allocation.  Reference pictures remain in the
         * MV-DPB through their own original lease.
         */
        view.parserRetains = false;
        try {
            sink_.ready(sink_.ctx, view);
        } catch (...) {
            /* Consumer callbacks must not break the parser. */
        }
    }

    void deliverPendingOutput() {
        while (!pending_.empty()) {
            deliverOnePendingOutput();
        }
    }

    void discardPendingOutput() noexcept { pending_.clear(); }

    /*
     * Drop the picture currently being accumulated without delivering it.
     * Marking is intentionally skipped: this is only used immediately before
     * an IDR with no_output_of_prior_pics_flag == 1, whose own marking clears
     * the DPB.
     */
    void dropCurrentWithoutOutput() {
        if (!current_.started) {
            return;
        }
        finish_picture_bookkeeping();
        current_.clear();
    }

    [[nodiscard]] std::uint32_t maxReferenceFrames() const noexcept {
        return std::max<std::uint32_t>(1, sps_max_refs_);
    }

    [[nodiscard]] std::uint32_t maxFrameNum() const noexcept {
        return current_.frame_num_max != 0 ? current_.frame_num_max : 1u;
    }

    [[nodiscard]] std::int32_t frameNumWrap(std::uint32_t frame_num) const noexcept {
        const std::uint32_t max_frame_num = maxFrameNum();
        if (frame_num > current_.frame_num) {
            return static_cast<std::int32_t>(frame_num - max_frame_num);
        }
        return static_cast<std::int32_t>(frame_num);
    }

    [[nodiscard]] std::int32_t framePicNum(const RefFrame& frame) const noexcept {
        return frameNumWrap(frame.frame_num);
    }

    [[nodiscard]] std::int32_t fieldPicNum(
            const RefFrame& frame, bool bottom) const noexcept {
        const std::int32_t wrap = frameNumWrap(frame.frame_num);
        const bool same_parity = bottom == current_.bottom_field;
        return 2 * wrap + (same_parity ? 1 : 0);
    }

    [[nodiscard]] std::int32_t fieldLongTermPicNum(
            const RefField& field, bool bottom) const noexcept {
        const bool same_parity = bottom == current_.bottom_field;
        return 2 * static_cast<std::int32_t>(field.long_term_frame_idx) +
               (same_parity ? 1 : 0);
    }

    [[nodiscard]] RefFrame* findFrameById(std::uint32_t frame_id) noexcept {
        for (RefFrame& frame : refs_) {
            if (frame.frame_id == frame_id) return &frame;
        }
        return nullptr;
    }

    void pruneFrames() {
        refs_.erase(
                std::remove_if(refs_.begin(), refs_.end(), [](const RefFrame& frame) {
                    return !frame.hasAnyField();
                }),
                refs_.end());
    }

    void clearFrame(RefFrame& frame) noexcept {
        frame.top.release();
        frame.bottom.release();
    }

    void releaseField(RefFrame& frame, bool bottom) noexcept {
        if (bottom) {
            frame.bottom.release();
        } else {
            frame.top.release();
        }
    }

    [[nodiscard]] RefField* fieldAt(RefFrame& frame, bool bottom) noexcept {
        return bottom ? &frame.bottom : &frame.top;
    }

    /*
     * IDR and adaptive marking resolve a LongTermFrameIdx that may already be
     * in use.  A whole frame/pair is removed when both fields carry it; an
     * individual field is removed for a non-paired/mixed field except when it
     * belongs to the picture being (re)assigned.
     */
    void removeLongTermConflicts(
            std::uint32_t long_term_frame_idx,
            const RefFrame* keep_frame) {
        /*
         * Identify the picture being (re)assigned by frame_id, not by
         * address: erasing an earlier vector element invalidates pointers.
         */
        const std::uint32_t keep_frame_id = keep_frame != nullptr ? keep_frame->frame_id : 0;
        for (auto it = refs_.begin(); it != refs_.end();) {
            RefFrame& frame = *it;
            if (keep_frame != nullptr && frame.frame_id == keep_frame_id) {
                ++it;
                continue;
            }
            const bool pair_conflict =
                    frame.hasBothFields() && frame.top.long_term && frame.bottom.long_term &&
                    frame.top.long_term_frame_idx == long_term_frame_idx &&
                    frame.bottom.long_term_frame_idx == long_term_frame_idx;
            if (pair_conflict) {
                clearFrame(frame);
            } else {
                for (int bottom = 0; bottom < 2; ++bottom) {
                    RefField& field = *fieldAt(frame, bottom != 0);
                    if (field.present && field.long_term &&
                        field.long_term_frame_idx == long_term_frame_idx) {
                        field.release();
                    }
                }
            }
            if (!frame.hasAnyField()) {
                it = refs_.erase(it);
            } else {
                ++it;
            }
        }
    }

    [[nodiscard]] RefFrame* findShortTermFrame(std::int32_t pic_num) noexcept {
        for (RefFrame& frame : refs_) {
            if (frame.shortTermGroup() &&
                framePicNum(frame) == pic_num) {
                return &frame;
            }
        }
        return nullptr;
    }

    [[nodiscard]] FieldRef findShortTermField(std::int32_t pic_num) noexcept {
        for (RefFrame& frame : refs_) {
            for (int bottom = 0; bottom < 2; ++bottom) {
                RefField& field = *fieldAt(frame, bottom != 0);
                if (field.present && !field.long_term &&
                    fieldPicNum(frame, bottom != 0) == pic_num) {
                    return FieldRef{&frame, bottom != 0};
                }
            }
        }
        return FieldRef{};
    }

    [[nodiscard]] RefFrame* findLongTermFrame(std::uint32_t long_term_pic_num) noexcept {
        for (RefFrame& frame : refs_) {
            if (frame.longTermGroup() &&
                frame.top.long_term_frame_idx == long_term_pic_num &&
                frame.bottom.long_term_frame_idx == long_term_pic_num) {
                return &frame;
            }
        }
        return nullptr;
    }

    [[nodiscard]] FieldRef findLongTermField(std::uint32_t long_term_pic_num) noexcept {
        for (RefFrame& frame : refs_) {
            for (int bottom = 0; bottom < 2; ++bottom) {
                RefField& field = *fieldAt(frame, bottom != 0);
                if (field.present && field.long_term &&
                    fieldLongTermPicNum(field, bottom != 0) ==
                            static_cast<std::int32_t>(long_term_pic_num)) {
                    return FieldRef{&frame, bottom != 0};
                }
            }
        }
        return FieldRef{};
    }

    void assignLongTerm(RefFrame& frame, bool bottom, std::uint32_t long_term_frame_idx,
                        bool whole_frame) {
        if (whole_frame) {
            frame.top.long_term = true;
            frame.top.long_term_frame_idx = long_term_frame_idx;
            frame.bottom.long_term = true;
            frame.bottom.long_term_frame_idx = long_term_frame_idx;
            return;
        }
        RefField& target = *fieldAt(frame, bottom);
        target.long_term = true;
        target.long_term_frame_idx = long_term_frame_idx;
        RefField& other = *fieldAt(frame, !bottom);
        if (other.present && other.long_term) {
            other.long_term_frame_idx = long_term_frame_idx;
        }
    }

    /*
     * 8.2.5.4.1/8.2.5.4.2: mark the reference field identified by PicNum or
     * LongTermPicNum unused.  For a field picture only that field is marked;
     * when it belongs to a frame/pair the other field keeps its marking.
     */
    void markFieldUnusedByPicNum(std::int32_t pic_num) {
        const FieldRef target = findShortTermField(pic_num);
        if (target.frame != nullptr) {
            releaseField(*target.frame, target.bottom);
            pruneFrames();
        }
    }

    void markFieldUnusedByLongTermNum(std::uint32_t long_term_pic_num) {
        const FieldRef target = findLongTermField(long_term_pic_num);
        if (target.frame != nullptr) {
            releaseField(*target.frame, target.bottom);
            pruneFrames();
        }
    }

    void markFrameUnusedByPicNum(std::int32_t pic_num) {
        RefFrame* target = findShortTermFrame(pic_num);
        if (target != nullptr) {
            clearFrame(*target);
            pruneFrames();
        }
    }

    void markFrameUnusedByLongTermNum(std::uint32_t long_term_pic_num) {
        RefFrame* target = findLongTermFrame(long_term_pic_num);
        if (target != nullptr) {
            clearFrame(*target);
            pruneFrames();
        }
    }

    /*
     * 8.2.5.4.3: assign a long-term index to a short-term frame/field.  The
     * target is identified by picNumX = CurrPicNum - (difference + 1).
     */
    void assignLongTermByPicNum(std::int32_t pic_num, std::uint32_t long_term_frame_idx) {
        if (current_.field_pic) {
            const FieldRef target = findShortTermField(pic_num);
            if (target.frame == nullptr) return;
            const std::uint32_t target_frame_id = target.frame->frame_id;
            const bool target_bottom = target.bottom;
            removeLongTermConflicts(long_term_frame_idx, target.frame);
            RefFrame* reassigned = findFrameById(target_frame_id);
            if (reassigned != nullptr) {
                assignLongTerm(*reassigned, target_bottom, long_term_frame_idx, false);
            }
            return;
        }
        RefFrame* target = findShortTermFrame(pic_num);
        if (target == nullptr) return;
        const std::uint32_t target_frame_id = target->frame_id;
        removeLongTermConflicts(long_term_frame_idx, target);
        RefFrame* reassigned = findFrameById(target_frame_id);
        if (reassigned != nullptr) {
            assignLongTerm(*reassigned, false, long_term_frame_idx, true);
        }
    }

    /* 8.2.5.4.4: update MaxLongTermFrameIdx and drop indices above it. */
    void applyMaxLongTermFrameIdx(std::uint32_t max_long_term_frame_idx_plus1) {
        max_long_term_frame_idx_ = max_long_term_frame_idx_plus1 == 0
                                          ? -1
                                          : static_cast<int>(max_long_term_frame_idx_plus1) - 1;
        for (auto it = refs_.begin(); it != refs_.end();) {
            RefFrame& frame = *it;
            if (frame.hasBothFields() && frame.top.long_term && frame.bottom.long_term &&
                frame.top.long_term_frame_idx == frame.bottom.long_term_frame_idx &&
                static_cast<int>(frame.top.long_term_frame_idx) > max_long_term_frame_idx_) {
                clearFrame(frame);
            } else {
                for (int bottom = 0; bottom < 2; ++bottom) {
                    RefField& field = *fieldAt(frame, bottom != 0);
                    if (field.present && field.long_term &&
                        static_cast<int>(field.long_term_frame_idx) >
                                max_long_term_frame_idx_) {
                        field.release();
                    }
                }
            }
            if (!frame.hasAnyField()) {
                it = refs_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void clearAllReferences() {
        refs_.clear();
        max_long_term_frame_idx_ = -1;
    }

    /* 8.2.5.4: adaptive memory control operations 1-6. */
    void applyMmcoOperations() {
        for (const MmcoOperation& op : current_.mmco_operations) {
            const std::uint32_t operation = op.memory_management_control_operation;
            if (operation == 0) break;

            const std::int32_t curr_pic_num = current_.field_pic
                                                      ? 2 * static_cast<std::int32_t>(current_.frame_num) + 1
                                                      : static_cast<std::int32_t>(current_.frame_num);
            const std::int32_t pic_num_x =
                    curr_pic_num - static_cast<std::int32_t>(op.difference_of_pic_nums_minus1 + 1);

            switch (operation) {
                case 1:
                    if (current_.field_pic) {
                        markFieldUnusedByPicNum(pic_num_x);
                    } else {
                        markFrameUnusedByPicNum(pic_num_x);
                    }
                    break;
                case 2:
                    if (current_.field_pic) {
                        markFieldUnusedByLongTermNum(op.long_term_pic_num);
                    } else {
                        markFrameUnusedByLongTermNum(op.long_term_pic_num);
                    }
                    break;
                case 3:
                    assignLongTermByPicNum(pic_num_x, op.long_term_frame_idx);
                    break;
                case 4:
                    applyMaxLongTermFrameIdx(op.max_long_term_frame_idx_plus1);
                    break;
                case 5:
                    clearAllReferences();
                    break;
                case 6: {
                    RefFrame* keep = findFrameById(current_.frame_id);
                    removeLongTermConflicts(op.long_term_frame_idx, keep);
                    break;
                }
                default:
                    break;
            }
        }
    }

    [[nodiscard]] std::size_t referenceFrameCount() const noexcept { return refs_.size(); }

    /*
     * 8.2.5.3: remove the short-term frame/pair/non-paired field with the
     * smallest FrameNumWrap.  With no short-term entries the long-term entry
     * with the largest LongTermFrameIdx is removed instead.
     */
    bool removeOldestReference() {
        RefFrame* oldest = nullptr;
        std::int32_t oldest_wrap = std::numeric_limits<std::int32_t>::max();
        for (RefFrame& frame : refs_) {
            const bool short_term = (frame.top.present && !frame.top.long_term) ||
                                    (frame.bottom.present && !frame.bottom.long_term);
            if (!short_term) continue;
            const std::int32_t wrap = frameNumWrap(frame.frame_num);
            if (oldest == nullptr || wrap < oldest_wrap) {
                oldest = &frame;
                oldest_wrap = wrap;
            }
        }
        if (oldest != nullptr) {
            clearFrame(*oldest);
            pruneFrames();
            return true;
        }

        RefFrame* largest = nullptr;
        std::uint32_t largest_idx = 0;
        for (RefFrame& frame : refs_) {
            std::uint32_t idx = 0;
            bool has_long_term = false;
            for (int bottom = 0; bottom < 2; ++bottom) {
                RefField& field = *fieldAt(frame, bottom != 0);
                if (field.present && field.long_term) {
                    idx = has_long_term ? std::max(idx, field.long_term_frame_idx)
                                        : field.long_term_frame_idx;
                    has_long_term = true;
                }
            }
            if (!has_long_term) continue;
            if (largest == nullptr || idx > largest_idx) {
                largest = &frame;
                largest_idx = idx;
            }
        }
        if (largest != nullptr) {
            clearFrame(*largest);
            pruneFrames();
            return true;
        }
        return false;
    }

    [[nodiscard]] RefField make_current_field() {
        RefField entry;
        entry.present = true;
        entry.long_term = current_.long_term;
        entry.non_existing = false;
        entry.long_term_frame_idx = current_.long_term_frame_idx;
        entry.poc = current_.field_pic ? current_.poc : current_.poc_top;
        entry.bottom = current_.bottom_field;
        entry.coded_field = current_.field_pic;
        entry.lease = current_.sink_buffer.lease;
        entry.owned_grid = std::move(current_.owned_grid);
        entry.cells = current_.cells;
        entry.width4x4 = current_.width4x4;
        entry.height4x4 = current_.height4x4;
        entry.slice_refs = current_.slice_refs;
        entry.slice_map = current_.slice_map;
        entry.slice_map_coverage = current_.slice_map_coverage;
        entry.mbaff = current_.mbaff;

        auto& header = current_header();
        for (int list = 0; list < 2; ++list) {
            entry.num_ref_idx[list] = header.num_ref_idx[list];
            entry.non_existing_mask[list] = header.ref_non_existing_mask[list];
            for (std::size_t i = 0; i < 32; ++i) {
                entry.ref_poc[list][i] = header.ref_poc[list][i];
            }
        }
        return entry;
    }

    void addReferenceField(RefField entry) {
        const bool assigned_long_term = entry.long_term;
        const std::uint32_t assigned_long_term_idx = entry.long_term_frame_idx;
        RefFrame* frame = findFrameById(current_.frame_id);
        if (frame == nullptr) {
            RefFrame created;
            created.frame_id = current_.frame_id;
            created.frame_num = current_.frame_num;
            created.poc_top = current_.poc_top;
            created.poc_bottom = current_.poc_bottom;
            created.frame_picture = !current_.field_pic;

            if (created.frame_picture) {
                created.top = entry;
                created.top.poc = current_.poc_top;
                created.top.bottom = false;
                created.top.coded_field = false;

                created.bottom = entry;
                created.bottom.poc = current_.poc_bottom;
                created.bottom.bottom = true;
                created.bottom.coded_field = false;
            } else if (entry.bottom) {
                created.bottom = std::move(entry);
            } else {
                created.top = std::move(entry);
            }
            refs_.push_back(std::move(created));
            return;
        }

        if (current_.field_pic) {
            if (entry.bottom) {
                frame->bottom = std::move(entry);
                frame->poc_bottom = current_.poc_bottom;
            } else {
                frame->top = std::move(entry);
                frame->poc_top = current_.poc_top;
            }
        } else {
            frame->frame_picture = true;
            frame->top = entry;
            frame->top.poc = current_.poc_top;
            frame->top.bottom = false;
            frame->top.coded_field = false;
            frame->bottom = entry;
            frame->bottom.poc = current_.poc_bottom;
            frame->bottom.bottom = true;
            frame->bottom.coded_field = false;
        }

        /*
         * A long-term assignment on the second field of a pair promotes the
         * pair to a long-term pair when the first field is long-term too.
         */
        if (assigned_long_term) {
            for (int bottom = 0; bottom < 2; ++bottom) {
                RefField& field = *fieldAt(*frame, bottom != 0);
                if (field.present && field.long_term) {
                    field.long_term_frame_idx = assigned_long_term_idx;
                }
            }
        }
    }

    void applyMarking(RefField& entry) {
        const bool second_field_pair =
                current_.field_pic && current_.bottom_field && current_.pair_second;
        RefFrame* pair = second_field_pair ? findFrameById(current_.frame_id) : nullptr;
        const bool pair_has_top = pair != nullptr && pair->top.present;
        const bool pair_top_short = pair_has_top && !pair->top.long_term;

        if (current_.idr) {
            /*
             * An IDR picture clears the DPB.  For the bottom field of an IDR
             * complementary pair the top field is part of the same picture
             * and is retained.  no_output_of_prior_pics_flag affects display
             * output only; this extractor already delivered the prior grids,
             * so the MV-DPB behaviour is the same either way.
             */
            if (!pair_has_top) {
                clearAllReferences();
            }
            max_long_term_frame_idx_ = current_.long_term ? 0 : -1;
            entry.long_term = current_.long_term;
            entry.long_term_frame_idx = 0;
            addReferenceField(std::move(entry));
            return;
        }

        if (current_.adaptive_marking) {
            applyMmcoOperations();
            addReferenceField(std::move(entry));
            return;
        }

        /*
         * Sliding-window marking.  The second field of a complementary pair
         * simply joins the first field and does not consume another frame
         * slot (8.2.5.3).
         */
        if (pair_top_short) {
            entry.long_term = false;
            addReferenceField(std::move(entry));
            return;
        }

        while (referenceFrameCount() >= maxReferenceFrames()) {
            if (!removeOldestReference()) break;
        }
        entry.long_term = false;
        addReferenceField(std::move(entry));
    }

    void finish_picture_bookkeeping() noexcept {
        last_picture_valid_ = true;
        last_picture_field_ = current_.field_pic;
        last_picture_bottom_ = current_.bottom_field;
        last_picture_frame_num_ = current_.frame_num;
        last_frame_id_ = current_.frame_id;

        if (current_.reference) {
            prev_ref_frame_num_ = current_.frame_num;
            prev_ref_frame_num_valid_ = true;
        }
    }

    [[nodiscard]] MotionReferencePicture toFieldView(RefFrame& frame, bool bottom) noexcept {
        RefField& field = *fieldAt(frame, bottom ? true : false);
        MotionReferencePicture view{};
        view.poc = field.poc;
        view.cells = field.cells;
        view.width4x4 = field.width4x4;
        view.height4x4 = field.height4x4;
        view.frame_num = frame.frame_num;
        view.field_pic = field.coded_field;
        view.bottom_field = bottom;
        view.non_existing = field.non_existing;
        view.num_ref_idx[0] = field.num_ref_idx[0];
        view.num_ref_idx[1] = field.num_ref_idx[1];
        for (int list = 0; list < 2; ++list) {
            view.non_existing_mask[list] = field.non_existing_mask[list];
            for (std::size_t i = 0; i < 32; ++i) {
                view.ref_poc[list][i] = field.ref_poc[list][i];
            }
        }
        view.slice_refs = field.slice_refs.data();
        view.slice_ref_count = static_cast<std::uint32_t>(field.slice_refs.size());
        view.slice_map = field.slice_map.data();
        view.slice_map_count = static_cast<std::uint32_t>(field.slice_map.size());
        view.slice_map_coverage = field.slice_map_coverage;
        view.mbaff = field.mbaff;
        return view;
    }

    /*
     * A frame-picture reference list entry.  A PAFF pair has no frame grid of
     * its own, so its field grids are interleaved into an owned frame grid
     * (vertical field motion is normalised to frame luma lines, matching the
     * MBAFF convention used by the public MV model).
     */
    [[nodiscard]] MotionReferencePicture toFrameView(RefFrame& frame) {
        MotionReferencePicture view{};
        if (frame.frame_picture) {
            RefField& field = frame.top.present ? frame.top : frame.bottom;
            view.poc = frame.poc();
            view.cells = field.cells;
            view.width4x4 = field.width4x4;
            view.height4x4 = field.height4x4;
            view.frame_num = frame.frame_num;
            view.field_pic = false;
            view.bottom_field = false;
            view.non_existing = frame.nonExisting();
            view.num_ref_idx[0] = field.num_ref_idx[0];
            view.num_ref_idx[1] = field.num_ref_idx[1];
            for (int list = 0; list < 2; ++list) {
                view.non_existing_mask[list] = field.non_existing_mask[list];
                for (std::size_t i = 0; i < 32; ++i) {
                    view.ref_poc[list][i] = field.ref_poc[list][i];
                }
            }
            view.slice_refs = field.slice_refs.data();
            view.slice_ref_count = static_cast<std::uint32_t>(field.slice_refs.size());
            view.slice_map = field.slice_map.data();
            view.slice_map_count = static_cast<std::uint32_t>(field.slice_map.size());
            view.slice_map_coverage = field.slice_map_coverage;
            view.mbaff = field.mbaff;
            return view;
        }

        for (std::size_t i = 0; i < current_.merged_ref_ids.size(); ++i) {
            if (current_.merged_ref_ids[i] == frame.frame_id) {
                RefField& field = frame.top.present ? frame.top : frame.bottom;
                view.poc = frame.poc();
                view.cells = current_.merged_refs[i].data();
                view.width4x4 = field.width4x4;
                view.height4x4 = field.height4x4 * 2;
                view.frame_num = frame.frame_num;
                view.field_pic = false;
                view.bottom_field = false;
                view.non_existing = frame.nonExisting();
                view.interleaved_fields = true;
                view.field_slice_refs[0] = frame.top.slice_refs.data();
                view.field_slice_ref_count[0] =
                        static_cast<std::uint32_t>(frame.top.slice_refs.size());
                view.field_slice_refs[1] = frame.bottom.slice_refs.data();
                view.field_slice_ref_count[1] =
                        static_cast<std::uint32_t>(frame.bottom.slice_refs.size());
                view.field_slice_map[0] = frame.top.slice_map.data();
                view.field_slice_map_count[0] =
                        static_cast<std::uint32_t>(frame.top.slice_map.size());
                view.field_slice_map_coverage[0] = frame.top.slice_map_coverage;
                view.field_slice_map[1] = frame.bottom.slice_map.data();
                view.field_slice_map_count[1] =
                        static_cast<std::uint32_t>(frame.bottom.slice_map.size());
                view.field_slice_map_coverage[1] = frame.bottom.slice_map_coverage;
                view.num_ref_idx[0] = field.num_ref_idx[0];
                view.num_ref_idx[1] = field.num_ref_idx[1];
                for (int list = 0; list < 2; ++list) {
                    view.non_existing_mask[list] = field.non_existing_mask[list];
                    for (std::size_t k = 0; k < 32; ++k) {
                        view.ref_poc[list][k] = field.ref_poc[list][k];
                    }
                }
                view.slice_refs = field.slice_refs.data();
                view.slice_ref_count = static_cast<std::uint32_t>(field.slice_refs.size());
                return view;
            }
        }

        RefField& top = frame.top;
        RefField& bottom = frame.bottom;
        const std::uint32_t width = top.present ? top.width4x4 : bottom.width4x4;
        const std::uint32_t height = top.present ? top.height4x4 : bottom.height4x4;
        std::vector<MvCell> merged(static_cast<std::size_t>(width) * height * 2u);

        for (std::uint32_t y = 0; y < height * 2u; ++y) {
            const bool bottom_row = (y & 1u) != 0;
            RefField& source = bottom_row ? bottom : top;
            for (std::uint32_t x = 0; x < width; ++x) {
                MvCell& dst = merged[static_cast<std::size_t>(y) * width + x];
                if (!source.present || source.cells == nullptr ||
                    x >= source.width4x4 || (y / 2u) >= source.height4x4) {
                    dst = MvCell{};
                    dst.flags = MvUnavailable;
                    continue;
                }
                dst = source.cells[static_cast<std::size_t>(y / 2u) * source.width4x4 + x];
                for (int list = 0; list < 2; ++list) {
                    const long scaled = static_cast<long>(dst.mv[list][1]) * 2L;
                    dst.mv[list][1] = static_cast<std::int16_t>(std::clamp(
                            scaled,
                            static_cast<long>(std::numeric_limits<std::int16_t>::min()),
                            static_cast<long>(std::numeric_limits<std::int16_t>::max())));
                }
                dst.flags |= MvField;
            }
        }

        current_.merged_ref_ids.push_back(frame.frame_id);
        current_.merged_refs.push_back(std::move(merged));

        RefField& field = top.present ? top : bottom;
        view.poc = frame.poc();
        view.cells = current_.merged_refs.back().data();
        view.width4x4 = width;
        view.height4x4 = height * 2;
        view.frame_num = frame.frame_num;
        view.field_pic = false;
        view.bottom_field = false;
        view.non_existing = frame.nonExisting();
        view.interleaved_fields = true;
        view.field_slice_refs[0] = top.slice_refs.data();
        view.field_slice_ref_count[0] = static_cast<std::uint32_t>(top.slice_refs.size());
        view.field_slice_refs[1] = bottom.slice_refs.data();
        view.field_slice_ref_count[1] = static_cast<std::uint32_t>(bottom.slice_refs.size());
        view.field_slice_map[0] = top.slice_map.data();
        view.field_slice_map_count[0] = static_cast<std::uint32_t>(top.slice_map.size());
        view.field_slice_map_coverage[0] = top.slice_map_coverage;
        view.field_slice_map[1] = bottom.slice_map.data();
        view.field_slice_map_count[1] = static_cast<std::uint32_t>(bottom.slice_map.size());
        view.field_slice_map_coverage[1] = bottom.slice_map_coverage;
        view.num_ref_idx[0] = field.num_ref_idx[0];
        view.num_ref_idx[1] = field.num_ref_idx[1];
        for (int list = 0; list < 2; ++list) {
            view.non_existing_mask[list] = field.non_existing_mask[list];
            for (std::size_t i = 0; i < 32; ++i) {
                view.ref_poc[list][i] = field.ref_poc[list][i];
            }
        }
        view.slice_refs = field.slice_refs.data();
        view.slice_ref_count = static_cast<std::uint32_t>(field.slice_refs.size());
        view.slice_map = field.slice_map.data();
        view.slice_map_count = static_cast<std::uint32_t>(field.slice_map.size());
        view.slice_map_coverage = field.slice_map_coverage;
        view.mbaff = field.mbaff;
        return view;
    }

    [[nodiscard]] static bool sameReference(
            const MotionReferencePicture& a, const MotionReferencePicture& b) noexcept {
        return a.cells == b.cells && a.poc == b.poc && a.frame_num == b.frame_num &&
               a.field_pic == b.field_pic && a.bottom_field == b.bottom_field &&
               a.non_existing == b.non_existing;
    }

    void insertReordered(
            std::vector<MotionReferencePicture>& list,
            const MotionReferencePicture& view,
            std::size_t index) {
        if (view.cells == nullptr && !view.non_existing) return;
        const auto end = std::remove_if(
                list.begin(), list.end(),
                [&](const MotionReferencePicture& ref) { return sameReference(ref, view); });
        list.erase(end, list.end());
        if (index > list.size()) index = list.size();
        list.insert(list.begin() + static_cast<std::ptrdiff_t>(index), view);
    }

    /*
     * 8.2.4.3.1/8.2.4.3.2 reference-list reordering.  Frame lists address
     * frames/pairs by PicNum, field lists address individual fields by their
     * parity-dependent PicNum/LongTermPicNum.
     */
    void applyListModification(
            std::vector<MotionReferencePicture>& list,
            const RefPicListModification& modification,
            std::uint32_t max_pic_num,
            std::uint32_t curr_pic_num,
            bool field_list) {
        if (!modification.modification_flag || modification.ops.empty()) return;

        std::int64_t predicted_pic_num = static_cast<std::int64_t>(curr_pic_num);
        std::size_t index = 0;

        for (const RefPicReorderingOp& op : modification.ops) {
            if (op.reordering_of_pic_nums_idc == 3) break;

            if (op.reordering_of_pic_nums_idc == 0 ||
                op.reordering_of_pic_nums_idc == 1) {
                const std::int64_t difference =
                        static_cast<std::int64_t>(op.abs_diff_pic_num_minus1) + 1;
                std::int64_t no_wrap = predicted_pic_num;
                if (op.reordering_of_pic_nums_idc == 0) {
                    no_wrap -= difference;
                    if (no_wrap < 0) no_wrap += max_pic_num;
                } else {
                    no_wrap += difference;
                    if (no_wrap >= static_cast<std::int64_t>(max_pic_num)) {
                        no_wrap -= max_pic_num;
                    }
                }
                predicted_pic_num = no_wrap;

                const std::int64_t pic_num =
                        no_wrap > static_cast<std::int64_t>(curr_pic_num)
                                ? no_wrap - static_cast<std::int64_t>(max_pic_num)
                                : no_wrap;

                MotionReferencePicture view{};
                if (field_list) {
                    const FieldRef target =
                            findShortTermField(static_cast<std::int32_t>(pic_num));
                    if (target.frame != nullptr) {
                        view = toFieldView(*target.frame, target.bottom);
                    }
                } else {
                    RefFrame* target =
                            findShortTermFrame(static_cast<std::int32_t>(pic_num));
                    if (target != nullptr) {
                        view = toFrameView(*target);
                    }
                }
                if (view.cells != nullptr || view.non_existing) {
                    insertReordered(list, view, index);
                    ++index;
                }

            } else if (op.reordering_of_pic_nums_idc == 2) {
                MotionReferencePicture view{};
                if (field_list) {
                    const FieldRef target = findLongTermField(op.long_term_pic_num);
                    if (target.frame != nullptr) {
                        view = toFieldView(*target.frame, target.bottom);
                    }
                } else {
                    RefFrame* target = findLongTermFrame(op.long_term_pic_num);
                    if (target != nullptr) {
                        view = toFrameView(*target);
                    }
                }
                if (view.cells != nullptr || view.non_existing) {
                    insertReordered(list, view, index);
                    ++index;
                }
            }
        }
    }

    void collectFrameGroups(
            std::vector<GroupRef>& short_term,
            std::vector<GroupRef>& long_term) {
        for (RefFrame& frame : refs_) {
            if (!frame.hasBothFields()) continue;
            if (frame.top.long_term != frame.bottom.long_term) continue;
            GroupRef ref{&frame, frame.poc(), frameNumWrap(frame.frame_num)};
            if (frame.top.long_term) {
                if (frame.top.long_term_frame_idx == frame.bottom.long_term_frame_idx) {
                    long_term.push_back(ref);
                }
            } else {
                short_term.push_back(ref);
            }
        }
    }

    [[nodiscard]] static std::uint32_t minLongTermIdx(const RefFrame& frame) noexcept {
        std::uint32_t idx = std::numeric_limits<std::uint32_t>::max();
        if (frame.top.present && frame.top.long_term) idx = frame.top.long_term_frame_idx;
        if (frame.bottom.present && frame.bottom.long_term &&
            frame.bottom.long_term_frame_idx < idx) {
            idx = frame.bottom.long_term_frame_idx;
        }
        return idx;
    }

    void collectFieldGroups(std::vector<GroupRef>& short_term,
                            std::vector<GroupRef>& long_term) {
        for (RefFrame& frame : refs_) {
            const bool has_short = (frame.top.present && !frame.top.long_term) ||
                                   (frame.bottom.present && !frame.bottom.long_term);
            const bool has_long = (frame.top.present && frame.top.long_term) ||
                                  (frame.bottom.present && frame.bottom.long_term);
            const GroupRef ref{&frame, frame.poc(), frameNumWrap(frame.frame_num)};
            if (has_short) short_term.push_back(ref);
            if (has_long) long_term.push_back(ref);
        }
    }

    [[nodiscard]] std::vector<MotionReferencePicture> interleaveFields(
            const std::vector<GroupRef>& groups,
            bool long_term) {
        struct Item {
            RefFrame* frame = nullptr;
            bool bottom = false;
        };
        std::vector<Item> items;
        for (const GroupRef& group : groups) {
            for (int bottom = 0; bottom < 2; ++bottom) {
                RefField& field = *fieldAt(*group.frame, bottom != 0);
                if (field.present && field.long_term == long_term) {
                    items.push_back(Item{group.frame, bottom != 0});
                }
            }
        }

        std::vector<MotionReferencePicture> out;
        out.reserve(items.size());
        std::vector<bool> used(items.size(), false);
        bool desired = current_.bottom_field;
        while (out.size() < items.size()) {
            std::size_t pick = items.size();
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (!used[i] && items[i].bottom == desired) {
                    pick = i;
                    break;
                }
            }
            if (pick == items.size()) {
                /* No more fields of the wanted parity: keep group order. */
                for (std::size_t i = 0; i < items.size(); ++i) {
                    if (!used[i]) {
                        used[i] = true;
                        out.push_back(toFieldView(*items[i].frame, items[i].bottom));
                    }
                }
                break;
            }
            used[pick] = true;
            out.push_back(toFieldView(*items[pick].frame, items[pick].bottom));
            desired = !desired;
        }
        return out;
    }

    void build_reference_lists(
            const SliceHeader& header,
            const PictureParameterSet& pps,
            const SequenceParameterSet& sps) {
        (void)pps;
        current_.list0.clear();
        current_.list1.clear();
        current_.merged_refs.clear();
        current_.merged_ref_ids.clear();
        const bool first_slice = current_.slice_refs.empty();

        /*
         * I and SI slices have no reference picture lists (7.3.3.1); in
         * particular an IDR picture must not expose stale pre-IDR POCs.
         */
        if (header.slice_type == SliceType::I || header.slice_type == SliceType::SI) {
            auto& header_mem = current_header();
            header_mem.flags = current_.flags;
            if (first_slice) {
                header_mem.num_ref_idx[0] = 0;
                header_mem.num_ref_idx[1] = 0;
                header_mem.ref_non_existing_mask[0] = 0;
                header_mem.ref_non_existing_mask[1] = 0;
            }
            recordSliceReference(header, sps);
            return;
        }

        const bool is_b = header.slice_type == SliceType::B;
        const std::uint32_t max_frame_num = 1u << sps.log2_max_frame_num();

        if (!current_.field_pic) {
            std::vector<GroupRef> short_term;
            std::vector<GroupRef> long_term;
            collectFrameGroups(short_term, long_term);

            /*
             * 8.2.4.2.3: for pic_order_cnt_type 0 the POC of a non-existing
             * frame is not defined, so such frames are omitted from B lists.
             * P/SP lists keep them in PicNum order (they can never legally be
             * selected, but their slots affect ref_idx arithmetic).
             */
            if (is_b && sps.pic_order_cnt_type == 0) {
                short_term.erase(std::remove_if(short_term.begin(), short_term.end(),
                                                [](const GroupRef& ref) {
                                                    return ref.frame->nonExisting();
                                                }),
                                 short_term.end());
            }

            std::sort(short_term.begin(), short_term.end(),
                      [](const GroupRef& a, const GroupRef& b) {
                          if (a.frame_num_wrap != b.frame_num_wrap) {
                              return a.frame_num_wrap > b.frame_num_wrap;
                          }
                          return a.poc > b.poc;
                      });
            std::sort(long_term.begin(), long_term.end(),
                      [](const GroupRef& a, const GroupRef& b) {
                          return minLongTermIdx(*a.frame) < minLongTermIdx(*b.frame);
                      });

            if (!is_b) {
                for (const GroupRef& ref : short_term) {
                    current_.list0.push_back(toFrameView(*ref.frame));
                }
                for (const GroupRef& ref : long_term) {
                    current_.list0.push_back(toFrameView(*ref.frame));
                }
            } else {
                std::vector<GroupRef> past;
                std::vector<GroupRef> future;
                for (const GroupRef& ref : short_term) {
                    if (ref.poc < current_.poc) {
                        past.push_back(ref);
                    } else {
                        future.push_back(ref);
                    }
                }
                std::sort(past.begin(), past.end(),
                          [](const GroupRef& a, const GroupRef& b) {
                              return a.poc > b.poc;
                          });
                std::sort(future.begin(), future.end(),
                          [](const GroupRef& a, const GroupRef& b) {
                              return a.poc < b.poc;
                          });

                for (const GroupRef& ref : past) {
                    current_.list0.push_back(toFrameView(*ref.frame));
                }
                for (const GroupRef& ref : future) {
                    current_.list0.push_back(toFrameView(*ref.frame));
                }
                for (const GroupRef& ref : long_term) {
                    current_.list0.push_back(toFrameView(*ref.frame));
                }

                for (const GroupRef& ref : future) {
                    current_.list1.push_back(toFrameView(*ref.frame));
                }
                for (const GroupRef& ref : past) {
                    current_.list1.push_back(toFrameView(*ref.frame));
                }
                for (const GroupRef& ref : long_term) {
                    current_.list1.push_back(toFrameView(*ref.frame));
                }
            }

            /*
             * 8.2.4.2.3: the initial B lists are made distinct before the
             * reordering commands are applied.
             */
            if (is_b && current_.list1.size() > 1 &&
                current_.list0.size() == current_.list1.size()) {
                bool identical = true;
                for (std::size_t i = 0; i < current_.list0.size(); ++i) {
                    if (!sameReference(current_.list0[i], current_.list1[i])) {
                        identical = false;
                        break;
                    }
                }
                if (identical) {
                    std::swap(current_.list1[0], current_.list1[1]);
                }
            }

            applyListModification(current_.list0, header.ref_pic_list_modification_l0,
                                  max_frame_num, header.frame_num, false);
            if (is_b) {
                applyListModification(current_.list1, header.ref_pic_list_modification_l1,
                                      max_frame_num, header.frame_num, false);
            }
        } else {
            std::vector<GroupRef> short_term;
            std::vector<GroupRef> long_term;
            collectFieldGroups(short_term, long_term);

            if (is_b && sps.pic_order_cnt_type == 0) {
                short_term.erase(std::remove_if(short_term.begin(), short_term.end(),
                                                [](const GroupRef& ref) {
                                                    return ref.frame->nonExisting();
                                                }),
                                 short_term.end());
                long_term.erase(std::remove_if(long_term.begin(), long_term.end(),
                                               [](const GroupRef& ref) {
                                                   return ref.frame->nonExisting();
                                               }),
                                long_term.end());
            }

            std::sort(short_term.begin(), short_term.end(),
                      [](const GroupRef& a, const GroupRef& b) {
                          if (a.frame_num_wrap != b.frame_num_wrap) {
                              return a.frame_num_wrap > b.frame_num_wrap;
                          }
                          return a.poc > b.poc;
                      });
            std::sort(long_term.begin(), long_term.end(),
                      [](const GroupRef& a, const GroupRef& b) {
                          return minLongTermIdx(*a.frame) < minLongTermIdx(*b.frame);
                      });

            if (!is_b) {
                const auto fields = interleaveFields(short_term, false);
                current_.list0.insert(current_.list0.end(), fields.begin(), fields.end());
                const auto long_fields = interleaveFields(long_term, true);
                current_.list0.insert(current_.list0.end(), long_fields.begin(), long_fields.end());
            } else {
                std::vector<GroupRef> past;
                std::vector<GroupRef> future;
                for (const GroupRef& ref : short_term) {
                    if (ref.poc <= current_.poc) {
                        past.push_back(ref);
                    } else {
                        future.push_back(ref);
                    }
                }
                std::sort(past.begin(), past.end(),
                          [](const GroupRef& a, const GroupRef& b) {
                              return a.poc > b.poc;
                          });
                std::sort(future.begin(), future.end(),
                          [](const GroupRef& a, const GroupRef& b) {
                              return a.poc < b.poc;
                          });

                const auto past_fields = interleaveFields(past, false);
                const auto future_fields = interleaveFields(future, false);
                const auto long_fields = interleaveFields(long_term, true);

                current_.list0.insert(current_.list0.end(), past_fields.begin(), past_fields.end());
                current_.list0.insert(current_.list0.end(), future_fields.begin(), future_fields.end());
                current_.list0.insert(current_.list0.end(), long_fields.begin(), long_fields.end());

                current_.list1.insert(current_.list1.end(), future_fields.begin(), future_fields.end());
                current_.list1.insert(current_.list1.end(), past_fields.begin(), past_fields.end());
                current_.list1.insert(current_.list1.end(), long_fields.begin(), long_fields.end());
            }

            if (is_b && current_.list1.size() > 1 &&
                current_.list0.size() == current_.list1.size()) {
                bool identical = true;
                for (std::size_t i = 0; i < current_.list0.size(); ++i) {
                    if (!sameReference(current_.list0[i], current_.list1[i])) {
                        identical = false;
                        break;
                    }
                }
                if (identical) {
                    std::swap(current_.list1[0], current_.list1[1]);
                }
            }

            const std::uint32_t field_max_pic_num = max_frame_num * 2u;
            const std::uint32_t field_curr_pic_num = 2u * header.frame_num + 1u;
            applyListModification(current_.list0, header.ref_pic_list_modification_l0,
                                  field_max_pic_num, field_curr_pic_num, true);
            if (is_b) {
                applyListModification(current_.list1, header.ref_pic_list_modification_l1,
                                      field_max_pic_num, field_curr_pic_num, true);
            }
        }

        /*
         * num_ref_idx_l*_active_minus1 counts reference fields for a field
         * picture and reference frames for a frame picture.  The slice header
         * already contains the PPS defaults when the override flag is clear.
         */
        const int active0 = header.num_ref_idx_l0_active_minus1 + 1;
        const int active1 = header.num_ref_idx_l1_active_minus1 + 1;
        if (current_.list0.size() > static_cast<std::size_t>(active0)) {
            current_.list0.resize(static_cast<std::size_t>(active0));
        }
        if (current_.list1.size() > static_cast<std::size_t>(active1)) {
            current_.list1.resize(static_cast<std::size_t>(active1));
        }

        current_.flags |= current_.list0.empty()
                                  ? 0u
                                  : static_cast<std::uint32_t>(MvPictureHasList0);
        current_.flags |= current_.list1.empty()
                                  ? 0u
                                  : static_cast<std::uint32_t>(MvPictureHasList1);

        auto& header_mem = current_header();
        header_mem.flags = current_.flags;
        if (first_slice) {
            header_mem.num_ref_idx[0] = static_cast<std::uint8_t>(current_.list0.size());
            header_mem.num_ref_idx[1] = static_cast<std::uint8_t>(current_.list1.size());
            header_mem.ref_non_existing_mask[0] = 0;
            header_mem.ref_non_existing_mask[1] = 0;
            for (std::size_t i = 0; i < current_.list0.size() && i < 32; ++i) {
                header_mem.ref_poc[0][i] = current_.list0[i].poc;
                if (current_.list0[i].non_existing) {
                    header_mem.ref_non_existing_mask[0] |= 1u << static_cast<unsigned>(i);
                }
            }
            for (std::size_t i = 0; i < current_.list1.size() && i < 32; ++i) {
                header_mem.ref_poc[1][i] = current_.list1[i].poc;
                if (current_.list1[i].non_existing) {
                    header_mem.ref_non_existing_mask[1] |= 1u << static_cast<unsigned>(i);
                }
            }
        }
        recordSliceReference(header, sps);
    }

    /*
     * Append the reference table that applies to one slice.  `ref_idx` in
     * the slice data indexes these lists, not the picture-level convenience
     * table, so every slice gets its own entry even when the lists happen to
     * be identical.
     */
    void recordSliceReference(const SliceHeader& header,
                              const SequenceParameterSet& sps) {
        (void)sps;
        MotionSliceReference ref{};
        ref.mb_addr_begin = header.first_mb_in_slice * (current_.mbaff ? 2u : 1u);
        ref.mb_addr_end = currentMbCount();
        if (!current_.slice_refs.empty()) {
            MotionSliceReference& previous = current_.slice_refs.back();
            if (ref.mb_addr_begin > previous.mb_addr_begin) {
                previous.mb_addr_end = std::min(previous.mb_addr_end, ref.mb_addr_begin);
            }
        }
        ref.colour_plane_id = header.colour_plane_id;
        if (header.slice_type == SliceType::I || header.slice_type == SliceType::SI) {
            ref.flags |= MvSliceIntra;
        }
        if (header.adaptive_ref_pic_marking_mode_flag) {
            ref.flags |= MvSliceAdaptiveMarking;
        }

        const auto fill = [&ref](int list, const std::vector<MotionReferencePicture>& source) {
            const std::size_t count = std::min<std::size_t>(source.size(), 32u);
            ref.num_ref_idx[list] = static_cast<std::uint8_t>(count);
            for (std::size_t i = 0; i < count; ++i) {
                ref.ref_poc[list][i] = source[i].poc;
                if (source[i].non_existing) {
                    ref.non_existing_mask[list] |= 1u << static_cast<unsigned>(i);
                }
            }
        };
        fill(0, current_.list0);
        fill(1, current_.list1);

        if (!current_.list0.empty()) ref.flags |= MvSliceHasList0;
        if (!current_.list1.empty()) ref.flags |= MvSliceHasList1;
        if ((ref.non_existing_mask[0] | ref.non_existing_mask[1]) != 0) {
            ref.flags |= MvSliceHasNonExistingRef;
        }

        current_.slice_refs.push_back(ref);
        if (!sink_.options.defer_ready) {
            ensureSliceTableCapacity(current_.slice_refs.size());
        }
    }

    /*
     * 8.2.5.2: insert non-existing short-term frames for every unused
     * frame_num between PrevRefFrameNum and the current picture.  The gap
     * frames occupy DPB and PicNum slots but have no coded motion; they are
     * never delivered through ready().
     */
    void synthesizeFrameNumGaps(
            const SequenceParameterSet& sps,
            const SliceHeader& header,
            bool idr,
            bool pair_second) {
        if (idr || pair_second || !prev_ref_frame_num_valid_ ||
            !sps.gaps_in_frame_num_value_allowed_flag) {
            return;
        }

        const std::uint32_t max_frame_num = current_.frame_num_max;
        if (max_frame_num == 0 || header.frame_num == prev_ref_frame_num_) {
            return;
        }
        const std::uint32_t mask = max_frame_num - 1u;
        std::uint32_t unused = (prev_ref_frame_num_ + 1u) & mask;
        if (unused == header.frame_num) {
            return;
        }

        /* A conforming picture cannot wrap more than MaxFrameNum-1 times. */
        for (std::uint32_t guard = 0; guard < max_frame_num && unused != header.frame_num;
             ++guard) {
            while (referenceFrameCount() >= maxReferenceFrames()) {
                if (!removeOldestReference()) {
                    break;
                }
            }

            RefFrame gap;
            gap.frame_id = next_frame_id_++;
            gap.frame_num = unused;
            gap.frame_picture = true;

            std::int32_t top_poc = kMotionVectorUnknownPoc;
            std::int32_t bottom_poc = kMotionVectorUnknownPoc;
            deriveGapPoc(sps, unused, top_poc, bottom_poc);
            gap.poc_top = top_poc;
            gap.poc_bottom = bottom_poc;

            RefField field;
            field.present = true;
            field.non_existing = true;
            field.long_term = false;
            field.coded_field = false;
            field.width4x4 = current_.width4x4;
            field.height4x4 = current_.height4x4;
            field.cells = nullptr;
            field.poc = top_poc;
            field.bottom = false;
            gap.top = field;
            field.poc = bottom_poc;
            field.bottom = true;
            gap.bottom = field;
            refs_.push_back(std::move(gap));

            prev_ref_frame_num_ = unused;
            prev_ref_frame_num_valid_ = true;
            unused = (unused + 1u) & mask;
        }
    }

    /*
     * POC derivation for a non-existing frame.  For pic_order_cnt_type 0 the
     * standard deliberately does not infer a POC (8.2.5.2); the unknown
     * sentinel records that, and B lists omit such frames.  Types 1 and 2 use
     * the inferred frame syntax from 8.2.1 with field_pic_flag = 0,
     * nal_ref_idc != 0 and zero deltas.
     */
    void deriveGapPoc(const SequenceParameterSet& sps,
                      std::uint32_t frame_num,
                      std::int32_t& top_poc,
                      std::int32_t& bottom_poc) {
        const std::uint32_t max_frame_num = 1u << sps.log2_max_frame_num();
        std::int32_t frame_num_offset = gap_poc_state_.prev_frame_num_offset;
        if (gap_poc_state_.valid && gap_poc_state_.prev_frame_num > frame_num) {
            frame_num_offset += static_cast<std::int32_t>(max_frame_num);
        }

        if (sps.pic_order_cnt_type == 1) {
            const std::uint32_t cycle = sps.num_ref_frames_in_pic_order_cnt_cycle;
            std::int64_t abs_frame_num =
                    cycle != 0 ? static_cast<std::int64_t>(frame_num_offset) + frame_num : 0;
            std::int64_t expected = 0;
            if (abs_frame_num > 0 && cycle != 0) {
                std::int64_t expected_delta = 0;
                for (std::uint32_t i = 0; i < cycle; ++i) {
                    expected_delta += sps.offset_for_ref_frame[i];
                }
                const std::int64_t cycle_count = (abs_frame_num - 1) / cycle;
                const std::int64_t frame_in_cycle = (abs_frame_num - 1) % cycle;
                expected = cycle_count * expected_delta;
                for (std::int64_t i = 0; i <= frame_in_cycle; ++i) {
                    expected += sps.offset_for_ref_frame[static_cast<std::size_t>(i)];
                }
            }
            top_poc = static_cast<std::int32_t>(expected);
            bottom_poc = top_poc + sps.offset_for_top_to_bottom_field;
        } else if (sps.pic_order_cnt_type == 2) {
            top_poc = 2 * (frame_num_offset + static_cast<std::int32_t>(frame_num));
            bottom_poc = top_poc;
        } else {
            top_poc = kMotionVectorUnknownPoc;
            bottom_poc = kMotionVectorUnknownPoc;
        }

        gap_poc_state_.valid = true;
        gap_poc_state_.prev_frame_num = frame_num;
        gap_poc_state_.prev_frame_num_offset = frame_num_offset;
    }

    /*
     * Advance the POC state used by deriveGapPoc() with the current decoded
     * picture.  This mirrors the FrameNumOffset part of 8.2.1; it is separate
     * from the outer PocState because the extractor only receives the already
     * derived POCs.
     */
    void updateGapPocState(const SequenceParameterSet& sps,
                           const SliceHeader& header,
                           bool idr) {
        if (idr) {
            gap_poc_state_ = GapPocState{};
            gap_poc_state_.valid = true;
            return;
        }

        const std::uint32_t max_frame_num = 1u << sps.log2_max_frame_num();
        std::int32_t frame_num_offset = gap_poc_state_.prev_frame_num_offset;
        if (gap_poc_state_.valid && gap_poc_state_.prev_frame_num > header.frame_num) {
            frame_num_offset += static_cast<std::int32_t>(max_frame_num);
        }

        bool mmco5 = false;
        for (const MmcoOperation& op : header.mmco_operations) {
            if (op.memory_management_control_operation == 5) {
                mmco5 = true;
                break;
            }
        }

        gap_poc_state_.valid = true;
        gap_poc_state_.prev_frame_num = header.frame_num;
        gap_poc_state_.prev_frame_num_offset = mmco5 ? 0 : frame_num_offset;
    }

    void start_picture(
            const SequenceParameterSet& sps,
            const PictureParameterSet& pps,
            const SliceHeader& header,
            bool idr,
            std::uint8_t nal_ref_idc) {
        sps_max_refs_ = sps.max_num_ref_frames;
        (void)pps;

        /*
         * A bottom field immediately following the top field with the same
         * frame_num is the second field of a complementary pair.  Both fields
         * share one DPB frame entry.
         */
        const bool pair_second =
                header.field_pic_flag && header.bottom_field_flag && last_picture_valid_ &&
                last_picture_field_ && !last_picture_bottom_ &&
                last_picture_frame_num_ == header.frame_num;

        current_.clear();
        current_.started = true;
        current_.reference = nal_ref_idc != 0;
        current_.idr = idr;
        current_.pair_second = pair_second;
        current_.frame_id = pair_second ? last_frame_id_ : next_frame_id_++;
        current_.frame_num = header.frame_num;
        current_.frame_num_max = 1u << sps.log2_max_frame_num();
        current_.poc = header.derived_poc;
        current_.poc_top = header.derived_poc_top;
        current_.poc_bottom = header.derived_poc_bottom;
        if (current_.poc_top == 0 && current_.poc_bottom == 0 && header.derived_poc != 0) {
            current_.poc_top = current_.poc;
            current_.poc_bottom = current_.poc;
        }
        current_.field_pic = header.field_pic_flag;
        current_.bottom_field = header.field_pic_flag && header.bottom_field_flag;
        current_.mbaff = !sps.frame_mbs_only_flag &&
                         sps.mb_adaptive_frame_field_flag &&
                         !header.field_pic_flag;
        current_.adaptive_marking = !idr && header.adaptive_ref_pic_marking_mode_flag;
        current_.mmco_operations = header.mmco_operations;

        /*
         * The current picture's final long-term assignment is known from the
         * slice header: IDR long_term_reference_flag or the last MMCO
         * operation 6.  Keeping it here lets the ready callback expose the
         * marking result even though marking itself runs at flush time.
         */
        if (idr) {
            current_.long_term = header.long_term_reference_flag;
            current_.long_term_frame_idx = 0;
        } else if (header.adaptive_ref_pic_marking_mode_flag) {
            for (const MmcoOperation& op : header.mmco_operations) {
                if (op.memory_management_control_operation == 6) {
                    current_.long_term = true;
                    current_.long_term_frame_idx = op.long_term_frame_idx;
                }
            }
        }

        /*
         * Frame-num gaps are handled by the non-existing-frame process of
         * H.264 8.2.5.2.  The synthetic frames enter the MV-DPB with PicNum
         * slots but no motion data.
         */
        const std::uint32_t map_units =
                static_cast<std::uint32_t>(sps.pic_height_in_map_units_minus1 + 1);
        current_.mb_width = sps.pic_width_in_mbs_minus1 + 1;
        current_.mb_height = header.field_pic_flag
                                     ? map_units
                                     : map_units * (sps.frame_mbs_only_flag ? 1u : 2u);
        current_.width4x4 = current_.mb_width * 4;
        current_.height4x4 = current_.mb_height * 4;
        const std::size_t mb_count =
                static_cast<std::size_t>(current_.mb_width) * current_.mb_height;
        /*
         * One-time capacity growth: every later picture reuses the array.  A
         * parsed macroblock calls MbData::reset() before writing, so only the
         * availability flag must be cleared here.  Unparsed/FMO/malformed
         * macroblocks therefore keep the exact "unavailable" behaviour they
         * had when the whole array was re-initialised.
         */
        if (current_.mbs.size() != mb_count) {
            current_.mbs.resize(mb_count);
        }
        for (detail::MbData& mb : current_.mbs) {
            mb.available = false;
        }
        current_.next_slice_id = 0;

        synthesizeFrameNumGaps(sps, header, idr, pair_second);
        updateGapPocState(sps, header, idr);

        current_.flags = (idr ? static_cast<std::uint32_t>(MvPictureIdr) : 0u) |
                         (header.slice_type == SliceType::I
                                  ? static_cast<std::uint32_t>(MvPictureIntra)
                                  : 0u) |
                         (current_.reference
                                  ? static_cast<std::uint32_t>(MvPictureReference)
                                  : 0u) |
                         (header.field_pic_flag
                                  ? static_cast<std::uint32_t>(MvPictureField)
                                  : 0u) |
                         (header.field_pic_flag && header.bottom_field_flag
                                  ? static_cast<std::uint32_t>(MvPictureBottomField)
                                  : 0u) |
                         (current_.mbaff
                                  ? static_cast<std::uint32_t>(MvPictureMbaff)
                                  : 0u) |
                         (sps.chroma_format_idc == 0
                                  ? static_cast<std::uint32_t>(MvPictureMonochrome)
                                  : 0u) |
                         (sps.separate_colour_plane_flag
                                  ? static_cast<std::uint32_t>(
                                            MvPictureSeparateColourPlanes)
                                  : 0u) |
                         (current_.long_term
                                  ? static_cast<std::uint32_t>(MvPictureLongTerm)
                                  : 0u) |
                         mvPictureColourPlaneFlag(colour_plane_id_);

        const std::size_t cell_count = static_cast<std::size_t>(current_.width4x4) *
                                       current_.height4x4;
        current_.slice_capacity = std::max<std::uint32_t>(
                1u, std::min<std::uint32_t>(currentMbCount(), kInitialSliceCapacity));
        current_.slice_map_capacity = std::max<std::uint32_t>(
                1u, std::min<std::uint32_t>(currentMbCount(), kInitialSliceCapacity));
        /*
         * Reserve the per-picture bookkeeping once.  CurrentPicture::clear()
         * keeps these allocations, so subsequent pictures of the same size do
         * not allocate in the slice/flush path.
         */
        current_.slice_refs.reserve(current_.slice_capacity);
        current_.slice_ref_by_id.reserve(current_.slice_capacity);
        current_.slice_map.reserve(current_.slice_map_capacity);
        current_.list0.reserve(32);
        current_.list1.reserve(32);
        const std::size_t cell_offset =
                cellsOffsetFor(current_.slice_capacity, current_.slice_map_capacity);
        current_.required_bytes = requiredBytesFor(
                current_.slice_capacity, current_.slice_map_capacity, cell_count);

        MotionVectorBufferRequest request{};
        request.width4x4 = current_.width4x4;
        request.height4x4 = current_.height4x4;
        request.cell_count = static_cast<std::uint32_t>(cell_count);
        request.cell_stride = sizeof(MvCell);
        request.required_bytes = current_.required_bytes;
        request.alignment = sink_.options.buffer_alignment;
        request.poc = current_.poc;
        request.frame_num = current_.frame_num;
        request.flags = current_.flags;
        request.colour_plane_id = colour_plane_id_;
        request.plane_count = plane_count_;

        if (sink_.acquire != nullptr &&
            cell_count <= static_cast<std::size_t>(sink_.options.max_macroblocks) * 256u) {
            current_.sink_buffer = sink_.acquire(sink_.ctx, request);
            if (current_.sink_buffer.data != nullptr &&
                current_.sink_buffer.capacity >= current_.required_bytes) {
                auto* header_mem = static_cast<std::uint8_t*>(current_.sink_buffer.data);
                current_.owned_header = MotionVectorBufferHeader{};
                current_.cells = reinterpret_cast<MvCell*>(header_mem + cell_offset);
            } else {
                current_.sink_buffer = MotionVectorBuffer{};
                current_.status = MotionVectorStatus::BufferUnavailable;
                current_.buffer_unavailable = true;
            }
        }

        if (current_.cells == nullptr) {
            current_.owned_grid = grid_pool_->tryAcquire();
            if (current_.owned_grid) {
                current_.owned_grid->cells.resize(cell_count);
                current_.cells = current_.owned_grid->cells.data();
            } else {
                current_.status = MotionVectorStatus::LimitExceeded;
            }
            if (current_.sink_buffer.data == nullptr && sink_.acquire != nullptr &&
                current_.status == MotionVectorStatus::Ok) {
                current_.status = MotionVectorStatus::BufferUnavailable;
            }
        }

        auto& header_mem = current_header();
        header_mem = MotionVectorBufferHeader{};
        header_mem.header_bytes = sizeof(MotionVectorBufferHeader);
        header_mem.total_bytes = static_cast<std::uint32_t>(current_.required_bytes);
        header_mem.flags = current_.flags;
        header_mem.poc = current_.poc;
        header_mem.frame_num = current_.frame_num;
        header_mem.width = sps.pic_width_in_luma_samples();
        header_mem.height = header.field_pic_flag
                                     ? sps.pic_height_in_luma_samples() / 2
                                     : sps.pic_height_in_luma_samples();
        header_mem.mb_width = current_.mb_width;
        header_mem.mb_height = current_.mb_height;
        header_mem.width4x4 = current_.width4x4;
        header_mem.height4x4 = current_.height4x4;
        header_mem.cell_count = static_cast<std::uint32_t>(cell_count);
        header_mem.cell_stride = sizeof(MvCell);
        header_mem.cells_offset = static_cast<std::uint32_t>(cell_offset);
        header_mem.block_count = 0;
        header_mem.block_stride = sizeof(MotionBlock);
        header_mem.blocks_offset = 0;
        header_mem.status = static_cast<std::uint8_t>(current_.status);
        header_mem.long_term_frame_idx = current_.long_term
                                                  ? static_cast<std::int8_t>(current_.long_term_frame_idx)
                                                  : static_cast<std::int8_t>(-1);
        header_mem.colour_plane_id = colour_plane_id_;
        header_mem.plane_count = plane_count_;
        header_mem.slice_ref_count = 0;
        header_mem.slice_ref_stride = sizeof(MotionSliceReference);
        header_mem.slice_refs_offset = static_cast<std::uint32_t>(sliceTableOffset());
        header_mem.slice_ref_capacity = current_.slice_capacity;
        header_mem.slice_map_count = 0;
        header_mem.slice_map_stride = sizeof(MotionSliceRun);
        header_mem.slice_map_offset = static_cast<std::uint32_t>(
                sliceMapOffsetFor(current_.slice_capacity));
        header_mem.slice_map_capacity = current_.slice_map_capacity;
        header_mem.slice_map_coverage = 0;

        if (current_.mbaff) {
            simd::fillCells(
                    current_.cells, cell_count, simd::CellPattern::unavailable().cell);
        }
        /*
         * Grid cells are not cleared here.  Every coded macroblock overwrites
         * its 4x4 cells, and flush() fills the ones no slice covered (see
         * fillUncoveredCells()).  This avoids writing the whole grid twice.
         */
    }

    void on_slice_impl(
            const NalUnit& nal,
            const SequenceParameterSet& sps,
            const PictureParameterSet& pps,
            const SliceHeader& header,
            bool idr,
            std::uint8_t nal_ref_idc,
            std::size_t header_bit_offset = 0) {
        if (sink_.acquire == nullptr || sink_.ready == nullptr) {
            return;
        }
        /* The facade routes each colour plane to its own extractor. */
        if (sink_.options.require_idr_start && idr == false && refs_.empty() &&
            !current_.started) {
            status_ = MotionVectorStatus::Unsupported;
            return;
        }

        const bool first_slice = header.first_mb_in_slice == 0;
        if (!first_slice && !current_.started) {
            status_ = MotionVectorStatus::Unsupported;
            return;
        }
        if (first_slice) {
            const bool no_output_prior = idr && header.no_output_of_prior_pics_flag;
            if (no_output_prior) {
                /*
                 * The new IDR says every prior picture still awaiting output
                 * must be discarded.  The top field of the same IDR frame is
                 * not a prior picture: preserve it when the bottom field
                 * arrives and only discard the queued earlier GOP.
                 */
                discardPendingOutput();
                const bool same_idr_pair =
                        current_.started && current_.field_pic && !current_.bottom_field &&
                        current_.frame_num == header.frame_num && header.field_pic_flag &&
                        header.bottom_field_flag;
                if (same_idr_pair) {
                    flush();
                } else {
                    dropCurrentWithoutOutput();
                }
            } else {
                flush();
                if (idr) {
                    /*
                     * no_output_of_prior_pics_flag == 0 (or a non-IDR
                     * boundary): prior pictures are output before this IDR.
                     */
                    deliverPendingOutput();
                }
            }
            start_picture(sps, pps, header, idr, nal_ref_idc);
            /*
             * A deferred picture may fail to acquire a buffer only because the
             * queue is holding earlier buffers; it is spilled to the copy
             * queue at flush and drained in order.  Do not surface that
             * transient condition as a parse error.  Immediate delivery and
             * genuine grid exhaustion keep the original BufferUnavailable /
             * LimitExceeded reporting.
             */
            const bool deferred_spill =
                    sink_.options.defer_ready && current_.buffer_unavailable &&
                    current_.cells != nullptr;
            if (deferred_spill) {
                current_.status = MotionVectorStatus::Ok;
            } else if (current_.status != MotionVectorStatus::Ok) {
                status_ = current_.status;
            }
        }
        if (current_.cells == nullptr) {
            status_ = current_.status;
            return;
        }

        /*
         * Reference lists are per slice, not per picture.  Rebuild them from
         * this slice's reordering commands and active counts, then record the
         * result so each cell's ref_idx can be mapped back to its slice's POC.
         */
        build_reference_lists(header, pps, sps);
        const std::uint32_t slice_ref_index =
                static_cast<std::uint32_t>(current_.slice_refs.size() - 1u);
        const MotionSliceReference slice_ref = current_.slice_refs.back();

        if (header.slice_type == SliceType::I || header.slice_type == SliceType::SI) {
            markIntraSliceCells();
            if (!current_.has_inter_slice) {
                current_.flags |= static_cast<std::uint32_t>(MvPictureIntra);
            }
            return;
        }
        current_.has_inter_slice = true;
        current_.flags &= ~static_cast<std::uint32_t>(MvPictureIntra);

        const std::uint32_t slice_id = current_.next_slice_id++;
        if (current_.slice_ref_by_id.size() <= slice_id) {
            current_.slice_ref_by_id.resize(
                    static_cast<std::size_t>(slice_id) + 1u, kMotionSliceIndexNone);
        }
        current_.slice_ref_by_id[slice_id] = slice_ref_index;

        extract_rbsp_into(nal.payload_bytes(), rbsp_);
        /*
         * The dispatcher already parsed this slice header to build `header`;
         * reuse its logical bit position instead of re-parsing the header a
         * second time.  A zero offset means an older caller did not supply it,
         * in which case fall back to one parse (keeps the API compatible).
         */
        std::size_t bit_offset = header_bit_offset;
        if (bit_offset == 0u) {
            RbspReader header_reader(nal.payload_bytes());
            (void)parse_slice_header(
                    header_reader, sps, pps, nal.type(), nal.header.nal_ref_idc);
            bit_offset = header_reader.logical_bit_position();
        }

        if (pps.entropy_coding_mode_flag) {
            bit_offset = (bit_offset + 7u) & ~std::size_t{7u};
        }

        MotionVectorStatus slice_status = MotionVectorStatus::Ok;
        SliceDataParser parser(
                sps,
                pps,
                header,
                idr,
                std::span<const std::uint8_t>(rbsp_.data(), rbsp_.size()),
                bit_offset,
                current_.cells,
                current_.width4x4,
                current_.height4x4,
                current_.mbs,
                slice_id,
                current_.list0,
                current_.list1,
                slice_status);

        if (!parser.parse()) {
            current_.status = slice_status;
            status_ = slice_status;
        }

        sanitizeSliceCells(slice_ref, slice_id);

        /*
         * Multi-slice pictures are accumulated until the next
         * first_mb_in_slice == 0 or the parse-end flush.
         */
    }

    /*
     * Mark only macroblocks that no inter slice has coded yet.  A picture may
     * legally mix I/SI and P/B slices; when an I slice is not the first slice,
     * marking the whole grid would erase motion already decoded for earlier
     * P/B slices.  Inter slices stamp MbData::available, so a macroblock that
     * is still unavailable belongs to an I/SI slice (or to a later slice that
     * will overwrite its cells when parsed).
     */
    void markIntraSliceCells() {
        if (current_.cells == nullptr) return;
        const std::uint32_t mb_count = currentMbCount();
        const std::size_t mb_entries = std::min<std::size_t>(mb_count, current_.mbs.size());
        for (std::uint32_t mb_addr = 0; mb_addr < mb_entries; ++mb_addr) {
            if (current_.mbs[mb_addr].available) {
                continue;
            }
            std::uint32_t mb_x = 0;
            std::uint32_t mb_y = 0;
            if (current_.mbaff) {
                const std::uint32_t pair = mb_addr / 2u;
                mb_x = pair % current_.mb_width;
                mb_y = (pair / current_.mb_width) * 2u + (mb_addr & 1u);
            } else {
                mb_x = mb_addr % current_.mb_width;
                mb_y = mb_addr / current_.mb_width;
            }
            simd::fillBlock(current_.cells, current_.width4x4,
                            mb_x * 4u, mb_y * 4u, 4, 4,
                            simd::CellPattern::intra().cell);
            current_.mbs[mb_addr].intra_cells_written = true;
        }
    }

    /*
     * Fill cells that no slice covered.  Previously the whole grid was
     * cleared at picture start and then overwritten by every coded MB; this
     * is the same result with one pass over only the uncovered macroblocks.
     */
    void fillUncoveredCells() {
        if (current_.cells == nullptr) {
            return;
        }
        const std::uint32_t mb_count = currentMbCount();
        const std::size_t mb_entries = std::min<std::size_t>(mb_count, current_.mbs.size());
        for (std::uint32_t mb_addr = 0; mb_addr < mb_entries; ++mb_addr) {
            const detail::MbData& mb = current_.mbs[mb_addr];
            if (mb.available || mb.intra_cells_written) {
                continue;
            }
            std::uint32_t mb_x = 0;
            std::uint32_t mb_y = 0;
            if (current_.mbaff) {
                const std::uint32_t pair = mb_addr / 2u;
                mb_x = pair % current_.mb_width;
                mb_y = (pair / current_.mb_width) * 2u + (mb_addr & 1u);
            } else {
                mb_x = mb_addr % current_.mb_width;
                mb_y = mb_addr / current_.mb_width;
            }
            simd::fillBlock(current_.cells, current_.width4x4,
                            mb_x * 4u, mb_y * 4u, 4, 4,
                            simd::CellPattern::unavailable().cell);
        }
    }

    /*
     * A slice may legally include a non-existing reference in its list slots
     * (the slot affects later PicNum/ref_idx arithmetic), but H.264 8.2.5.2
     * forbids selecting one.  Never expose fabricated motion for such a
     * selection: zero the affected list, retain ref_idx for diagnostics and
     * set MvRefNonExisting.  If no list remains usable the cell is marked
     * MvUnavailable.
     *
     * The macroblocks are selected by slice id, not by a contiguous address
     * range: an FMO slice group is not contiguous, and an MBAFF address is
     * not raster order.
     */
    void sanitizeSliceCells(const MotionSliceReference& slice, std::uint32_t slice_id) {
        if (current_.cells == nullptr) return;
        if ((slice.non_existing_mask[0] | slice.non_existing_mask[1]) == 0) return;
        const std::uint32_t mb_count = currentMbCount();
        const std::size_t mb_entries = std::min<std::size_t>(mb_count, current_.mbs.size());
        for (std::uint32_t mb_addr = 0; mb_addr < mb_entries; ++mb_addr) {
            const detail::MbData& mb = current_.mbs[mb_addr];
            if (!mb.available || mb.slice_id != slice_id) {
                continue;
            }
            std::uint32_t mb_x = 0;
            std::uint32_t mb_y = 0;
            if (current_.mbaff) {
                const std::uint32_t pair = mb_addr / 2u;
                mb_x = pair % current_.mb_width;
                mb_y = (pair / current_.mb_width) * 2u + (mb_addr & 1u);
            } else {
                mb_x = mb_addr % current_.mb_width;
                mb_y = mb_addr / current_.mb_width;
            }
            for (std::uint32_t y = 0; y < 4; ++y) {
                for (std::uint32_t x = 0; x < 4; ++x) {
                    MvCell& cell =
                            current_.cells[static_cast<std::size_t>(mb_y * 4u + y) *
                                                   current_.width4x4 +
                                           mb_x * 4u + x];
                    bool missing = false;
                    bool usable = false;
                    for (int list = 0; list < 2; ++list) {
                        const std::uint32_t mask = slice.non_existing_mask[list];
                        if (mask == 0) {
                            const int index = cell.ref_idx[list];
                            if (index >= 0 && index < 32 &&
                                index < static_cast<int>(slice.num_ref_idx[list])) {
                                usable = true;
                            }
                            continue;
                        }
                        const int index = cell.ref_idx[list];
                        if (index < 0 || index >= 32) {
                            continue;
                        }
                        if ((mask & (1u << static_cast<unsigned>(index))) != 0) {
                            cell.mv[list][0] = 0;
                            cell.mv[list][1] = 0;
                            cell.flags &= static_cast<std::uint8_t>(
                                    ~(list == 0 ? MvList0 : MvList1));
                            missing = true;
                        } else if (index < static_cast<int>(slice.num_ref_idx[list])) {
                            usable = true;
                        }
                    }
                    if (missing) {
                        cell.flags |= MvRefNonExisting;
                        if (!usable) {
                            cell.flags |= MvUnavailable;
                        }
                    }
                }
            }
        }
    }

    MotionVectorSink sink_{};
    std::unique_ptr<MotionGridPool> grid_pool_;
    std::vector<RefFrame> refs_;
    CurrentPicture current_;
    std::vector<PendingOutput> pending_;
    /* RBSP of the slice currently being parsed; reused across slices. */
    std::vector<std::uint8_t> rbsp_;
    /*
     * Scratch buffers for finalizeSliceReferences(), sized to the picture.
     * Reused across pictures so the flush path does not allocate.
     */
    std::vector<std::uint32_t> scratch_per_mb_;
    std::vector<std::uint32_t> scratch_order_;
    std::vector<std::uint32_t> scratch_old_to_new_;
    std::vector<MotionSliceReference> scratch_sorted_;
    MotionVectorStatus status_ = MotionVectorStatus::Ok;
    std::uint8_t colour_plane_id_ = 0;
    std::uint8_t plane_count_ = 1;

    struct GapPocState {
        bool valid = false;
        std::uint32_t prev_frame_num = 0;
        std::int32_t prev_frame_num_offset = 0;
    };
    GapPocState gap_poc_state_;

    std::uint32_t sps_max_refs_ = 16;
    int max_long_term_frame_idx_ = -1;
    std::uint32_t next_frame_id_ = 1;
    std::uint32_t prev_ref_frame_num_ = 0;
    bool prev_ref_frame_num_valid_ = false;
    bool last_picture_valid_ = false;
    bool last_picture_field_ = false;
    bool last_picture_bottom_ = false;
    std::uint32_t last_picture_frame_num_ = 0;
    std::uint32_t last_frame_id_ = 0;
};

/*
 * Public facade.  Ordinary streams use a single plane decoder.  A stream with
 * separate_colour_plane_flag owns three independent plane decoders, each with
 * its own MV-DPB and grid, and delivers each plane separately.  When a new
 * access unit starts, all planes of the previous unit are flushed first so
 * consumers receive plane 0/1/2 of one access unit together.
 */
class MotionVectorExtractor {
   public:
    using MotionGrid = MotionPlaneExtractor::MotionGrid;
    using MotionGridPolicy = MotionPlaneExtractor::MotionGridPolicy;
    using MotionGridPool = MotionPlaneExtractor::MotionGridPool;

    MotionVectorExtractor() = default;

    void set_sink(const MotionVectorSink& sink) {
        sink_ = sink;
        for (auto& plane : planes_) {
            if (plane) {
                plane->set_sink(sink_);
            }
        }
    }

    void clear_sink() {
        sink_ = MotionVectorSink{};
        for (auto& plane : planes_) {
            if (plane) {
                plane->clear_sink();
            }
        }
    }

    void reset() {
        for (auto& plane : planes_) {
            if (plane) {
                plane->reset();
                plane->clear_sink();
            }
            plane.reset();
        }
        status_ = MotionVectorStatus::Ok;
        au_valid_ = false;
    }

    [[nodiscard]] MotionVectorStatus status() const noexcept { return status_; }

    /*
     * H.264 data partitioning (NAL unit types 2-4) splits one slice across
     * three NAL units whose residual and prediction syntax must be combined
     * before a macroblock can be reconstructed.  The extractor does not
     * implement that combination, so the caller reports it explicitly rather
     * than silently returning a picture with missing slices.
     */
    void noteUnsupportedDataPartition() noexcept {
        status_ = MotionVectorStatus::Unsupported;
    }

    /*
     * Reference-picture count of colour plane 0.  For separate-plane streams
     * use the overload to inspect each independent plane DPB.
     */
    [[nodiscard]] std::size_t referencePictureCount() const noexcept {
        return referencePictureCount(0);
    }

    [[nodiscard]] std::size_t referencePictureCount(std::uint8_t plane) const noexcept {
        const MotionPlaneExtractor* decoder = plane_at(plane);
        return decoder != nullptr ? decoder->referencePictureCount() : 0;
    }

    [[nodiscard]] int maxLongTermFrameIdx() const noexcept {
        return maxLongTermFrameIdx(0);
    }

    [[nodiscard]] int maxLongTermFrameIdx(std::uint8_t plane) const noexcept {
        const MotionPlaneExtractor* decoder = plane_at(plane);
        return decoder != nullptr ? decoder->maxLongTermFrameIdx() : -1;
    }

    void on_slice(
            const NalUnit& nal,
            const SequenceParameterSet& sps,
            const PictureParameterSet& pps,
            const SliceHeader& header,
            bool idr,
            std::uint8_t nal_ref_idc,
            std::size_t header_bit_offset = 0) {
        try {
            const bool separate = sps.separate_colour_plane_flag;
            const std::uint8_t plane = separate ? header.colour_plane_id : 0;
            if (plane > 2) {
                status_ = MotionVectorStatus::Malformed;
                return;
            }

            if (separate && header.first_mb_in_slice == 0) {
                if (!au_valid_) {
                    remember_access_unit(header, idr);
                } else if (!same_access_unit(header, idr)) {
                    /*
                     * The previous access unit is complete.  Queue it in
                     * every plane, then either output it or discard it based
                     * on this IDR's no_output_of_prior_pics_flag.  The plane
                     * decoder's own IDR handling sees empty queues afterwards.
                     */
                    flush_planes();
                    if (idr) {
                        if (header.no_output_of_prior_pics_flag) {
                            discard_pending_planes();
                        } else {
                            drain_pending_planes();
                        }
                    }
                    remember_access_unit(header, idr);
                }
            }

            auto& decoder = ensure_plane(plane);
            decoder.set_colour_plane(plane, separate ? 3 : 1);
            decoder.on_slice(nal, sps, pps, header, idr, nal_ref_idc, header_bit_offset);

            if (decoder.status() != MotionVectorStatus::Ok) {
                status_ = decoder.status();
            }
        } catch (...) {
            status_ = MotionVectorStatus::Malformed;
        }
    }

    void flush() {
        flush_planes();
        au_valid_ = false;
    }

    /*
     * Complete the stream: flush every plane's current picture and deliver
     * its deferred queue.  At normal access-unit boundaries the facade has
     * already drained earlier units, so the plane order here retains the
     * 0/1/2 delivery order of the last access unit.
     */
    void finish() {
        flush_planes();
        drain_pending_planes();
        au_valid_ = false;
    }

    [[nodiscard]] std::size_t deferredPictureCount(std::uint8_t plane = 0) const noexcept {
        const MotionPlaneExtractor* decoder = plane_at(plane);
        return decoder != nullptr ? decoder->deferredPictureCount() : 0;
    }

   private:
    [[nodiscard]] MotionPlaneExtractor* plane_at(std::uint8_t plane) noexcept {
        return plane < planes_.size() ? planes_[plane].get() : nullptr;
    }

    [[nodiscard]] const MotionPlaneExtractor* plane_at(std::uint8_t plane) const noexcept {
        return plane < planes_.size() ? planes_[plane].get() : nullptr;
    }

    [[nodiscard]] MotionPlaneExtractor& ensure_plane(std::uint8_t plane) {
        auto& slot = planes_[plane];
        if (!slot) {
            slot = std::make_unique<MotionPlaneExtractor>();
            slot->set_sink(sink_);
        }
        return *slot;
    }

    void flush_planes() {
        for (auto& plane : planes_) {
            if (plane) {
                plane->flush();
            }
        }
    }

    /*
     * Deliver one queued picture per plane per round so an access unit is
     * emitted as plane 0/1/2 before the next access unit, even though each
     * plane owns an independent deferred queue.  With no_output pending, the
     * queues hold every access unit since the last IDR.
     */
    void drain_pending_planes() {
        bool delivered = true;
        while (delivered) {
            delivered = false;
            for (auto& plane : planes_) {
                if (plane && plane->deferredPictureCount() > 0) {
                    plane->deliverNextPendingPicture();
                    delivered = true;
                }
            }
        }
    }

    void discard_pending_planes() {
        for (auto& plane : planes_) {
            if (plane) {
                plane->discardPendingPictures();
            }
        }
    }

    [[nodiscard]] bool same_access_unit(const SliceHeader& header, bool idr) const noexcept {
        if (!au_valid_ || header.frame_num != au_frame_num_ || idr != au_idr_ ||
            header.field_pic_flag != au_field_ || header.bottom_field_flag != au_bottom_ ||
            header.derived_poc != au_poc_ || header.derived_poc_top != au_poc_top_ ||
            header.derived_poc_bottom != au_poc_bottom_) {
            return false;
        }
        return !idr || header.idr_pic_id == au_idr_pic_id_;
    }

    void remember_access_unit(const SliceHeader& header, bool idr) noexcept {
        au_valid_ = true;
        au_frame_num_ = header.frame_num;
        au_idr_ = idr;
        au_idr_pic_id_ = header.idr_pic_id;
        au_field_ = header.field_pic_flag;
        au_bottom_ = header.bottom_field_flag;
        au_poc_ = header.derived_poc;
        au_poc_top_ = header.derived_poc_top;
        au_poc_bottom_ = header.derived_poc_bottom;
    }

    std::array<std::unique_ptr<MotionPlaneExtractor>, 3> planes_{};
    MotionVectorSink sink_{};
    MotionVectorStatus status_ = MotionVectorStatus::Ok;

    bool au_valid_ = false;
    std::uint32_t au_frame_num_ = 0;
    bool au_idr_ = false;
    std::uint32_t au_idr_pic_id_ = 0;
    bool au_field_ = false;
    bool au_bottom_ = false;
    std::int32_t au_poc_ = 0;
    std::int32_t au_poc_top_ = 0;
    std::int32_t au_poc_bottom_ = 0;
};

} // namespace avc
} // namespace bs
