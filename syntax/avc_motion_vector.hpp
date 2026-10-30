// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/*
 * ===========================================================================
 * AVC motion-vector output model
 * ===========================================================================
 *
 * Experimental: the opt-in motion-vector extractor, its MotionVectorSink API
 * and the versioned buffer layout may change in future releases.
 *
 * Motion vectors are produced by the opt-in H.264 slice-data extractor.  The
 * public output is a versioned POD buffer supplied by the consumer, so the
 * parser never allocates or owns the consumer's motion-field memory.
 *
 * Coordinates and units
 * ---------------------
 *   - `MvCell` is one 4x4 luma block of the macroblock grid.
 *   - `mv` is in quarter-luma-sample units, [list][x/y].
 *   - Positive x is right, positive y is down.
 *   - A reference POC table maps ref_idx back to the picture it points at;
 *     short-term entries precede long-term entries, as required by H.264
 *     8.2.4.2.  `MvPictureLongTerm` and `long_term_frame_idx` expose the
 *     current picture's own marking.
 *
 * Per-slice reference lists
 * -------------------------
 * ref_pic_list_reordering() and the active reference counts are slice-level
 * syntax.  A picture may therefore contain slices whose identical ref_idx
 * points at different POCs.  Version 3 of the header carries a sorted
 * MotionSliceReference table plus an exact MotionSliceRun map from
 * macroblock address to slice-table index; use motionRefPocAt() or
 * motionTo(view, x4, y4, target_poc) to resolve the table for a cell.  The
 * map is authoritative, so a non-contiguous FMO slice group resolves every
 * one of its macroblocks instead of falling back to the first slice.  The
 * header's own ref_poc/num_ref_idx fields are the first slice's convenience
 * copy only.
 *
 * Macroblock addressing
 * ---------------------
 * MotionSliceReference.mb_addr_begin and MotionSliceRun.mb_addr use
 * CurrMbAddr units from H.264 7.3.4: raster over picture macroblocks for
 * frame/field pictures without MBAFF, and `2 * pair_index + parity` for an
 * MBAFF frame picture.  The motion grid origin helpers below convert a 4x4
 * cell coordinate to the same units.
 *
 * Non-existing frames
 * -------------------
 * Frames inferred by the frame_num gap process (8.2.5.2) occupy reference
 * list slots and PicNum order but have no motion data.  Their
 * ref_poc entry is kMotionVectorUnknownPoc when pic_order_cnt_type is 0;
 * non_existing_mask marks the slot in every case.  Cells that select one are
 * zeroed and flagged MvRefNonExisting (and MvUnavailable when no list remains
 * usable).
 *
 * Delivery
 * --------
 * The extractor normally defers ready() so an IDR with
 * no_output_of_prior_pics_flag == 1 can suppress the queued pictures.  The
 * lease in MotionVectorBufferView refers to the delivered buffer and remains
 * valid while the consumer keeps a copy.
 *
 * Field pictures
 * --------------
 * A field picture (`MvPictureField`) produces its own 4x4 grid whose height
 * is the field height.  Cell coordinates and vertical motion components are
 * in field-line units, exactly as coded by the field macroblocks.
 *
 * For an MBAFF frame picture (`MvPictureMbaff`) the grid is the normal frame
 * grid.  Cells of a field-coded macroblock carry `MvField`; their vertical
 * motion is normalised to the frame luma grid (the coded field MV is doubled)
 * so a consumer can mix frame- and field-coded macroblocks in one grid.
 * `ref_idx` always indexes the picture's reference-picture list; for an MBAFF
 * field macroblock the list still contains frame entries, and the field
 * parity is the parity of the macroblock within its pair.
 *
 * The buffer is read-only once the ready callback runs.  The parser may keep
 * a reference-picture buffer alive through the lease while it is in the DPB.
 */

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>

