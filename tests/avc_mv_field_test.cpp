// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#include "bsparser.hpp"
#include "utils/MotionBufferPool.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

namespace {

struct PictureInfo {
    std::uint32_t flags = 0;
    std::uint8_t status = 255;
    std::int32_t poc = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mb_width = 0;
    std::uint32_t mb_height = 0;
    std::uint32_t width4x4 = 0;
    std::uint32_t height4x4 = 0;
    std::uint8_t num_ref_idx[2]{0, 0};
    std::int32_t ref_poc[2]{0, 0};
    int field_cells = 0;
    int direct_cells = 0;
    int inter_cells = 0;
    int nonzero_mv_cells = 0;
};

struct Context {
    bs::utils::MotionBufferPool* pool = nullptr;
    std::vector<PictureInfo> pictures;
};

bs::avc::MotionVectorBuffer acquire_thunk(
        void* context, const bs::avc::MotionVectorBufferRequest& request) {
    return static_cast<Context*>(context)->pool->acquire(request);
}

void ready(void* context, const bs::avc::MotionVectorBufferView& view) {
    auto* ctx = static_cast<Context*>(context);
    PictureInfo info{};
    if (view.header != nullptr && bs::avc::motionVectorHeaderValid(*view.header)) {
        info.flags = view.header->flags;
        info.status = view.header->status;
        info.poc = view.header->poc;
        info.width = view.header->width;
        info.height = view.header->height;
        info.mb_width = view.header->mb_width;
        info.mb_height = view.header->mb_height;
        info.width4x4 = view.header->width4x4;
        info.height4x4 = view.header->height4x4;
        info.num_ref_idx[0] = view.header->num_ref_idx[0];
        info.num_ref_idx[1] = view.header->num_ref_idx[1];
        info.ref_poc[0] = view.header->ref_poc[0][0];
        info.ref_poc[1] = view.header->ref_poc[1][0];

        for (const bs::avc::MvCell& cell : bs::avc::motionCells(view)) {
            if (cell.flags & bs::avc::MvField) ++info.field_cells;
            if (cell.flags & bs::avc::MvDirect) ++info.direct_cells;
            if (!(cell.flags & bs::avc::MvUnavailable) &&
                !(cell.flags & bs::avc::MvIntra)) {
                ++info.inter_cells;
            }
            if (cell.mv[0][0] != 0 || cell.mv[0][1] != 0 ||
                cell.mv[1][0] != 0 || cell.mv[1][1] != 0) {
                ++info.nonzero_mv_cells;
            }
        }
    } else {
        info.status = 254;
    }
    ctx->pictures.push_back(info);
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

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

std::vector<PictureInfo> run_stream(const char* path, std::size_t extras) {
    const auto bytes = read_file(path);

    bs::utils::MotionBufferPool pool(
            {8, 32, static_cast<std::size_t>(extras) << 20, 64});
    Context context{&pool, {}};

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

    expect(state->avc_motion_vectors() != nullptr,
           "motion-vector extractor exists for the field stream");
    if (state->avc_motion_vectors() != nullptr) {
        expect(state->avc_motion_vectors()->status() == bs::avc::MotionVectorStatus::Ok,
               "field stream extractor status is Ok");
    }

    state->clear();
    return context.pictures;
}

void test_paff(const char* path) {
    const auto pics = run_stream(path, 8);
    expect(pics.size() == 5, "PAFF fixture emits five field/frame pictures");
    if (pics.size() != 5) return;

    expect(pics[0].status == 0 && pics[1].status == 0 && pics[2].status == 0 &&
                   pics[3].status == 0 && pics[4].status == 0,
           "PAFF pictures are fully parsed");
    expect(pics[0].poc == 0 && pics[1].poc == 0,
           "PAFF IDR top/bottom fields have POC 0");
    expect(pics[2].poc == 2 && pics[3].poc == 2,
           "PAFF P top/bottom fields have POC 2");
    expect(pics[4].poc == 4, "PAFF P frame has POC 4");

    expect((pics[0].flags & bs::avc::MvPictureField) != 0,
           "PAFF top IDR field is marked MvPictureField");
    expect((pics[0].flags & bs::avc::MvPictureBottomField) == 0,
           "PAFF top IDR field is not marked bottom");
    expect((pics[1].flags & bs::avc::MvPictureBottomField) != 0,
           "PAFF bottom IDR field is marked MvPictureBottomField");

    for (int i = 0; i < 4; ++i) {
        expect(pics[static_cast<std::size_t>(i)].width == 32 &&
                       pics[static_cast<std::size_t>(i)].height == 32,
               "PAFF field dimensions are 32x32");
        expect(pics[static_cast<std::size_t>(i)].mb_width == 2 &&
                       pics[static_cast<std::size_t>(i)].mb_height == 2,
               "PAFF field macroblock grid is 2x2");
        expect(pics[static_cast<std::size_t>(i)].width4x4 == 8 &&
                       pics[static_cast<std::size_t>(i)].height4x4 == 8,
               "PAFF field cell grid is 8x8");
    }

    expect(pics[2].field_cells == 64 && pics[3].field_cells == 64,
           "PAFF P fields mark every cell MvField");
    expect(pics[2].inter_cells == 64 && pics[3].inter_cells == 64,
           "PAFF P fields produce inter cells");
    expect(pics[2].nonzero_mv_cells >= 16,
           "PAFF coded field macroblock produces a non-zero MVD");
    expect(pics[2].num_ref_idx[0] == 1 && pics[2].ref_poc[0] == 0,
           "PAFF top P field references the IDR field POC 0");
    expect(pics[3].num_ref_idx[0] == 1 && pics[3].ref_poc[0] == 0,
           "PAFF bottom P field references the IDR field POC 0");

    expect((pics[4].flags & bs::avc::MvPictureField) == 0,
           "PAFF frame picture is not marked MvPictureField");
    expect(pics[4].width == 32 && pics[4].height == 64,
           "PAFF frame dimensions are 32x64");
    expect(pics[4].mb_width == 2 && pics[4].mb_height == 4,
           "PAFF frame macroblock grid is 2x4");
    expect(pics[4].width4x4 == 8 && pics[4].height4x4 == 16,
           "PAFF frame cell grid is 8x16");
    expect(pics[4].inter_cells == 128, "PAFF frame P picture produces inter cells");
}

void test_mbaff(const char* path, std::size_t expected_pictures, bool expect_direct,
                bool expect_field_cells = true) {
    const auto pics = run_stream(path, 16);
    expect(pics.size() == expected_pictures, "MBAFF fixture picture count");
    if (pics.size() != expected_pictures) return;

    int field_pictures = 0;
    int pictures_with_field_cells = 0;
    int pictures_with_direct_cells = 0;
    int pictures_with_field_and_direct = 0;

    for (const PictureInfo& pic : pics) {
        expect(pic.status == 0, "MBAFF picture is fully parsed");
        expect((pic.flags & bs::avc::MvPictureMbaff) != 0,
               "MBAFF picture is marked MvPictureMbaff");
        expect((pic.flags & bs::avc::MvPictureField) == 0,
               "MBAFF frame picture is not marked MvPictureField");
        expect(pic.width == 64 && pic.height == 64, "MBAFF picture is 64x64");
        expect(pic.mb_width == 4 && pic.mb_height == 4,
               "MBAFF macroblock grid is 4x4");
        expect(pic.width4x4 == 16 && pic.height4x4 == 16,
               "MBAFF cell grid is 16x16");

        if (pic.field_cells > 0) ++pictures_with_field_cells;
        if (pic.direct_cells > 0) ++pictures_with_direct_cells;
        if (pic.field_cells > 0 && pic.direct_cells > 0) {
            ++pictures_with_field_and_direct;
        }
        if ((pic.flags & bs::avc::MvPictureField) != 0) ++field_pictures;
    }

    expect(field_pictures == 0, "MBAFF frame stream has no field pictures");
    if (expect_field_cells) {
        expect(pictures_with_field_cells > 0,
               "MBAFF stream contains field-coded macroblock cells");
    }
    expect(pictures_with_direct_cells > 0,
           "MBAFF stream contains direct-mode cells");
    if (expect_direct) {
        expect(pictures_with_field_and_direct > 0,
               "MBAFF stream combines field-coded and direct-mode cells");
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: avc_mv_field_test <paff.h264> <mbaff_cabac.h264> "
                     "<mbaff_cavlc.h264>\n");
        return 1;
    }

    test_paff(argv[1]);
    test_mbaff(argv[2], 15, true);
    /*
     * The CAVLC MBAFF fixture is entirely frame-coded: correcting B-slice
     * I_PCM classification removes the bogus field-coded cells the old
     * parser produced here, so do not require any.
     */
    test_mbaff(argv[3], 5, false, false);

    if (failures != 0) {
        std::fprintf(stderr, "%d field motion-vector failure(s)\n", failures);
        return 1;
    }

    std::puts("avc field motion vectors: ok");
    return 0;
}
