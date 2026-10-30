// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * Synthetic coverage for the parts of the MV-DPB that the older fixtures did
 * not exercise:
 *
 *   avc_frame_num_gap_cavlc.h264
 *       progressive frame_num gap, non-existing reference slots, a B picture
 *       whose list 1 contains the non-existing frame, and zeroed motion for
 *       cells that select it;
 *   avc_frame_num_gap_field_cavlc.h264
 *       the same gap process across a PAFF complementary field pair;
 *   avc_multislice_refs_cavlc.h264
 *       two slices in one picture with different reordering/active counts;
 *   avc_no_output_prior_cavlc.h264
 *       IDR no_output_of_prior_pics_flag delivery suppression.
 */

#include "bsparser.hpp"
#include "utils/MotionBufferPool.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Picture {
    bs::avc::MotionVectorBufferHeader header{};
    std::vector<bs::avc::MotionSliceReference> slices;
    std::vector<bs::avc::MvCell> cells;

    [[nodiscard]] int refPocAt(std::uint32_t x4, std::uint32_t y4) const {
        if (cells.empty() || header.mb_width == 0) return 0;
        const std::uint32_t index =
                static_cast<std::size_t>(y4) * header.width4x4 + x4;
        if (index >= cells.size()) return 0;
        const std::int8_t ref = cells[index].ref_idx[0];
        if (ref < 0) return 0;
        const std::uint32_t mb_addr = (y4 >> 2) * header.mb_width + (x4 >> 2);
        for (const auto& slice : slices) {
            if (mb_addr >= slice.mb_addr_begin && mb_addr < slice.mb_addr_end) {
                return ref < static_cast<std::int8_t>(slice.num_ref_idx[0])
                               ? slice.ref_poc[0][static_cast<std::size_t>(ref)]
                               : 0;
            }
        }
        return header.ref_poc[0][static_cast<std::size_t>(ref)];
    }
};

struct Context {
    bs::utils::MotionBufferPool* pool = nullptr;
    std::vector<Picture> pictures;
};

bs::avc::MotionVectorBuffer acquire_thunk(
        void* context, const bs::avc::MotionVectorBufferRequest& request) {
    return static_cast<Context*>(context)->pool->acquire(request);
}

void ready(void* context, const bs::avc::MotionVectorBufferView& view) {
    auto* ctx = static_cast<Context*>(context);
    Picture picture;
    if (view.header != nullptr && bs::avc::motionVectorHeaderValid(*view.header)) {
        picture.header = *view.header;
        const auto cells = bs::avc::motionCells(view);
        picture.cells.assign(cells.begin(), cells.end());
        const auto slices = bs::avc::motionSliceReferences(view);
        picture.slices.assign(slices.begin(), slices.end());
    } else {
        picture.header.status = 254;
    }
    ctx->pictures.push_back(std::move(picture));
}

std::vector<std::uint8_t> read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(1);
    }
    return std::vector<std::uint8_t>(
            std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

int failures = 0;

void expect(bool condition, const char* message, const char* detail = "") {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s%s%s\n", message, detail[0] != '\0' ? " " : "", detail);
        ++failures;
    }
}

std::string describe(const Picture& picture, std::size_t index) {
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "picture=%zu poc=%d frame_num=%u status=%u",
                  index, picture.header.poc, picture.header.frame_num,
                  picture.header.status);
    return std::string(buffer);
}

void expect_list(const Picture& picture, std::size_t index, int list,
                 const std::vector<std::int32_t>& expected) {
    const std::string detail = describe(picture, index);
    if (picture.header.num_ref_idx[list] != expected.size()) {
        expect(false, "reference-list length", detail.c_str());
        return;
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (picture.header.ref_poc[list][i] != expected[i]) {
            expect(false, "reference-list POC", detail.c_str());
            return;
        }
    }
}

std::size_t count_flag(const Picture& picture, std::uint8_t flag) {
    return static_cast<std::size_t>(std::count_if(
            picture.cells.begin(), picture.cells.end(),
            [flag](const bs::avc::MvCell& cell) { return (cell.flags & flag) != 0; }));
}

