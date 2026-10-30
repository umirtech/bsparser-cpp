// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * I_PCM / monochrome / separate-colour-plane coverage for the opt-in H.264
 * motion-vector extractor.
 *
 * Each argument pair is "<stream.h264> <kind>" where kind is:
 *   pcm          CABAC/CAVLC I_PCM stream, 4:2:0/4:2:2/4:4:4 or 10-bit;
 *   mono         monochrome stream, CABAC or CAVLC;
 *   sep          separate_colour_plane_flag=1 synthetic CAVLC stream;
 *   bpcm         synthetic CAVLC B slice carrying an I_PCM macroblock.
 *
 * The PCM and mono streams are also compared with FFmpeg-derived motion
 * references in avc_mv_chroma_test; this test asserts the status, picture
 * flags and, for the synthetic separate-plane fixture, the exact luma grid.
 */

#include "bsparser.hpp"
#include "utils/MotionBufferPool.h"

#include <cstddef>
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

struct Grid {
    std::uint32_t width4x4 = 0;
    std::uint32_t height4x4 = 0;
    std::uint32_t flags = 0;
    std::int32_t poc = 0;
    std::uint32_t frame_num = 0;
    std::uint8_t colour_plane_id = 0;
    std::uint8_t plane_count = 0;
    std::uint8_t num_ref_idx[2]{0, 0};
    std::int32_t ref_poc[2][32]{};
    std::vector<bs::avc::MvCell> cells;
};

struct Stats {
    int pictures = 0;
    int ok = 0;
    int unsupported = 0;
    int truncated = 0;
    int other_failures = 0;
    std::vector<Grid> grids;
};

struct Context {
    bs::utils::MotionBufferPool* pool = nullptr;
    Stats stats{};
};

bs::avc::MotionVectorBuffer acquire_thunk(
        void* context,
        const bs::avc::MotionVectorBufferRequest& request) {
    return static_cast<Context*>(context)->pool->acquire(request);
}

void ready(void* context, const bs::avc::MotionVectorBufferView& view) {
    auto* stats = &static_cast<Context*>(context)->stats;
    ++stats->pictures;

    if (view.header == nullptr || view.data == nullptr ||
        !bs::avc::motionVectorHeaderValid(*view.header)) {
        ++stats->other_failures;
        return;
    }

    switch (static_cast<bs::avc::MotionVectorStatus>(view.header->status)) {
        case bs::avc::MotionVectorStatus::Ok:
            ++stats->ok;
            break;
        case bs::avc::MotionVectorStatus::Truncated:
            ++stats->truncated;
            return;
        case bs::avc::MotionVectorStatus::Unsupported:
            ++stats->unsupported;
            return;
        default:
            ++stats->other_failures;
            return;
    }

    Grid grid;
    grid.width4x4 = view.header->width4x4;
    grid.height4x4 = view.header->height4x4;
    grid.flags = view.header->flags;
    grid.poc = view.header->poc;
    grid.frame_num = view.header->frame_num;
    grid.colour_plane_id = view.header->colour_plane_id;
    grid.plane_count = view.header->plane_count;
    grid.num_ref_idx[0] = view.header->num_ref_idx[0];
    grid.num_ref_idx[1] = view.header->num_ref_idx[1];
    for (int list = 0; list < 2; ++list) {
        for (std::size_t i = 0; i < 32; ++i) {
            grid.ref_poc[list][i] = view.header->ref_poc[list][i];
        }
    }
    const auto cells = bs::avc::motionCells(view);
    grid.cells.assign(cells.begin(), cells.end());
    stats->grids.push_back(std::move(grid));
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

void expect(bool condition, const char* message, const char* stream) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s (%s)\n", message, stream);
        ++failures;
    }
}

const bs::avc::MvCell& at(const Grid& grid, std::uint32_t x, std::uint32_t y) {
    return grid.cells[static_cast<std::size_t>(y) * grid.width4x4 + x];
}

