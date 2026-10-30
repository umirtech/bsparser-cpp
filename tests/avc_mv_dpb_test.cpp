// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * H.264 reference-picture-management tests.
 *
 * The first fixture is an x264 multi-reference/B-frame stress stream whose
 * P-picture list-0 motion is compared with FFmpeg's exported MV grid.  The
 * remaining fixtures are synthetic because x264 does not emit MMCO/long-term
 * syntax or PAFF:
 *
 *   - avc_mmco_cavlc.h264: sliding window, adaptive marking operations 1-6,
 *     IDR long-term marking and DPB reset;
 *   - avc_field_reorder_cavlc.h264: field-list reordering by opposite parity,
 *     long-term field reordering and frame lists with a long-term pair;
 *   - avc_frame_num_wrap_cavlc.h264: FrameNumWrap/PicNum arithmetic across a
 *     log2_max_frame_num wrap;
 *   - avc_frame_num_gap_cavlc.h264: frame_num gap synthesis, including a
 *     B picture whose list 1 contains the non-existing frame (detailed
 *     assertions live in avc_mv_ext_test.cpp).
 */

#include "bsparser.hpp"
#include "utils/MotionBufferPool.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Picture {
    std::int32_t poc = 0;
    std::uint32_t frame_num = 0;
    std::uint32_t flags = 0;
    std::uint8_t status = 255;
    std::int8_t long_term_frame_idx = -1;
    std::uint8_t num_ref_idx[2]{0, 0};
    std::int32_t ref_poc[2][32]{};
    std::uint32_t width4x4 = 0;
    std::uint32_t height4x4 = 0;
    std::vector<bs::avc::MvCell> cells;
};

struct Context {
    bs::utils::MotionBufferPool* pool = nullptr;
    std::vector<Picture> pictures;
    int direct_cells = 0;
};

bs::avc::MotionVectorBuffer acquire_thunk(
        void* context, const bs::avc::MotionVectorBufferRequest& request) {
    return static_cast<Context*>(context)->pool->acquire(request);
}

void ready(void* context, const bs::avc::MotionVectorBufferView& view) {
    auto* ctx = static_cast<Context*>(context);
    Picture picture;
    if (view.header != nullptr && bs::avc::motionVectorHeaderValid(*view.header)) {
        picture.poc = view.header->poc;
        picture.frame_num = view.header->frame_num;
        picture.flags = view.header->flags;
        picture.status = view.header->status;
        picture.long_term_frame_idx = view.header->long_term_frame_idx;
        picture.width4x4 = view.header->width4x4;
        picture.height4x4 = view.header->height4x4;
        for (int list = 0; list < 2; ++list) {
            picture.num_ref_idx[list] = view.header->num_ref_idx[list];
            for (std::size_t i = 0; i < 32; ++i) {
                picture.ref_poc[list][i] = view.header->ref_poc[list][i];
            }
        }
        const auto cells = bs::avc::motionCells(view);
        picture.cells.assign(cells.begin(), cells.end());
        for (const bs::avc::MvCell& cell : cells) {
            if (cell.flags & bs::avc::MvDirect) ++ctx->direct_cells;
        }
    } else {
        picture.status = 254;
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

struct ReferenceGrid {
    std::uint32_t width4x4 = 0;
    std::uint32_t height4x4 = 0;
    std::vector<std::array<std::int16_t, 2>> mv;
};

std::vector<ReferenceGrid> read_mv_reference(const char* path) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(1);
    }
    std::vector<ReferenceGrid> pictures;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream header(line);
        std::string kind;
        header >> kind;
        if (kind != "picture") continue;
        ReferenceGrid grid;
        int index = 0;
        header >> index >> grid.width4x4 >> grid.height4x4;
        grid.mv.resize(static_cast<std::size_t>(grid.width4x4) * grid.height4x4);
        for (auto& mv : grid.mv) {
            if (!std::getline(in, line)) {
                std::fprintf(stderr, "truncated motion-vector reference %s\n", path);
                std::exit(1);
            }
            std::istringstream entry(line);
            int x = 0;
            int y = 0;
            int dx = 0;
            int dy = 0;
            entry >> x >> y >> dx >> dy;
            mv = {static_cast<std::int16_t>(dx), static_cast<std::int16_t>(dy)};
        }
        pictures.push_back(std::move(grid));
    }
    return pictures;
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
                  index, picture.poc, picture.frame_num, picture.status);
    return std::string(buffer);
}