std::vector<Picture> run_stream(const char* path, bool defer_ready = true) {
    const auto bytes = read_file(path);
    bs::utils::MotionBufferPool pool({4, 32, 32u << 20, 64});
    Context context{&pool, {}};

    auto state = bs::create_state(bs::Codec::Avc);
    bs::AvcParsedHandlers handlers{};
    handlers.motion_vectors.ctx = &context;
    handlers.motion_vectors.acquire = &acquire_thunk;
    handlers.motion_vectors.ready = &ready;
    handlers.motion_vectors.options.emit_intra = true;
    handlers.motion_vectors.options.defer_ready = defer_ready;
    (void)bs::parse(*state,
                    std::span<const std::uint8_t>(bytes.data(), bytes.size()),
                    bs::NalFramingMode::AnnexB,
                    handlers,
                    4);

    expect(state->avc_motion_vectors() != nullptr, "motion-vector extractor exists", path);
    if (state->avc_motion_vectors() != nullptr) {
        expect(state->avc_motion_vectors()->deferredPictureCount() == 0,
               "parse end drains the deferred queue", path);
    }
    state->clear();
    pool.trimAll();
    expect(pool.stats().buffersInUse == 0, "clearing releases all leases", path);
    return context.pictures;
}

void test_gap(const char* path) {
    const auto pictures = run_stream(path);
    expect(pictures.size() == 6, "gap fixture picture count", path);
    if (pictures.size() != 6) return;

    const std::vector<std::int32_t> pocs = {0, 2, 5, 6, 8, 14};
    for (std::size_t i = 0; i < pictures.size(); ++i) {
        expect(pictures[i].header.status ==
                       static_cast<std::uint8_t>(bs::avc::MotionVectorStatus::Ok),
               "gap picture parses completely", describe(pictures[i], i).c_str());
        expect(pictures[i].header.poc == pocs[i], "gap picture POC",
               describe(pictures[i], i).c_str());
    }

    /* P3 sees the non-existing fn2 (POC 4) before P1 (POC 2). */
    expect_list(pictures[3], 3, 0, {4, 2});
    expect((pictures[3].header.ref_non_existing_mask[0] & 1u) != 0,
           "P3 list 0 marks the non-existing fn2");
    expect((pictures[3].header.flags & bs::avc::MvPictureHasNonExistingRef) != 0,
           "P3 picture is flagged as containing a non-existing reference");
    expect(count_flag(pictures[3], bs::avc::MvRefNonExisting) > 0,
           "P3 cells selecting the non-existing frame are marked");
    expect(count_flag(pictures[3], bs::avc::MvUnavailable) == pictures[3].cells.size(),
           "P3 cells selecting the non-existing frame carry no MV data");

    /* P4 keeps the slot: fn3 (6), fn2 (4), fn1 (2). */
    expect_list(pictures[4], 4, 0, {6, 4, 2});
    expect((pictures[4].header.ref_non_existing_mask[0] & 2u) != 0,
           "P4 list 0 keeps the non-existing fn2 slot");
    expect(count_flag(pictures[4], bs::avc::MvRefNonExisting) == 0,
           "P4 cells select the real fn3 and are usable");

    /* P7 jumps over fn5 and fn6; both synthetic slots are kept in PicNum order. */
    expect_list(pictures[5], 5, 0, {12, 10, 8, 6});
    expect(pictures[5].header.ref_non_existing_mask[0] == 0x3u,
           "P7 list 0 marks both consecutive non-existing frames");
    expect(count_flag(pictures[5], bs::avc::MvRefNonExisting) > 0,
           "P7 cells selecting the first non-existing frame are marked");

    /* B list 1 contains fn2 at index 0 and is sanitized. */
    expect_list(pictures[2], 2, 1, {4, 2});
    expect((pictures[2].header.ref_non_existing_mask[1] & 1u) != 0,
           "B list 1 marks the non-existing colocated slot");
    expect(count_flag(pictures[2], bs::avc::MvRefNonExisting) > 0,
           "B direct cells selecting the non-existing list slot are marked");
}