void check_separate_planes(const Stats& stats, const char* stream) {
    expect(stats.pictures == 9,
           "separate-plane stream emits three planes for three access units",
           stream);
    if (stats.pictures != 9) return;

    for (int frame = 0; frame < 3; ++frame) {
        for (int plane = 0; plane < 3; ++plane) {
            const Grid& grid = stats.grids[static_cast<std::size_t>(frame * 3 + plane)];
            expect((grid.flags & bs::avc::MvPictureSeparateColourPlanes) != 0,
                   "separate-colour-plane flag is set", stream);
            expect(grid.colour_plane_id == plane,
                   "buffer header carries the colour plane id", stream);
            expect(bs::avc::mvPictureColourPlane(grid.flags) == plane,
                   "picture flags carry the colour plane id", stream);
            expect(grid.plane_count == 3,
                   "buffer header reports three colour planes", stream);
            expect(grid.frame_num == static_cast<std::uint32_t>(frame),
                   "all planes share the access-unit frame_num", stream);
            expect(grid.poc == frame * 2,
                   "all planes share the access-unit POC", stream);
            expect(grid.width4x4 == 8 && grid.height4x4 == 8 &&
                           grid.cells.size() == 64,
                   "separate-plane grid is 8x8 4x4 cells", stream);
        }
    }
    if (stats.grids.size() != 9) return;

    for (int plane = 0; plane < 3; ++plane) {
        const Grid& idr = stats.grids[static_cast<std::size_t>(plane)];
        expect((idr.flags & bs::avc::MvPictureIdr) != 0,
               "separate-plane IDR flag is set", stream);
        expect((idr.flags & bs::avc::MvPictureIntra) != 0,
               "separate-plane IDR is intra", stream);
        for (const auto& cell : idr.cells) {
            expect(cell.flags == bs::avc::MvIntra, "IDR cells are intra", stream);
        }
    }

    auto check_mb = [&](const Grid& grid, std::uint32_t mb_x, std::uint32_t mb_y,
                       std::int16_t dx, std::int16_t dy, std::uint8_t flags,
                       const char* message) {
        for (std::uint32_t y = 0; y < 4; ++y) {
            for (std::uint32_t x = 0; x < 4; ++x) {
                const auto& cell = at(grid, mb_x * 4 + x, mb_y * 4 + y);
                expect(cell.flags == flags, message, stream);
                expect(cell.mv[0][0] == dx && cell.mv[0][1] == dy,
                       "separate-plane motion matches the plane slice", stream);
                expect(cell.ref_idx[0] == 0 && cell.ref_idx[1] == -1,
                       "separate-plane ref index is zero", stream);
            }
        }
    };

    const Grid& p0_frame1 = stats.grids[3];
    expect(p0_frame1.flags & bs::avc::MvPictureHasList0,
           "plane 0 P picture has list 0", stream);
    expect(p0_frame1.num_ref_idx[0] >= 1,
           "plane 0 P picture has a reference", stream);
    expect(p0_frame1.ref_poc[0][0] == 0,
           "plane 0 references its own IDR plane", stream);
    check_mb(p0_frame1, 0, 0, 2, -2, bs::avc::MvList0,
             "plane 0 P_L0 MVD is applied");
    for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 4; x < 8; ++x) {
            expect(at(p0_frame1, x, y).flags == bs::avc::MvIntra,
                   "plane 0 I_PCM cells are intra", stream);
        }
    }
    for (std::uint32_t y = 4; y < 8; ++y) {
        for (std::uint32_t x = 0; x < 8; ++x) {
            const auto& cell = at(p0_frame1, x, y);
            expect(cell.flags == (bs::avc::MvSkip | bs::avc::MvList0),
                   "plane 0 skipped cells are marked skip/list-0", stream);
            expect(cell.mv[0][0] == 0 && cell.mv[0][1] == 0,
                   "plane 0 skipped cells use the zero predictor", stream);
        }
    }

    const Grid& p1_frame1 = stats.grids[4];
    expect(p1_frame1.ref_poc[0][0] == 0,
           "plane 1 references its own IDR plane", stream);
    check_mb(p1_frame1, 0, 0, 1, 1, bs::avc::MvList0,
             "plane 1 P_L0 MVD is applied");
    for (std::uint32_t mb = 1; mb < 4; ++mb) {
        check_mb(p1_frame1, mb % 2, mb / 2, 0, 0,
                 static_cast<std::uint8_t>(bs::avc::MvSkip | bs::avc::MvList0),
                 "plane 1 skipped macroblock is marked skip/list-0");
    }

    const Grid& p2_frame1 = stats.grids[5];
    expect(p2_frame1.ref_poc[0][0] == 0,
           "plane 2 references its own IDR plane", stream);
    for (std::uint32_t mb = 0; mb < 2; ++mb) {
        check_mb(p2_frame1, mb, 0, 0, 0,
                 static_cast<std::uint8_t>(bs::avc::MvSkip | bs::avc::MvList0),
                 "plane 2 leading skip is marked skip/list-0");
    }
    check_mb(p2_frame1, 0, 1, -3, 2, bs::avc::MvList0,
             "plane 2 P_L0 MVD is applied");
    check_mb(p2_frame1, 1, 1, 0, 0,
             static_cast<std::uint8_t>(bs::avc::MvSkip | bs::avc::MvList0),
             "plane 2 trailing skip is marked skip/list-0");

    for (std::uint32_t mb = 0; mb < 4; ++mb) {
        check_mb(stats.grids[6], mb % 2, mb / 2, 0, 0,
                 static_cast<std::uint8_t>(bs::avc::MvSkip | bs::avc::MvList0),
                 "plane 0 second P picture is fully skipped");
        check_mb(stats.grids[8], mb % 2, mb / 2, 0, 0,
                 static_cast<std::uint8_t>(bs::avc::MvSkip | bs::avc::MvList0),
                 "plane 2 second P picture is fully skipped");
    }
    for (std::uint32_t mb = 0; mb < 3; ++mb) {
        check_mb(stats.grids[7], mb % 2, mb / 2, 0, 0,
                 static_cast<std::uint8_t>(bs::avc::MvSkip | bs::avc::MvList0),
                 "plane 1 second P picture leading skip");
    }
    check_mb(stats.grids[7], 1, 1, -2, 3, bs::avc::MvList0,
             "plane 1 second P picture coded MB");
}