void expect_list_vector(const Picture& picture, std::size_t index, int list,
                        const std::vector<std::int32_t>& expected) {
    const std::string detail = describe(picture, index);
    if (picture.num_ref_idx[list] != expected.size()) {
        expect(false, "reference-list length", detail.c_str());
        return;
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (picture.ref_poc[list][i] != expected[i]) {
            expect(false, "reference-list POC", detail.c_str());
            return;
        }
    }
}

void expect_list(const Picture& picture, std::size_t index, int list,
                 std::initializer_list<std::int32_t> expected) {
    expect_list_vector(picture, index, list,
                       std::vector<std::int32_t>(expected.begin(), expected.end()));
}

std::vector<Picture> run_stream(const char* path, std::size_t extra_megabytes,
                                int* direct_cells = nullptr,
                                std::size_t* reference_count = nullptr,
                                int* max_long_term = nullptr,
                                bs::avc::MotionVectorStatus* final_status = nullptr) {
    const auto bytes = read_file(path);
    bs::utils::MotionBufferPool pool(
            {8, 40, static_cast<std::size_t>(extra_megabytes) << 20, 64});
    Context context{&pool, {}, 0};

    auto state = bs::create_state(bs::Codec::Avc);
    bs::AvcParsedHandlers handlers{};
    handlers.motion_vectors.ctx = &context;
    handlers.motion_vectors.acquire = &acquire_thunk;
    handlers.motion_vectors.ready = &ready;
    handlers.motion_vectors.options.emit_intra = true;
    (void)bs::parse(*state,
                    std::span<const std::uint8_t>(bytes.data(), bytes.size()),
                    bs::NalFramingMode::AnnexB,
                    handlers,
                    4);

    expect(state->avc_motion_vectors() != nullptr, "motion-vector extractor exists",
           path);
    if (direct_cells != nullptr) *direct_cells = context.direct_cells;
    if (state->avc_motion_vectors() != nullptr) {
        if (reference_count != nullptr) {
            *reference_count = state->avc_motion_vectors()->referencePictureCount();
        }
        if (max_long_term != nullptr) {
            *max_long_term = state->avc_motion_vectors()->maxLongTermFrameIdx();
        }
        if (final_status != nullptr) {
            *final_status = state->avc_motion_vectors()->status();
        }
    }

    state->clear();
    pool.trimAll();
    expect(pool.stats().buffersInUse == 0, "clearing the DPB releases all leases", path);
    return context.pictures;
}