void test_gap_poc0(const char* path) {
    const auto pictures = run_stream(path);
    expect(pictures.size() == 5, "POC-type-0 gap fixture picture count", path);
    if (pictures.size() != 5) return;

    /* B lists omit the non-existing frame (8.2.4.2.3). */
    expect_list(pictures[2], 2, 0, {2, 0});
    expect_list(pictures[2], 2, 1, {0, 2});
    expect(pictures[2].header.ref_non_existing_mask[0] == 0 &&
                   pictures[2].header.ref_non_existing_mask[1] == 0,
           "POC-type-0 B lists omit the non-existing frame");

    /* The P list keeps the slot, with the explicit unknown-POC sentinel. */
    expect_list(pictures[3], 3, 0,
                {bs::avc::kMotionVectorUnknownPoc, 2, 0});
    expect((pictures[3].header.ref_non_existing_mask[0] & 1u) != 0,
           "POC-type-0 P list marks the unknown slot");
    expect(count_flag(pictures[3], bs::avc::MvRefNonExisting) > 0,
           "POC-type-0 cells selecting the non-existing frame are marked");
    expect(count_flag(pictures[3], bs::avc::MvUnavailable) == pictures[3].cells.size(),
           "POC-type-0 non-existing selection carries no MV data");
}

void test_gap_field(const char* path) {
    const auto pictures = run_stream(path);
    expect(pictures.size() == 6, "field-gap fixture picture count", path);
    if (pictures.size() != 6) return;

    const std::vector<std::vector<std::int32_t>> expected = {
            {}, {}, {0, 0}, {0, 2, 0}, {4, 4, 2, 2, 0, 0},
            {4, 6, 2, 4, 0, 2}};
    for (std::size_t i = 0; i < pictures.size(); ++i) {
        expect(pictures[i].header.status ==
                       static_cast<std::uint8_t>(bs::avc::MotionVectorStatus::Ok),
               "field-gap picture parses completely", describe(pictures[i], i).c_str());
        expect((pictures[i].header.flags & bs::avc::MvPictureField) != 0,
               "field-gap picture is a field picture");
        expect_list(pictures[i], i, 0, expected[i]);
    }

    /* Both fields of the synthesized frame occupy slots 0 and 3. */
    expect(pictures[4].header.ref_non_existing_mask[0] == 0x3u,
           "top field list marks both non-existing fields");
    expect(pictures[5].header.ref_non_existing_mask[0] == 0x9u,
           "bottom field list marks both non-existing fields");
    for (std::size_t i = 4; i < 6; ++i) {
        expect(count_flag(pictures[i], bs::avc::MvRefNonExisting) > 0,
               "field cells selecting the non-existing pair are marked");
        expect(count_flag(pictures[i], bs::avc::MvUnavailable) ==
                       pictures[i].cells.size(),
               "field cells selecting the non-existing pair carry no MV data");
    }
}

void test_multislice_refs(const char* path) {
    const auto pictures = run_stream(path);
    expect(pictures.size() == 5, "multislice fixture picture count", path);
    if (pictures.size() != 5) return;

    const Picture& picture = pictures[4];
    expect(picture.header.status ==
                   static_cast<std::uint8_t>(bs::avc::MotionVectorStatus::Ok),
           "multislice picture parses completely");
    expect(picture.slices.size() == 2, "multislice picture exposes two slice tables");
    if (picture.slices.size() != 2) return;

    /* Picture-level convenience copy comes from the first slice. */
    expect(picture.header.num_ref_idx[0] == 2,
           "picture-level list comes from the first slice");
    expect(picture.header.ref_poc[0][0] == 4 && picture.header.ref_poc[0][1] == 6,
           "picture-level POCs come from the first slice");

    const auto& first = picture.slices[0];
    const auto& second = picture.slices[1];
    expect(first.mb_addr_begin == 0 && first.mb_addr_end == 2,
           "first slice covers macroblocks 0-1");
    expect(first.num_ref_idx[0] == 2, "first slice activates two references");
    expect(first.ref_poc[0][0] == 4 && first.ref_poc[0][1] == 6,
           "first slice reordering places fn2 first");
    expect(second.mb_addr_begin == 2 && second.mb_addr_end == 4,
           "second slice covers macroblocks 2-3");
    expect(second.num_ref_idx[0] == 1, "second slice activates one reference");
    expect(second.ref_poc[0][0] == 6, "second slice keeps fn3");

    /* The same ref_idx 0 resolves through each slice's own table. */
    expect(picture.refPocAt(0, 0) == 4, "cell in slice 0 resolves to POC 4");
    expect(picture.refPocAt(0, 4) == 6, "cell in slice 1 resolves to POC 6");
    expect(picture.refPocAt(7, 3) == 4, "last cell of slice 0 resolves to POC 4");
    expect(picture.refPocAt(7, 4) == 6, "first cell of slice 1's second row resolves to POC 6");

    /* The same table must be written on the immediate (legacy) ready path. */
    const auto immediate = run_stream(path, false);
    expect(immediate.size() == 5, "immediate multislice picture count", path);
    if (immediate.size() == 5) {
        expect(immediate[4].slices.size() == 2,
               "immediate path writes both slice tables");
        expect(immediate[4].refPocAt(0, 0) == 4 && immediate[4].refPocAt(0, 4) == 6,
               "immediate path maps each slice's ref_idx");
    }
}