void check_b_pcm(const Stats& stats, const char* stream) {
    expect(stats.pictures == 4, "B-slice PCM stream emits four pictures", stream);
    if (stats.pictures != 4) return;

    const Grid& b = stats.grids[2];
    expect((b.flags & (bs::avc::MvPictureHasList0 | bs::avc::MvPictureHasList1)) ==
                   (bs::avc::MvPictureHasList0 | bs::avc::MvPictureHasList1),
           "B picture has both reference lists", stream);
    expect(b.width4x4 == 8 && b.height4x4 == 8 && b.cells.size() == 64,
           "B picture grid is complete", stream);
    if (b.cells.size() != 64) return;

    for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) {
            const auto& cell = at(b, x, y);
            expect(cell.flags == bs::avc::MvList0,
                   "B_L0 macroblock carries list-0 cells", stream);
            expect(cell.mv[0][0] == 1 && cell.mv[0][1] == -1,
                   "B_L0 MVD is applied", stream);
            expect(cell.ref_idx[0] == 0, "B_L0 ref index is zero", stream);
        }
        for (std::uint32_t x = 4; x < 8; ++x) {
            expect(at(b, x, y).flags == bs::avc::MvIntra,
                   "B-slice I_PCM cells are intra", stream);
        }
    }
    for (std::uint32_t y = 4; y < 8; ++y) {
        for (std::uint32_t x = 0; x < 8; ++x) {
            const auto& cell = at(b, x, y);
            expect((cell.flags & bs::avc::MvDirect) != 0,
                   "B_SKIP cells are direct", stream);
            expect((cell.flags & bs::avc::MvList0) != 0,
                   "B_SKIP cells use list 0", stream);
        }
    }
}