void test_multiref_stress(const char* stream, const char* reference) {
    int direct_cells = 0;
    const auto pictures = run_stream(stream, 32, &direct_cells);
    expect(!pictures.empty(), "multi-reference stream emits pictures", stream);
    if (pictures.empty()) return;

    bool all_ok = true;
    int b_pictures = 0;
    for (const Picture& picture : pictures) {
        all_ok = all_ok && picture.status ==
                                    static_cast<std::uint8_t>(bs::avc::MotionVectorStatus::Ok);
        if (picture.num_ref_idx[1] > 0) ++b_pictures;
    }
    expect(all_ok, "every multi-reference picture parses completely", stream);
    expect(pictures.size() == 60, "multi-reference picture count", stream);
    expect(b_pictures > 0, "multi-reference stream contains B pictures", stream);
    expect(direct_cells > 0, "multi-reference B pictures contain direct cells", stream);

    std::vector<Picture> p_pictures;
    for (const Picture& picture : pictures) {
        if ((picture.flags & bs::avc::MvPictureIntra) == 0 &&
            picture.num_ref_idx[1] == 0) {
            p_pictures.push_back(picture);
        }
    }

    const auto expected = read_mv_reference(reference);
    expect(!expected.empty(), "multi-reference motion reference is populated", reference);
    expect(p_pictures.size() >= expected.size(),
           "multi-reference stream exposes the referenced P pictures", stream);

    const std::size_t count = std::min(expected.size(), p_pictures.size());
    for (std::size_t i = 0; i < count; ++i) {
        const Picture& actual = p_pictures[i];
        const ReferenceGrid& want = expected[i];
        const std::string detail = "reference picture " + std::to_string(i);
        expect(actual.width4x4 == want.width4x4 &&
                       actual.height4x4 == want.height4x4,
               "multi-reference P-picture dimensions match FFmpeg", detail.c_str());
        if (actual.width4x4 != want.width4x4 || actual.height4x4 != want.height4x4) {
            continue;
        }
        expect(actual.cells.size() == want.mv.size(),
               "multi-reference P-picture cell count matches FFmpeg", detail.c_str());
        if (actual.cells.size() != want.mv.size()) continue;

        bool match = true;
        for (std::size_t c = 0; c < want.mv.size(); ++c) {
            if (actual.cells[c].mv[0][0] != want.mv[c][0] ||
                actual.cells[c].mv[0][1] != want.mv[c][1]) {
                match = false;
                break;
            }
        }
        expect(match, "multi-reference P-picture list-0 motion matches FFmpeg",
               detail.c_str());
    }
}

void test_mmco(const char* path) {
    std::size_t reference_count = 0;
    int max_long_term = -2;
    const auto pictures =
            run_stream(path, 8, nullptr, &reference_count, &max_long_term);
    expect(pictures.size() == 18, "MMCO fixture picture count", path);
    if (pictures.size() != 18) return;

    const std::vector<std::vector<std::int32_t>> expected = {
            {},                 // IDR
            {0},                // P1
            {2, 0},             // P2
            {4, 2, 0},          // P3
            {6, 4, 2},          // P4 (MMCO 6, current LT)
            {6, 4, 2, 8},       // P5
            {10, 6, 4, 2, 8},   // P6 (MMCO 5)
            {12},               // P7
            {},                 // IDR (LT, clears DPB)
            {0},                // P1
            {2, 0},             // P2
            {4, 2, 0},          // P3 (MMCO 1)
            {6, 2, 0},          // P4 (MMCO 2)
            {8, 6, 2},          // P5 (MMCO 3)
            {10, 8, 2, 6},      // P6 (MMCO 3 reassigns index 2)
            {12, 10, 2, 8},     // P7 (MMCO 4 shrink)
            {14, 12, 10, 2},    // P8
            {16, 14, 12, 10, 2}, // P9 (sliding window)
    };

    for (std::size_t i = 0; i < pictures.size(); ++i) {
        const Picture& picture = pictures[i];
        expect(picture.status ==
                       static_cast<std::uint8_t>(bs::avc::MotionVectorStatus::Ok),
               "MMCO picture is fully parsed", describe(picture, i).c_str());
        expect_list_vector(picture, i, 0, expected[i]);
    }

    expect((pictures[4].flags & bs::avc::MvPictureLongTerm) != 0,
           "MMCO 6 marks the current picture long-term");
    expect(pictures[4].long_term_frame_idx == 0,
           "MMCO 6 assigns long-term index 0");
    expect((pictures[8].flags & bs::avc::MvPictureLongTerm) != 0,
           "IDR long_term_reference_flag marks the IDR long-term");
    expect(pictures[8].long_term_frame_idx == 0,
           "IDR long-term index is 0");
    expect(pictures[8].num_ref_idx[0] == 0,
           "IDR picture exposes no reference list");
    expect((pictures[4].flags & bs::avc::MvPictureIdr) == 0,
           "non-IDR long-term picture is not flagged IDR");
    expect(reference_count == 6, "MMCO fixture ends with six DPB frames");
    expect(max_long_term == 0, "MMCO fixture MaxLongTermFrameIdx is zero");
}