namespace bs {
namespace avc {

inline constexpr std::uint32_t kMotionVectorBufferMagic = 0x4D564231u; // "MVB1"
/*
 * Version 2 added both the colour-plane identity and the per-slice reference
 * table with its non-existing-reference masks.  Version 3 adds the exact
 * macroblock-to-slice map used by non-contiguous slice groups (FMO).  The
 * header remains a plain aggregate; `header_bytes` and the offsets let a
 * consumer validate a buffer without knowing this layout.
 */
inline constexpr std::uint16_t kMotionVectorBufferVersion = 3;
inline constexpr std::int32_t kMotionVectorScale = 4; // quarter-luma-sample
/*
 * POC value used for a "non-existing" reference picture whose picture order
 * count cannot be inferred (pic_order_cnt_type == 0, H.264 8.2.5.2).  It is
 * also the "no reference picture" value returned by the lookup helpers.
 */
inline constexpr std::int32_t kMotionVectorUnknownPoc =
        std::numeric_limits<std::int32_t>::min();

enum class MotionVectorStatus : std::uint8_t {
    Ok = 0,
    Unsupported,
    Truncated,
    LimitExceeded,
    Malformed,
    BufferUnavailable,
};

enum MotionVectorPictureFlags : std::uint32_t {
    MvPictureIdr = 1u << 0,
    MvPictureIntra = 1u << 1,
    MvPictureHasList0 = 1u << 2,
    MvPictureHasList1 = 1u << 3,
    MvPictureField = 1u << 4,
    MvPictureReference = 1u << 5,
    MvPictureBottomField = 1u << 6,
    /*
     * The picture is a frame picture coded with macroblock-adaptive
     * frame/field (MBAFF) syntax.  Individual cells carry MvField when
     * their macroblock pair was field-coded.
     */
    MvPictureMbaff = 1u << 7,
    /* chroma_format_idc == 0: the picture has no chroma component. */
    MvPictureMonochrome = 1u << 8,
    /*
     * separate_colour_plane_flag == 1.  Each colour plane is coded as an
     * independent picture with its own MV-DPB and is delivered in its own
     * buffer.  The two bits at MvPictureColourPlaneShift carry the plane
     * index; the dedicated header fields are the primary identity.
     */
    MvPictureSeparateColourPlanes = 1u << 9,
    /*
     * The picture is marked "used for long-term reference" after decoding
     * (IDR long_term_reference_flag or MMCO operation 3/6).
     */
    MvPictureLongTerm = 1u << 10,
    /*
     * separate_colour_plane_flag == 1.  Each colour plane is coded as an
     * independent picture and is delivered in its own buffer.  The two bits
     * at MvPictureColourPlaneShift carry the plane index as well; the
     * dedicated header fields are the primary identity.
     */
    MvPictureColourPlaneShift = 11,
    MvPictureColourPlaneMask = 3u << MvPictureColourPlaneShift,
    /*
     * The picture is a "non-existing" frame inferred by the frame_num gap
     * process (H.264 8.2.5.2).  Such pictures are never delivered through the
     * ready callback; the flag is available on internal DPB views.
     */
    MvPictureNonExisting = 1u << 13,
    /*
     * At least one reference list of this picture points at a non-existing
     * frame.  Cells that actually select such a reference are marked with
     * MvRefNonExisting and carry no motion data.
     */
    MvPictureHasNonExistingRef = 1u << 14,
};

[[nodiscard]]
constexpr std::uint32_t mvPictureColourPlaneFlag(std::uint8_t colour_plane_id) noexcept {
    return (static_cast<std::uint32_t>(colour_plane_id) & 3u)
           << MvPictureColourPlaneShift;
}

[[nodiscard]]
constexpr std::uint8_t mvPictureColourPlane(std::uint32_t flags) noexcept {
    return static_cast<std::uint8_t>((flags & MvPictureColourPlaneMask) >>
                                     MvPictureColourPlaneShift);
}

enum MvCellFlags : std::uint8_t {
    MvIntra = 1u << 0,
    MvSkip = 1u << 1,
    MvDirect = 1u << 2,
    MvList0 = 1u << 3,
    MvList1 = 1u << 4,
    MvUnavailable = 1u << 5,
    MvField = 1u << 6,
    /*
     * The cell selects a reference picture that is marked "non-existing" by
     * the frame_num gap process.  The affected list's motion is zero and has
     * no defined reference data.  `ref_idx` is retained so the consumer can
     * still resolve which list slot was coded.
     */
    MvRefNonExisting = 1u << 7,
};

/*
 * Per-slice flags for MotionSliceReference.  A picture may contain slices with
 * different reference-list reordering commands and active counts (H.264
 * 7.3.3.1/8.2.4); the table below records the result for each slice.
 */
enum MvSliceFlags : std::uint8_t {
    MvSliceHasList0 = 1u << 0,
    MvSliceHasList1 = 1u << 1,
    MvSliceIntra = 1u << 2,
    MvSliceHasNonExistingRef = 1u << 3,
    /* dec_ref_pic_marking() adaptive mode was signalled in this slice. */
    MvSliceAdaptiveMarking = 1u << 4,
};

/*
 * One 4x4 luma cell.  Kept at 8 bytes so a consumer can upload the grid
 * directly as an aligned GPU buffer.
 */
struct MvCell {
    std::int16_t mv[2][2]{};       // [list][x/y], quarter-luma-sample
    std::int8_t ref_idx[2]{-1, -1};
    std::uint8_t flags = MvUnavailable;
    std::uint8_t reserved[5]{};
};

static_assert(sizeof(MvCell) == 16, "MvCell must stay 16 bytes for direct GPU upload");

/*
 * Optional partition view.  The parser's first zero-copy delivery writes the
 * dense 4x4 cells only; blocks are reserved for a later optional buffer.
 */
struct MotionBlock {
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint8_t w = 0;
    std::uint8_t h = 0;
    std::uint8_t list = 0;
    std::uint8_t flags = MvUnavailable;
    std::int16_t mv_x = 0;
    std::int16_t mv_y = 0;
    std::int8_t ref_idx = -1;
    std::uint8_t reserved[3]{};
};

static_assert(sizeof(MotionBlock) == 16, "MotionBlock must stay 16 bytes");

/*
 * Reference table for one slice of the picture.  Slice data writes ref_idx
 * relative to the list that was active for that slice, so the picture-level
 * table alone is not enough when slices use different reordering commands or
 * active counts.
 *
 * `mb_addr_begin` is the first macroblock address covered by the slice and
 * `mb_addr_end` is one past the last one, both in the same macroblock-address
 * units as H.264 CurrMbAddr (first_mb_in_slice * (1 + MbaffFrameFlag) for the
 * beginning).  The entries in the buffer are sorted by mb_addr_begin.
 *
 * These begin/end bounds describe the slice's syntax position.  They are
 * exact for contiguous (non-FMO) pictures but cannot describe a
 * non-contiguous slice group; use the MotionSliceRun map in the header to
 * resolve the slice of a macroblock in that case.
 */
struct MotionSliceReference {
    std::uint32_t mb_addr_begin = 0;
    std::uint32_t mb_addr_end = 0;
    std::uint8_t colour_plane_id = 0;
    std::uint8_t flags = 0;
    std::uint8_t num_ref_idx[2]{0, 0};
    /*
     * Bit i is set when ref_poc[list][i] is a non-existing frame generated by
     * the frame_num gap process.  The list still occupies its PicNum slot so
     * later ref_idx values map correctly, but it has no motion data.
     */
    std::uint32_t non_existing_mask[2]{0, 0};
    std::int32_t ref_poc[2][32]{};
};

static_assert(sizeof(MotionSliceReference) == 276,
              "MotionSliceReference must stay a 4-byte-aligned POD");

/*
 * One run of consecutive macroblock addresses that share a slice reference
 * table entry.  Runs are sorted by mb_addr and are non-overlapping; run i
 * covers `[mb_addr, next.mb_addr)` and the last run covers up to the header's
 * slice_map_coverage.  `slice_index` indexes the MotionSliceReference table;
 * kMotionSliceIndexNone marks a macroblock that was not coded by any slice
 * with a reference table (for example an intra slice).
 *
 * A contiguous picture needs one run per slice.  A non-contiguous FMO map
 * needs one run per contiguous address span within a slice, which is what
 * makes per-cell slice resolution exact.
 */
struct MotionSliceRun {
    std::uint32_t mb_addr = 0;
    std::uint32_t slice_index = 0;
};

static_assert(sizeof(MotionSliceRun) == 8, "MotionSliceRun must stay 8 bytes");

inline constexpr std::uint32_t kMotionSliceIndexNone =
        std::numeric_limits<std::uint32_t>::max();

struct MotionVectorBufferHeader {
    std::uint32_t magic = kMotionVectorBufferMagic;
    std::uint16_t version = kMotionVectorBufferVersion;
    std::uint16_t header_bytes = 0;