void check_b_pcm_cabac(const Stats& stats, const char* stream) {
    expect(stats.pictures == 3, "CABAC B-slice PCM stream emits three pictures", stream);
    if (stats.pictures != 3) return;

    const Grid& b = stats.grids[2];
    expect((b.flags & (bs::avc::MvPictureHasList0 | bs::avc::MvPictureHasList1)) ==
                   (bs::avc::MvPictureHasList0 | bs::avc::MvPictureHasList1),
           "CABAC B picture has both reference lists", stream);
    expect((b.flags & bs::avc::MvPictureIntra) == 0,
           "CABAC B picture is inter", stream);
    expect(b.poc == 2, "CABAC B picture POC is two", stream);
    expect(b.width4x4 == 8 && b.height4x4 == 8 && b.cells.size() == 64,
           "CABAC B picture grid is complete", stream);
    if (b.cells.size() != 64) return;

    /* MB 0: the I_PCM macroblock. */
    for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) {
            expect(at(b, x, y).flags == bs::avc::MvIntra,
                   "CABAC B I_PCM cells are intra", stream);
        }
    }
    /* MB 1: the I_16x16 macroblock that follows the PCM restart. */
    for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 4; x < 8; ++x) {
            expect(at(b, x, y).flags == bs::avc::MvIntra,
                   "macroblock after I_PCM is fully parsed", stream);
        }
    }
    /* MBs 2 and 3: B_SKIP/direct. */
    for (std::uint32_t y = 4; y < 8; ++y) {
        for (std::uint32_t x = 0; x < 8; ++x) {
            const auto& cell = at(b, x, y);
            expect((cell.flags & bs::avc::MvDirect) != 0,
                   "CABAC B_SKIP cells are direct", stream);
            expect((cell.flags & (bs::avc::MvList0 | bs::avc::MvList1)) != 0,
                   "CABAC B_SKIP cells reference a list", stream);
        }
    }
}

void check_paff_chroma(const Stats& stats, const char* stream) {
    expect(stats.pictures == 5, "high-chroma PAFF stream emits five pictures", stream);
    if (stats.pictures != 5) return;

    for (const Grid* grid : {&stats.grids[0], &stats.grids[1]}) {
        expect((grid->flags & bs::avc::MvPictureField) != 0,
               "PAFF IDR picture is a field", stream);
        expect((grid->flags & bs::avc::MvPictureIdr) != 0,
               "PAFF IDR field is marked IDR", stream);
        expect((grid->flags & bs::avc::MvPictureIntra) != 0,
               "PAFF IDR field is intra", stream);
        expect(grid->width4x4 == 8 && grid->height4x4 == 8 &&
                       grid->cells.size() == 64,
               "PAFF IDR field grid is 8x8", stream);
    }
    expect((stats.grids[0].flags & bs::avc::MvPictureBottomField) == 0,
           "PAFF IDR top field is not marked bottom", stream);
    expect((stats.grids[1].flags & bs::avc::MvPictureBottomField) != 0,
           "PAFF IDR bottom field is marked bottom", stream);

    const Grid& top = stats.grids[2];
    expect((top.flags & bs::avc::MvPictureField) != 0,
           "PAFF P top field is a field", stream);
    expect((top.flags & bs::avc::MvPictureBottomField) == 0,
           "PAFF P top field is not marked bottom", stream);
    expect(top.width4x4 == 8 && top.height4x4 == 8 && top.cells.size() == 64,
           "PAFF P top field grid is 8x8", stream);
    expect(top.num_ref_idx[0] == 1 && top.ref_poc[0][0] == 0,
           "PAFF P top field references the IDR field", stream);
    if (top.cells.size() == 64) {
        for (std::uint32_t y = 0; y < 4; ++y) {
            for (std::uint32_t x = 0; x < 4; ++x) {
                expect(at(top, x, y).flags == (bs::avc::MvIntra | bs::avc::MvField),
                       "PAFF P-field I_PCM cells are intra", stream);
            }
            for (std::uint32_t x = 4; x < 8; ++x) {
                const auto& cell = at(top, x, y);
                expect(cell.flags == (bs::avc::MvSkip | bs::avc::MvList0 | bs::avc::MvField),
                       "PAFF P-field skipped cells are marked skip", stream);
            }
        }
        for (std::uint32_t y = 4; y < 8; ++y) {
            for (std::uint32_t x = 0; x < 8; ++x) {
                expect(at(top, x, y).flags == (bs::avc::MvSkip | bs::avc::MvList0 | bs::avc::MvField),
                       "PAFF P-field trailing macroblocks are skipped", stream);
            }
        }
    }

    const Grid& bottom = stats.grids[3];
    expect((bottom.flags & bs::avc::MvPictureField) != 0,
           "PAFF P bottom field is a field", stream);
    expect((bottom.flags & bs::avc::MvPictureBottomField) != 0,
           "PAFF P bottom field is marked bottom", stream);
    expect(bottom.cells.size() == 64, "PAFF P bottom field grid is 8x8", stream);
    for (const auto& cell : bottom.cells) {
        expect(cell.flags == (bs::avc::MvSkip | bs::avc::MvList0 | bs::avc::MvField),
               "PAFF P bottom field is entirely skipped", stream);
    }

    const Grid& frame = stats.grids[4];
    expect((frame.flags & bs::avc::MvPictureField) == 0,
           "PAFF frame picture is not a field", stream);
    expect(frame.width4x4 == 8 && frame.height4x4 == 16 &&
                   frame.cells.size() == 128,
           "PAFF frame picture grid is 8x16", stream);
    for (const auto& cell : frame.cells) {
        expect(cell.flags == (bs::avc::MvSkip | bs::avc::MvList0),
               "PAFF frame P picture is entirely skipped", stream);
    }
}