void test_field_reorder(const char* path) {
    std::size_t reference_count = 0;
    int max_long_term = -2;
    const auto pictures =
            run_stream(path, 8, nullptr, &reference_count, &max_long_term);
    expect(pictures.size() == 11, "field-reorder fixture picture count", path);
    if (pictures.size() != 11) return;

    /*
     * Index 4 is a field list reordered by opposite parity (P bottom POC 6
     * before P top POC 4); index 6 is a long-term field reorder.
     */
    expect_list(pictures[2], 2, 0, {0, 0});
    expect_list(pictures[3], 3, 0, {0, 4, 0});
    expect_list(pictures[4], 4, 0, {6, 4});
    expect_list(pictures[5], 5, 0, {0, 8, 4, 0, 6});
    expect_list(pictures[6], 6, 0, {6});
    expect_list(pictures[7], 7, 0, {10, 14, 0, 8, 4, 0, 6});
    expect_list(pictures[8], 8, 0, {14, 8, 0});
    expect_list(pictures[9], 9, 0, {18, 14, 0, 8});
    expect_list(pictures[10], 10, 0, {20, 18, 14, 0});

    for (std::size_t i = 0; i < 8; ++i) {
        expect((pictures[i].flags & bs::avc::MvPictureField) != 0,
               "PAFF fixture field picture is marked MvPictureField");
        const bool bottom = (i & 1u) != 0;
        expect(((pictures[i].flags & bs::avc::MvPictureBottomField) != 0) == bottom,
               "PAFF fixture bottom-field flag matches coding order");
        expect((pictures[i].flags & bs::avc::MvPictureMbaff) == 0,
               "PAFF fixture is not marked MBAFF");
    }
    for (std::size_t i = 8; i < pictures.size(); ++i) {
        expect((pictures[i].flags & bs::avc::MvPictureField) == 0,
               "PAFF fixture frame picture is not marked MvPictureField");
        expect((pictures[i].flags & bs::avc::MvPictureMbaff) == 0,
               "PAFF fixture frame picture is not marked MBAFF");
    }
    expect(reference_count == 6, "field fixture ends with six DPB frames");
    expect(max_long_term == 1, "field fixture MaxLongTermFrameIdx is one");
}

void test_field_mmco(const char* path) {
    std::size_t reference_count = 0;
    int max_long_term = -2;
    const auto pictures =
            run_stream(path, 8, nullptr, &reference_count, &max_long_term);
    expect(pictures.size() == 9, "field-MMCO fixture picture count", path);
    if (pictures.size() != 9) return;

    expect_list(pictures[4], 4, 0, {4, 6, 0, 0});
    expect_list(pictures[5], 5, 0, {0, 8, 0, 6});
    expect_list(pictures[6], 6, 0, {8, 10, 0, 0, 6});
    expect_list(pictures[7], 7, 0, {10, 8, 0, 0, 12});
    expect_list(pictures[8], 8, 0, {8, 0, 12});

    expect((pictures[6].flags & bs::avc::MvPictureLongTerm) != 0,
           "MMCO 6 marks a field picture long-term");
    expect(pictures[6].long_term_frame_idx == 1,
           "field MMCO 6 assigns long-term index 1");
    expect((pictures[7].flags & bs::avc::MvPictureLongTerm) != 0,
           "MMCO 6 marks the second field of the pair long-term");
    expect(pictures[8].num_ref_idx[0] == 3,
           "frame list keeps the long-term field pair last");
    expect(reference_count == 4, "field-MMCO fixture DPB holds four frames");
    expect(max_long_term == 1, "field-MMCO fixture MaxLongTermFrameIdx is one");
}