    std::uint32_t total_bytes = 0;
    std::uint32_t flags = 0;

    std::int32_t poc = 0;
    std::uint32_t frame_num = 0;

    std::uint32_t width = 0;   // cropped luma dimension
    std::uint32_t height = 0;  // cropped luma dimension
    std::uint32_t mb_width = 0;
    std::uint32_t mb_height = 0;
    std::uint32_t width4x4 = 0;
    std::uint32_t height4x4 = 0;

    std::uint32_t cell_count = 0;
    std::uint32_t cell_stride = sizeof(MvCell);
    std::uint32_t cells_offset = 0;

    std::uint32_t block_count = 0;
    std::uint32_t block_stride = sizeof(MotionBlock);
    std::uint32_t blocks_offset = 0;

    /*
     * Picture-level convenience copy of the *first* slice's reference table.
     * Use motionRefPocAt()/motionSliceReferenceAt() to resolve the table that
     * actually applies to a cell in a multi-slice picture.
     */
    std::int32_t ref_poc[2][32]{};
    std::uint8_t num_ref_idx[2]{0, 0};
    std::uint8_t status = static_cast<std::uint8_t>(MotionVectorStatus::Ok);
    /*
     * LongTermFrameIdx assigned to this picture, or -1 when the picture is
     * short-term (or not a reference picture).
     */
    std::int8_t long_term_frame_idx = -1;
    /*
     * Colour plane identity.  plane_count is 3 for a
     * separate_colour_plane_flag stream and 1 otherwise; colour_plane_id is
     * 0 for ordinary streams and 0/1/2 for the independent planes.
     */
    std::uint8_t colour_plane_id = 0;
    std::uint8_t plane_count = 1;