void check_mbaff_chroma(
        const Stats& stats,
        const char* stream,
        int expected_pictures,
        bool expect_field_cells,
        bool expect_direct_cells = true,
        std::uint32_t width4x4 = 16,
        std::uint32_t height4x4 = 16) {
    expect(stats.pictures == expected_pictures,
           "high-chroma MBAFF picture count", stream);
    if (stats.pictures != expected_pictures) return;

    int field_pictures = 0;
    int pictures_with_field_cells = 0;
    int pictures_with_direct_cells = 0;
    int pictures_mixing_field_and_frame_cells = 0;
    for (const Grid& grid : stats.grids) {
        expect((grid.flags & bs::avc::MvPictureMbaff) != 0,
               "high-chroma picture is marked MBAFF", stream);
        expect((grid.flags & bs::avc::MvPictureField) == 0,
               "high-chroma MBAFF picture is not a field picture", stream);
        expect(grid.width4x4 == width4x4 && grid.height4x4 == height4x4 &&
                       grid.cells.size() ==
                               static_cast<std::size_t>(width4x4) * height4x4,
               "high-chroma MBAFF grid dimensions", stream);
        int field_cells = 0;
        int direct_cells = 0;
        for (const auto& cell : grid.cells) {
            if (cell.flags & bs::avc::MvField) ++field_cells;
            if (cell.flags & bs::avc::MvDirect) ++direct_cells;
        }
        if (field_cells > 0) ++pictures_with_field_cells;
        if (field_cells > 0 &&
            field_cells < static_cast<int>(grid.cells.size())) {
            ++pictures_mixing_field_and_frame_cells;
        }
        if (direct_cells > 0) ++pictures_with_direct_cells;
        if ((grid.flags & bs::avc::MvPictureField) != 0) ++field_pictures;
    }
    expect(field_pictures == 0, "high-chroma MBAFF stream has no field pictures", stream);
    if (expect_field_cells) {
        expect(pictures_with_field_cells > 0,
               "high-chroma MBAFF stream contains field-coded MB cells", stream);
        expect(pictures_mixing_field_and_frame_cells > 0,
               "high-chroma MBAFF picture mixes field- and frame-coded MB cells",
               stream);
    } else {
        expect(pictures_with_field_cells == 0,
               "high-chroma MBAFF stream has no field-coded MB cells", stream);
    }
    if (expect_direct_cells) {
        expect(pictures_with_direct_cells > 0,
               "high-chroma MBAFF stream contains direct-mode cells", stream);
    }
}