void test_field_b_lists(const char* path) {
    const auto pictures = run_stream(path, 4);
    expect(pictures.size() == 5, "field-B fixture picture count", path);
    if (pictures.size() != 5) return;

    const Picture& b = pictures[4];
    expect(b.status == static_cast<std::uint8_t>(bs::avc::MotionVectorStatus::Ok),
           "field B picture is fully parsed");
    expect((b.flags & bs::avc::MvPictureField) != 0,
           "field B picture is marked MvPictureField");
    expect((b.flags & bs::avc::MvPictureReference) == 0,
           "field B picture is not a reference picture");
    expect_list(b, 4, 0, {4, 6, 0, 0});
    expect_list(b, 4, 1, {6, 4, 0, 0});

    int direct_cells = 0;
    for (const bs::avc::MvCell& cell : b.cells) {
        if (cell.flags & bs::avc::MvDirect) ++direct_cells;
    }
    expect(direct_cells > 0, "skipped field B macroblocks are direct");
}

void test_frame_num_wrap(const char* path) {
    std::size_t reference_count = 0;
    int max_long_term = -2;
    const auto pictures =
            run_stream(path, 4, nullptr, &reference_count, &max_long_term);
    expect(pictures.size() == 18, "frame_num-wrap fixture picture count", path);
    if (pictures.size() != 18) return;

    /* Ordinary sliding window keeps two short-term frames. */
    expect_list(pictures[1], 1, 0, {0});
    expect_list(pictures[15], 15, 0, {28, 26});

    /* Wrapped PicNums: fn15 is -1, fn14 is -2.  The reordering command moves
       fn14 (-2) in front of fn15, then MMCO 1 removes fn15 (-1). */
    expect(pictures[16].frame_num == 0, "frame_num wraps to zero");
    expect(pictures[16].poc == 32, "frame_num wrap increments POC");
    expect_list(pictures[16], 16, 0, {28, 30});

    /* Next fn1 sees the new fn0 (POC 32) and the old fn14 (POC 28). */
    expect_list(pictures[17], 17, 0, {32, 28});
    expect(reference_count == 2, "wrap fixture DPB holds two frames");
    expect(max_long_term == -1, "wrap fixture has no long-term indices");
}

void test_frame_num_gap(const char* path) {
    bs::avc::MotionVectorStatus final_status = bs::avc::MotionVectorStatus::Ok;
    const auto pictures = run_stream(path, 4, nullptr, nullptr, nullptr, &final_status);
    expect(pictures.size() == 6, "frame_num-gap fixture picture count", path);
    if (pictures.size() != 6) return;
    for (std::size_t i = 0; i < pictures.size(); ++i) {
        expect(pictures[i].status ==
                       static_cast<std::uint8_t>(bs::avc::MotionVectorStatus::Ok),
               "frame_num-gap picture is fully parsed", describe(pictures[i], i).c_str());
    }
    expect_list(pictures[3], 3, 0, {4, 2});
    expect_list(pictures[4], 4, 0, {6, 4, 2});
    expect_list(pictures[5], 5, 0, {12, 10, 8, 6});
    expect(final_status == bs::avc::MotionVectorStatus::Ok,
           "gap fixture extractor status is Ok");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 9) {
        std::fprintf(stderr,
                     "usage: avc_mv_dpb_test <multiref.h264> <multiref.mv.txt> "
                     "<mmco.h264> <field_reorder.h264> <field_mmco.h264> "
                     "<field_b.h264> <wrap.h264> <gap.h264>\n");
        return 1;
    }

    test_multiref_stress(argv[1], argv[2]);
    test_mmco(argv[3]);
    test_field_reorder(argv[4]);
    test_field_mmco(argv[5]);
    test_field_b_lists(argv[6]);
    test_frame_num_wrap(argv[7]);
    test_frame_num_gap(argv[8]);

    if (failures != 0) {
        std::fprintf(stderr, "%d motion-vector DPB failure(s)\n", failures);
        return 1;
    }

    std::puts("avc motion-vector DPB: ok");
    return 0;
}