    /* Picture-level non-existing masks matching ref_poc/num_ref_idx above. */
    std::uint32_t ref_non_existing_mask[2]{0, 0};

    /*
     * Per-slice table (version 2+).  `slice_refs_offset` is relative to the
     * start of the buffer, `slice_ref_stride` is sizeof(MotionSliceReference),
     * and `slice_ref_count` entries are valid.  `slice_ref_capacity` records
     * the allocated slot count so a consumer can validate the offset.
     */
    std::uint32_t slice_ref_count = 0;
    std::uint32_t slice_ref_stride = sizeof(MotionSliceReference);
    std::uint32_t slice_refs_offset = 0;
    std::uint32_t slice_ref_capacity = 0;

    /*
     * Exact macroblock-to-slice map (version 3+).  `slice_map_offset` is
     * relative to the buffer start and `slice_map_stride` is
     * sizeof(MotionSliceRun).  `slice_map_count` sorted, non-overlapping runs
     * cover macroblock addresses [0, slice_map_coverage); each run's
     * slice_index indexes the MotionSliceReference table.  A picture without a
     * usable map (all-intra, or a consumer-built buffer) leaves count == 0 and
     * the helpers fall back to the begin/end ranges.
     */
    std::uint32_t slice_map_count = 0;
    std::uint32_t slice_map_stride = sizeof(MotionSliceRun);
    std::uint32_t slice_map_offset = 0;
    std::uint32_t slice_map_capacity = 0;
    std::uint32_t slice_map_coverage = 0;
    std::uint32_t slice_map_reserved = 0;
};

static_assert(std::is_standard_layout_v<MotionSliceReference> &&
                      std::is_trivially_copyable_v<MotionSliceReference>,
              "MotionSliceReference must remain a POD table entry");
static_assert(std::is_standard_layout_v<MotionSliceRun> &&
                      std::is_trivially_copyable_v<MotionSliceRun>,
              "MotionSliceRun must remain a POD run entry");
static_assert(std::is_standard_layout_v<MotionVectorBufferHeader> &&
                      std::is_trivially_copyable_v<MotionVectorBufferHeader>,
              "MotionVectorBufferHeader must remain a POD header");

/*
 * Memory the consumer must provide.  `required_bytes` is exact for the dense
 * grid path; the requested alignment is passed through so a GPU-mappable
 * allocation can be used directly.
 */
struct MotionVectorBufferRequest {
    std::uint32_t width4x4 = 0;
    std::uint32_t height4x4 = 0;
    std::uint32_t cell_count = 0;
    std::uint32_t cell_stride = sizeof(MvCell);
    std::size_t required_bytes = 0;
    std::size_t alignment = 64;
    std::int32_t poc = 0;
    std::uint32_t frame_num = 0;
    std::uint32_t flags = 0;
    std::uint8_t colour_plane_id = 0;
    std::uint8_t plane_count = 1;
};

/*
 * The lease is intentionally type-erased.  A consumer pool can wrap its own
 * slot bookkeeping in a shared_ptr with a custom deleter; the parser only
 * copies the lease when it needs to retain a reference picture.
 */
using MotionVectorBufferLease = std::shared_ptr<void>;

struct MotionVectorBuffer {
    void* data = nullptr;
    std::size_t capacity = 0;
    MotionVectorBufferLease lease;
};

struct MotionVectorBufferView {
    const MotionVectorBufferHeader* header = nullptr;
    const void* data = nullptr;
    std::size_t bytes = 0;
    MotionVectorBufferLease lease;
    bool parserRetains = false;
};

using MotionVectorBufferAcquire =
        MotionVectorBuffer (*)(void* ctx, const MotionVectorBufferRequest& request);

using MotionVectorBufferReady =
        void (*)(void* ctx, const MotionVectorBufferView& view);

struct MotionVectorOptions {
    bool emit_intra = true;
    bool require_idr_start = true;
    /*
     * Call MotionVectorExtractor::finish() at the end of State::parse():
     * flush the picture under construction and drain the deferred ready
     * queue.  With defer_ready == false this is just a final flush().
     */
    bool flush_on_parse_end = true;
    /*
     * Defer ready() until an IDR picture (or end of parsing) decides whether
     * no_output_of_prior_pics_flag suppresses the queued pictures.  Disable
     * for legacy immediate delivery, where no_output_of_prior_pics_flag is
     * necessarily ignored because a picture cannot be recalled after ready().
     */
    bool defer_ready = true;
    /*
     * Maximum number of pictures queued by defer_ready.  0 means no limit;
     * the queue is then bounded only by what the bitstream asks us to retain.
     * When non-zero, the oldest queued picture is delivered as output before
     * the queue can exceed the limit.
     */
    std::uint32_t max_deferred_pictures = 0;
    std::uint32_t max_macroblocks = 1u << 20;
    std::size_t buffer_alignment = 64;
};

struct MotionVectorSink {
    void* ctx = nullptr;
    MotionVectorBufferAcquire acquire = nullptr;
    MotionVectorBufferReady ready = nullptr;
    MotionVectorOptions options{};
};

struct MotionSample {
    float x = 0.0f;
    float y = 0.0f;
    bool valid = false;
};

[[nodiscard]]
inline bool motionVectorHeaderValid(const MotionVectorBufferHeader& header) noexcept {
    if (header.magic != kMotionVectorBufferMagic ||
        header.version != kMotionVectorBufferVersion) {
        return false;
    }
    if (header.header_bytes < sizeof(MotionVectorBufferHeader)) {
        return false;
    }
    if (header.colour_plane_id > 2 || header.plane_count == 0 ||
        header.plane_count > 3) {
        return false;
    }
    if (header.cell_count > 0 &&
        (header.cells_offset < header.header_bytes ||
         header.cell_stride != sizeof(MvCell))) {
        return false;
    }
    if (header.block_count > 0 &&
        (header.blocks_offset < header.header_bytes ||
         header.block_stride != sizeof(MotionBlock))) {
        return false;
    }
    if (header.slice_ref_count > header.slice_ref_capacity ||
        header.slice_ref_stride != sizeof(MotionSliceReference)) {
        return false;
    }
    if (header.slice_ref_capacity > 0 &&
        header.slice_refs_offset < header.header_bytes) {
        return false;
    }
    if (header.slice_ref_count > 0 && header.slice_ref_capacity == 0) {
        return false;
    }
    if (header.slice_map_count > header.slice_map_capacity ||
        header.slice_map_stride != sizeof(MotionSliceRun)) {
        return false;
    }
    if (header.slice_map_capacity > 0 &&
        header.slice_map_offset < header.header_bytes) {
        return false;
    }
    if (header.slice_map_count > 0 &&
        (header.slice_map_capacity == 0 || header.slice_map_coverage == 0)) {
        return false;
    }
    return true;
}

[[nodiscard]]
inline std::span<const MotionSliceReference> motionSliceReferences(
        const MotionVectorBufferView& view) noexcept {
    if (view.header == nullptr || view.data == nullptr ||
        !motionVectorHeaderValid(*view.header) || view.header->slice_ref_count == 0) {
        return {};
    }

    const auto* base = static_cast<const std::uint8_t*>(view.data);
    const std::size_t end = static_cast<std::size_t>(view.header->slice_refs_offset) +
                            static_cast<std::size_t>(view.header->slice_ref_count) *
                                    sizeof(MotionSliceReference);
    if (end > view.bytes) {
        return {};
    }

    return std::span<const MotionSliceReference>(
            reinterpret_cast<const MotionSliceReference*>(base + view.header->slice_refs_offset),
            view.header->slice_ref_count);
}

[[nodiscard]]
inline std::span<const MotionSliceRun> motionSliceRuns(
        const MotionVectorBufferView& view) noexcept {
    if (view.header == nullptr || view.data == nullptr ||
        !motionVectorHeaderValid(*view.header) || view.header->slice_map_count == 0) {
        return {};
    }

    const auto* base = static_cast<const std::uint8_t*>(view.data);
    const std::size_t end = static_cast<std::size_t>(view.header->slice_map_offset) +
                            static_cast<std::size_t>(view.header->slice_map_count) *
                                    sizeof(MotionSliceRun);
    if (end > view.bytes) {
        return {};
    }

    return std::span<const MotionSliceRun>(
            reinterpret_cast<const MotionSliceRun*>(base + view.header->slice_map_offset),
            view.header->slice_map_count);
}

/*
 * Resolve `mb_addr` through the exact macroblock map.  Returns true when the
 * map covers the address; `index` is then a MotionSliceReference index or
 * kMotionSliceIndexNone.  Returns false when no map exists or the address is
 * outside its coverage, in which case the caller may use the begin/end ranges.
 */
[[nodiscard]]
inline bool motionSliceIndexAt(
        const MotionVectorBufferView& view,
        std::uint32_t mb_addr,
        std::uint32_t& index) noexcept {
    if (view.header == nullptr) {
        return false;
    }
    const auto runs = motionSliceRuns(view);
    if (runs.empty() || mb_addr >= view.header->slice_map_coverage) {
        return false;
    }

    const auto it = std::upper_bound(
            runs.begin(), runs.end(), mb_addr,
            [](std::uint32_t address, const MotionSliceRun& run) {
                return address < run.mb_addr;
            });
    if (it == runs.begin()) {
        return false;
    }
    index = (it - 1)->slice_index;
    return true;
}

/*
 * Resolve the slice entry containing `mb_addr` (CurrMbAddr units).  The
 * version-3 macroblock map is authoritative when present, which makes the
 * lookup exact for non-contiguous FMO slice groups.  Without a map the sorted
 * begin/end ranges are searched, so consumer-built buffers and all-intra
 * pictures keep working.
 */
[[nodiscard]]
inline const MotionSliceReference* motionSliceReferenceAt(
        const MotionVectorBufferView& view,
        std::uint32_t mb_addr) noexcept {
    const auto slices = motionSliceReferences(view);
    if (slices.empty()) {
        return nullptr;
    }

    std::uint32_t index = 0;
    if (motionSliceIndexAt(view, mb_addr, index)) {
        if (index == kMotionSliceIndexNone || index >= slices.size()) {
            return nullptr;
        }
        return &slices[index];
    }

    const auto it = std::upper_bound(
            slices.begin(), slices.end(), mb_addr,
            [](std::uint32_t address, const MotionSliceReference& slice) {
                return address < slice.mb_addr_begin;
            });
    if (it == slices.begin()) {
        return nullptr;
    }
    const MotionSliceReference& candidate = *(it - 1);
    return mb_addr >= candidate.mb_addr_begin && mb_addr < candidate.mb_addr_end
                   ? &candidate
                   : nullptr;
}

/*
 * CurrMbAddr of the macroblock containing a 4x4 luma cell.  For an MBAFF
 * frame picture the grid is the frame grid while CurrMbAddr is
 * 2 * pair_index + parity, exactly the addressing used by the slice tables and
 * by FMO MapUnitToSliceGroupMap conversion.
 */
[[nodiscard]]
inline std::uint32_t motionMbAddressAt(
        const MotionVectorBufferHeader& picture,
        std::uint32_t x4,
        std::uint32_t y4) noexcept {
    if (picture.mb_width == 0 || x4 >= picture.width4x4 || y4 >= picture.height4x4) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    const std::uint32_t mb_x = x4 >> 2;
    const std::uint32_t frame_y = y4 >> 2;
    if ((picture.flags & MvPictureMbaff) != 0) {
        return ((frame_y >> 1) * picture.mb_width + mb_x) * 2u + (frame_y & 1u);
    }
    return frame_y * picture.mb_width + mb_x;
}

[[nodiscard]]
inline std::int32_t motionRefPocAt(
        const MotionVectorBufferView& view,
        std::uint32_t x4,
        std::uint32_t y4,
        int list,
        int ref_idx) noexcept {
    if (view.header == nullptr || list < 0 || list > 1 || ref_idx < 0 || ref_idx >= 32) {
        return kMotionVectorUnknownPoc;
    }

    const auto slices = motionSliceReferences(view);
    const MotionSliceReference* slice = nullptr;
    const std::uint32_t mb_addr = motionMbAddressAt(*view.header, x4, y4);
    if (mb_addr != std::numeric_limits<std::uint32_t>::max()) {
        std::uint32_t index = kMotionSliceIndexNone;
        if (motionSliceIndexAt(view, mb_addr, index)) {
            /*
             * The macroblock map is authoritative: an address that it maps
             * to kMotionSliceIndexNone was coded by a slice without a
             * reference table and must not fall back to the picture-level
             * first-slice copy.
             */
            if (index == kMotionSliceIndexNone || index >= slices.size()) {
                return kMotionVectorUnknownPoc;
            }
            slice = &slices[index];
        } else {
            slice = motionSliceReferenceAt(view, mb_addr);
        }
    }

    if (slice != nullptr) {
        if (ref_idx >= static_cast<int>(slice->num_ref_idx[list])) {
            return kMotionVectorUnknownPoc;
        }
        return slice->ref_poc[list][static_cast<std::size_t>(ref_idx)];
    }
    if (ref_idx >= static_cast<int>(view.header->num_ref_idx[list])) {
        return kMotionVectorUnknownPoc;
    }
    return view.header->ref_poc[list][static_cast<std::size_t>(ref_idx)];
}

[[nodiscard]]
inline bool motionRefNonExistingAt(
        const MotionVectorBufferView& view,
        std::uint32_t x4,
        std::uint32_t y4,
        int list,
        int ref_idx) noexcept {
    if (view.header == nullptr || list < 0 || list > 1 || ref_idx < 0 || ref_idx >= 32) {
        return false;
    }

    const auto slices = motionSliceReferences(view);
    const MotionSliceReference* slice = nullptr;
    const std::uint32_t mb_addr = motionMbAddressAt(*view.header, x4, y4);
    if (mb_addr != std::numeric_limits<std::uint32_t>::max()) {
        std::uint32_t index = kMotionSliceIndexNone;
        if (motionSliceIndexAt(view, mb_addr, index)) {
            if (index == kMotionSliceIndexNone || index >= slices.size()) {
                return false;
            }
            slice = &slices[index];
        } else {
            slice = motionSliceReferenceAt(view, mb_addr);
        }
    }

    const std::uint32_t mask = slice != nullptr
                                       ? slice->non_existing_mask[list]
                                       : view.header->ref_non_existing_mask[list];
    return (mask & (1u << static_cast<unsigned>(ref_idx))) != 0;
}

[[nodiscard]]
inline std::span<const MvCell> motionCells(const MotionVectorBufferView& view) noexcept {
    if (view.header == nullptr || view.data == nullptr || !motionVectorHeaderValid(*view.header)) {
        return {};
    }

    const auto* base = static_cast<const std::uint8_t*>(view.data);
    const std::size_t end = static_cast<std::size_t>(view.header->cells_offset) +
                            static_cast<std::size_t>(view.header->cell_count) *
                                    sizeof(MvCell);
    if (end > view.bytes) {
        return {};
    }

    return std::span<const MvCell>(
            reinterpret_cast<const MvCell*>(base + view.header->cells_offset),
            view.header->cell_count);
}

[[nodiscard]]
inline std::span<const MotionBlock> motionBlocks(const MotionVectorBufferView& view) noexcept {
    if (view.header == nullptr || view.data == nullptr || !motionVectorHeaderValid(*view.header) ||
        view.header->block_count == 0) {
        return {};
    }

    const auto* base = static_cast<const std::uint8_t*>(view.data);
    const std::size_t end = static_cast<std::size_t>(view.header->blocks_offset) +
                            static_cast<std::size_t>(view.header->block_count) *
                                    sizeof(MotionBlock);
    if (end > view.bytes) {
        return {};
    }

    return std::span<const MotionBlock>(
            reinterpret_cast<const MotionBlock*>(base + view.header->blocks_offset),
            view.header->block_count);
}

[[nodiscard]]
inline const MvCell* motionCellAt(
        const MotionVectorBufferView& view,
        std::uint32_t x4,
        std::uint32_t y4) noexcept {
    if (view.header == nullptr || x4 >= view.header->width4x4 || y4 >= view.header->height4x4) {
        return nullptr;
    }
    const auto cells = motionCells(view);
    const std::size_t index = static_cast<std::size_t>(y4) * view.header->width4x4 + x4;
    return index < cells.size() ? &cells[index] : nullptr;
}

namespace detail {

template <typename RefPoc, typename RefNonExisting>
[[nodiscard]]
inline MotionSample motionToResolved(
        const MvCell& cell,
        std::int32_t target_poc,
        std::int32_t picture_poc,
        RefPoc&& ref_poc,
        RefNonExisting&& ref_non_existing) noexcept {
    MotionSample result{};
    if (target_poc == picture_poc) {
        result.valid = true;
        return result;
    }

    int best_list = -1;
    std::int64_t best_distance = std::numeric_limits<std::int64_t>::max();
    bool best_same_side = false;

    for (int list = 0; list < 2; ++list) {
        const std::int8_t ref_idx = cell.ref_idx[list];
        if (ref_idx < 0 || ref_idx >= 32 || ref_non_existing(list, ref_idx)) {
            continue;
        }
        const std::int32_t list_poc = ref_poc(list, ref_idx);
        if (list_poc == kMotionVectorUnknownPoc || list_poc == picture_poc) {
            continue;
        }
        const std::int64_t distance =
                std::llabs(static_cast<std::int64_t>(list_poc) - target_poc);
        const bool same_side =
                ((target_poc > picture_poc) == (list_poc > picture_poc));
        if (best_list < 0 || (same_side && !best_same_side) ||
            (same_side == best_same_side && distance < best_distance)) {
            best_list = list;
            best_distance = distance;
            best_same_side = same_side;
        }
    }

    if (best_list < 0) {
        return result;
    }

    const std::int8_t ref_idx = cell.ref_idx[best_list];
    const std::int32_t ref_poc_value = ref_poc(best_list, ref_idx);
    const double denom = static_cast<double>(ref_poc_value - picture_poc);
    if (denom == 0.0) {
        return result;
    }

    const double scale = static_cast<double>(target_poc - picture_poc) / denom;
    result.x = static_cast<float>(cell.mv[best_list][0] / 4.0 * scale);
    result.y = static_cast<float>(cell.mv[best_list][1] / 4.0 * scale);
    result.valid = std::isfinite(result.x) && std::isfinite(result.y);
    return result;
}

}  // namespace detail

/*
 * Project a cell's motion onto `target_poc` using the picture-level table.
 * This overload cannot know which slice a cell belongs to; for multi-slice
 * pictures use the (view, x4, y4) overload below, which resolves the exact
 * per-slice table.
 */
[[nodiscard]]
inline MotionSample motionTo(
        const MvCell& cell,
        std::int32_t target_poc,
        const MotionVectorBufferHeader& picture) noexcept {
    return detail::motionToResolved(
            cell, target_poc, picture.poc,
            [&picture](int list, int index) noexcept {
                return picture.ref_poc[list][static_cast<std::size_t>(index)];
            },
            [&picture](int list, int index) noexcept {
                return (picture.ref_non_existing_mask[list] &
                        (1u << static_cast<unsigned>(index))) != 0;
            });
}

/*
 * Per-slice-aware projection.  The cell at (x4, y4) is resolved through the
 * slice table written by the extractor, so different slices of one picture
 * may legitimately use different reference POCs for the same ref_idx.
 */
[[nodiscard]]
inline MotionSample motionTo(
        const MotionVectorBufferView& view,
        std::uint32_t x4,
        std::uint32_t y4,
        std::int32_t target_poc) noexcept {
    const MvCell* cell = motionCellAt(view, x4, y4);
    if (cell == nullptr || view.header == nullptr) {
        return {};
    }
    return detail::motionToResolved(
            *cell, target_poc, view.header->poc,
            [&view, x4, y4](int list, int index) noexcept {
                return motionRefPocAt(view, x4, y4, list, index);
            },
            [&view, x4, y4](int list, int index) noexcept {
                return motionRefNonExistingAt(view, x4, y4, list, index);
            });
}

}  // namespace avc
}  // namespace bs