void test_stream(const char* stream_path, const char* kind) {
    const auto bytes = read_file(stream_path);

    bs::utils::MotionBufferPool pool({4, 16, 32u << 20, 64});
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

    const std::string mode(kind);
    const bs::avc::MotionVectorExtractor* extractor =
            state->avc_motion_vectors();

    expect(context.stats.truncated == 0, "no truncated pictures", stream_path);
    expect(context.stats.other_failures == 0, "no malformed pictures", stream_path);
    expect(extractor != nullptr, "motion-vector extractor exists", stream_path);

    expect(context.stats.pictures > 0, "stream produced pictures", stream_path);
    expect(context.stats.unsupported == 0, "no unsupported pictures", stream_path);
    expect(context.stats.ok == context.stats.pictures,
           "every picture is fully parsed", stream_path);
    if (extractor != nullptr) {
        expect(extractor->status() == bs::avc::MotionVectorStatus::Ok,
               "extractor status is Ok", stream_path);
    }

    if (mode == "mono") {
        for (const Grid& grid : context.stats.grids) {
            expect((grid.flags & bs::avc::MvPictureMonochrome) != 0,
                   "monochrome picture flag is set", stream_path);
        }
    } else if (mode == "sep") {
        check_separate_planes(context.stats, stream_path);
        if (extractor != nullptr) {
            for (std::uint8_t plane = 0; plane < 3; ++plane) {
                expect(extractor->referencePictureCount(plane) == 1,
                       "each colour plane keeps its own reference DPB",
                       stream_path);
                expect(extractor->maxLongTermFrameIdx(plane) == -1,
                       "each colour plane keeps its own long-term state",
                       stream_path);
            }
        }
    } else if (mode == "bpcm") {
        check_b_pcm(context.stats, stream_path);
    } else if (mode == "bpcm-cabac") {
        check_b_pcm_cabac(context.stats, stream_path);
    } else if (mode == "field-chroma") {
        check_paff_chroma(context.stats, stream_path);
    } else if (mode == "mbaff-chroma") {
        check_mbaff_chroma(context.stats, stream_path, 10, true);
    } else if (mode == "mbaff-chroma-8") {
        check_mbaff_chroma(context.stats, stream_path, 8, true);
    } else if (mode == "mbaff-chroma-cavlc") {
        check_mbaff_chroma(context.stats, stream_path, 24, true);
    } else if (mode == "mbaff-chroma-cavlc-10bit") {
        check_mbaff_chroma(context.stats, stream_path, 24, true, false);
    } else if (mode == "mbaff-chroma-cavlc-frame") {
        check_mbaff_chroma(context.stats, stream_path, 12, false);
    } else if (mode != "pcm") {
        expect(false, "unknown test mode", stream_path);
    }

    state->clear();
    pool.trimAll();
    expect(pool.stats().buffersInUse == 0, "clear releases all buffers", stream_path);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3 || (argc % 2) == 0) {
        std::fprintf(stderr,
                     "usage: avc_mv_pcm_test <stream.h264> "
                     "<pcm|mono|sep|bpcm|bpcm-cabac|field-chroma|mbaff-chroma|"
                     "mbaff-chroma-8|mbaff-chroma-cavlc|mbaff-chroma-cavlc-10bit|"
                     "mbaff-chroma-cavlc-frame> "
                     "[<stream.h264> <kind> ...]\n");
        return 1;
    }

    for (int i = 1; i + 1 < argc; i += 2) {
        test_stream(argv[i], argv[i + 1]);
    }

    if (failures != 0) {
        std::fprintf(stderr, "%d I_PCM/monochrome/plane failure(s)\n", failures);
        return 1;
    }

    std::puts("avc I_PCM/monochrome/separate-plane: ok");
    return 0;
}