void test_no_output_prior(const char* path) {
    const auto pictures = run_stream(path);
    /*
     * IDR0/P1/P2 are suppressed by the second IDR.  IDR2 (flag 0) flushes
     * IDR1/P1; the final IDR2/P1 pair is flushed at parse end.
     */
    expect(pictures.size() == 4, "no-output fixture delivers only surviving pictures", path);
    if (pictures.size() != 4) return;

    const std::vector<std::int32_t> pocs = {0, 2, 0, 2};
    for (std::size_t i = 0; i < pictures.size(); ++i) {
        expect(pictures[i].header.status ==
                       static_cast<std::uint8_t>(bs::avc::MotionVectorStatus::Ok),
               "no-output surviving picture parses completely",
               describe(pictures[i], i).c_str());
        expect(pictures[i].header.poc == pocs[i], "no-output surviving POC",
               describe(pictures[i], i).c_str());
    }
    expect((pictures[0].header.flags & bs::avc::MvPictureIdr) != 0,
           "first surviving picture is the suppressing IDR");
    expect((pictures[2].header.flags & bs::avc::MvPictureIdr) != 0,
           "third surviving picture is the flag-0 IDR");
    expect(pictures[0].header.frame_num == 0 && pictures[1].header.frame_num == 1 &&
                   pictures[2].header.frame_num == 0 && pictures[3].header.frame_num == 1,
           "no-output fixture keeps the second GOP only");
}

/*
 * Direct-mode helper coverage: a MotionReferencePicture must resolve the
 * co-located cell's slice through the exact FMO map, including MBAFF
 * addressing, rather than through the picture-level first-slice copy.
 */
void test_reference_slice_map() {
    bs::avc::MotionSliceReference slices[2]{};
    slices[0].mb_addr_begin = 0;
    slices[0].mb_addr_end = 4;
    slices[0].num_ref_idx[0] = 1;
    slices[0].ref_poc[0][0] = 4;
    slices[1].mb_addr_begin = 0;
    slices[1].mb_addr_end = 4;
    slices[1].num_ref_idx[0] = 1;
    slices[1].ref_poc[0][0] = 2;

    bs::avc::MotionSliceRun runs[2]{};
    runs[0] = bs::avc::MotionSliceRun{0, 0};
    runs[1] = bs::avc::MotionSliceRun{1, 1};

    bs::avc::MotionReferencePicture reference{};
    reference.width4x4 = 16;
    reference.height4x4 = 16;
    reference.poc = 6;
    reference.slice_refs = slices;
    reference.slice_ref_count = 2;
    reference.slice_map = runs;
    reference.slice_map_count = 2;
    reference.slice_map_coverage = 4;
    reference.num_ref_idx[0] = 1;
    reference.ref_poc[0][0] = 4;

    expect(reference.refPocAt(0, 0, 0, 0) == 4,
           "reference picture maps macroblock 0 to its slice");
    expect(reference.refPocAt(4, 0, 0, 0) == 2,
           "reference picture does not fall back to the first slice");
    expect(reference.refPocAt(8, 0, 0, 0) == 2,
           "reference picture map covers every non-contiguous macroblock");
    expect(reference.refPocAt(0, 0, 0, 1) == bs::avc::kMotionVectorUnknownPoc,
           "reference picture inactive index is unknown");

    reference.mbaff = true;
    expect(reference.refPocAt(0, 0, 0, 0) == 4,
           "MBAFF reference top-left macroblock address");
    expect(reference.refPocAt(0, 4, 0, 0) == 2,
           "MBAFF reference bottom-left macroblock address");
    expect(reference.refPocAt(4, 0, 0, 0) == 2,
           "MBAFF reference top-right macroblock address");
    expect(reference.refPocAt(4, 4, 0, 0) == 2,
           "MBAFF reference bottom-right macroblock address");

    slices[1].non_existing_mask[0] = 1;
    expect(reference.refNonExistingAt(4, 0, 0, 0),
           "reference picture applies the mapped slice's non-existing mask");
    expect(!reference.refNonExistingAt(0, 0, 0, 0),
           "reference picture keeps slice 0's mask separate");
}

/*
 * H.264 data partitioning (NAL unit types 2-4) cannot be combined into one
 * macroblock grid by this extractor.  Rewrite the first VCL NAL of a valid
 * stream to partition A and make sure the extractor reports Unsupported
 * instead of delivering a partial picture.
 */
void test_data_partition_unsupported(const char* path) {
    std::vector<std::uint8_t> bytes = read_file(path);
    bool changed = false;
    for (std::size_t i = 0; i + 3 < bytes.size(); ++i) {
        if (bytes[i] != 0 || bytes[i + 1] != 0 || bytes[i + 2] != 1) {
            continue;
        }
        const std::size_t header = i + 3;
        if (header >= bytes.size()) {
            break;
        }
        const std::uint8_t type = bytes[header] & 0x1fu;
        if (type != 1u && type != 5u) {
            i = header;
            continue;
        }
        bytes[header] = static_cast<std::uint8_t>((bytes[header] & 0xe0u) | 0x02u);
        std::size_t end = bytes.size();
        for (std::size_t j = header + 1; j + 3 < bytes.size(); ++j) {
            if (bytes[j] == 0 && bytes[j + 1] == 0 &&
                (bytes[j + 2] == 1 || (bytes[j + 2] == 0 && bytes[j + 3] == 1))) {
                end = j;
                break;
            }
        }
        bytes.resize(end);
        changed = true;
        break;
    }
    expect(changed, "data-partition fixture has a VCL NAL to rewrite", path);
    if (!changed) {
        return;
    }

    bs::utils::MotionBufferPool pool({4, 8, 8u << 20, 64});
    Context context{&pool, {}};
    auto state = bs::create_state(bs::Codec::Avc);
    bs::AvcParsedHandlers handlers{};
    handlers.motion_vectors.ctx = &context;
    handlers.motion_vectors.acquire = &acquire_thunk;
    handlers.motion_vectors.ready = &ready;
    (void)bs::parse(*state,
                    std::span<const std::uint8_t>(bytes.data(), bytes.size()),
                    bs::NalFramingMode::AnnexB,
                    handlers,
                    4);

    auto* extractor = state->avc_motion_vectors();
    expect(extractor != nullptr, "data-partition extractor exists", path);
    expect(extractor != nullptr &&
                   extractor->status() == bs::avc::MotionVectorStatus::Unsupported,
           "data partitioning reports Unsupported", path);
    expect(context.pictures.empty(), "data partitioning delivers no partial grid", path);

    state->clear();
    pool.trimAll();
    expect(pool.stats().buffersInUse == 0, "data-partition clear releases leases", path);
}

}  // namespace

int main(int argc, char** argv) {
    test_reference_slice_map();

    if (argc < 6) {
        std::fprintf(stderr,
                     "usage: avc_mv_ext_test <gap.h264> <gap_field.h264> "
                     "<gap_poc0.h264> <multislice_refs.h264> "
                     "<no_output_prior.h264>\n");
        return 1;
    }

    test_gap(argv[1]);
    test_gap_field(argv[2]);
    test_gap_poc0(argv[3]);
    test_multislice_refs(argv[4]);
    test_no_output_prior(argv[5]);
    test_data_partition_unsupported(argv[1]);

    if (failures != 0) {
        std::fprintf(stderr, "%d extended motion-vector failure(s)\n", failures);
        return 1;
    }

    std::puts("avc motion-vector extended: ok");
    return 0;
}
