// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * Slice-group (FMO) coverage for the opt-in AVC motion-vector extractor.
 *
 * The test first checks the 8.2.2 map derivation directly for map types 0-6,
 * then parses the synthetic CAVLC and CABAC streams produced by
 * tools/gen_avc_fmo_fixture.py.  The base fixtures have one IDR picture and
 * one P picture split into one slice per slice group.  The `_refs_` variants
 * add two reference P pictures and give each group a different reordering /
 * active-count combination, so each cell's ref POC is checked against the
 * exact macroblock-to-slice map rather than the first-slice fallback.  The P
 * grid must cover every macroblock; CAVLC fixtures additionally have one
 * coded P_L0 macroblock per slice group at a known address with a known MVD,
 * proving that NextMbAddress iteration and mb_skip_run stay inside the group.
 * The CABAC fixtures use all-skipped slices, so every group boundary
 * exercises end_of_slice_flag.
 */

#include "bsparser.hpp"
#include "parser/avc_fmo.hpp"
#include "utils/MotionBufferPool.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <iterator>
#include <sstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* message, const char* stream) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s (%s)\n", message, stream);
        ++failures;
    }
}

struct Grid {
    std::uint32_t width4x4 = 0;
    std::uint32_t height4x4 = 0;
    std::uint32_t flags = 0;
    std::vector<bs::avc::MvCell> cells;
    bs::avc::MotionVectorBufferHeader header{};
    /* Kept alive for per-cell helpers when the test asks for it. */
    bs::avc::MotionVectorBufferView view{};
};

struct Context {
    bs::utils::MotionBufferPool* pool = nullptr;
    int pictures = 0;
    int ok = 0;
    bool capture_views = false;
    std::vector<Grid> grids;
};

bs::avc::MotionVectorBuffer acquire(
        void* opaque, const bs::avc::MotionVectorBufferRequest& request) {
    return static_cast<Context*>(opaque)->pool->acquire(request);
}

void ready(void* opaque, const bs::avc::MotionVectorBufferView& view) {
    auto* context = static_cast<Context*>(opaque);
    ++context->pictures;
    if (view.header == nullptr || view.data == nullptr ||
        !bs::avc::motionVectorHeaderValid(*view.header)) {
        return;
    }
    if (view.header->status !=
        static_cast<std::uint8_t>(bs::avc::MotionVectorStatus::Ok)) {
        return;
    }
    ++context->ok;
    Grid grid;
    grid.width4x4 = view.header->width4x4;
    grid.height4x4 = view.header->height4x4;
    grid.flags = view.header->flags;
    grid.header = *view.header;
    const auto cells = bs::avc::motionCells(view);
    grid.cells.assign(cells.begin(), cells.end());
    if (context->capture_views) {
        grid.view = view;
    }
    context->grids.push_back(std::move(grid));
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

const bs::avc::MvCell& at(const Grid& grid, std::uint32_t mb, std::uint32_t x,
                          std::uint32_t y) {
    const std::uint32_t gx = (mb % 4) * 4 + x;
    const std::uint32_t gy = (mb / 4) * 4 + y;
    return grid.cells[static_cast<std::size_t>(gy) * grid.width4x4 + gx];
}

struct Reference {
    int map_type = -1;
    int groups = 0;
    bool cabac = false;
    int refs = 0;
    int project_poc = 0;
    struct GroupInfo {
        int group = 0;
        int active = 0;
        std::vector<int> pocs;
    };
    struct Coded {
        std::uint32_t mb = 0;
        std::int16_t dx = 0;
        std::int16_t dy = 0;
    };
    std::vector<GroupInfo> group_info;
    std::vector<int> mb_group;
    std::vector<Coded> coded;
};

Reference read_reference(const std::string& path, const char* stream) {
    Reference reference;
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        std::exit(1);
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        char keyword[32]{};
        if (std::sscanf(line.c_str(), "%31s", keyword) != 1) continue;
        const std::string key(keyword);
        if (key == "map_type") {
            (void)std::sscanf(line.c_str(), "map_type %d", &reference.map_type);
        } else if (key == "entropy") {
            char mode[16]{};
            (void)std::sscanf(line.c_str(), "entropy %15s", mode);
            reference.cabac = std::string(mode) == "cabac";
        } else if (key == "groups") {
            (void)std::sscanf(line.c_str(), "groups %d", &reference.groups);
        } else if (key == "refs") {
            (void)std::sscanf(line.c_str(), "refs %d", &reference.refs);
        } else if (key == "project_poc") {
            (void)std::sscanf(line.c_str(), "project_poc %d", &reference.project_poc);
        } else if (key == "group") {
            std::istringstream tokens(line);
            std::string word;
            Reference::GroupInfo info;
            tokens >> word >> info.group >> word >> info.active >> word;
            int poc = 0;
            while (tokens >> poc) {
                info.pocs.push_back(poc);
            }
            if (info.group >= 0) {
                if (reference.group_info.size() <= static_cast<std::size_t>(info.group)) {
                    reference.group_info.resize(static_cast<std::size_t>(info.group) + 1u);
                }
                reference.group_info[static_cast<std::size_t>(info.group)] = info;
            }
        } else if (key == "mb_group") {
            unsigned mb = 0;
            int group = 0;
            if (std::sscanf(line.c_str(), "mb_group %u %d", &mb, &group) == 2) {
                if (reference.mb_group.size() <= mb) {
                    reference.mb_group.resize(mb + 1u, -1);
                }
                reference.mb_group[mb] = group;
            }
        } else if (key == "coded") {
            unsigned mb = 0;
            int dx = 0;
            int dy = 0;
            if (std::sscanf(line.c_str(), "coded %u %d %d", &mb, &dx, &dy) == 3) {
                reference.coded.push_back(Reference::Coded{
                        mb, static_cast<std::int16_t>(dx), static_cast<std::int16_t>(dy)});
            }
        }
    }
    expect(reference.map_type >= 0 && reference.groups > 1,
           "fixture sidecar is well formed", stream);
    if (reference.refs > 0) {
        expect(reference.group_info.size() ==
                       static_cast<std::size_t>(reference.groups),
               "reference sidecar lists every slice group", stream);
        expect(reference.mb_group.size() == 16,
               "reference sidecar lists every macroblock", stream);
    }
    return reference;
}

std::vector<std::uint8_t> map_from_spec(int map_type) {
    switch (map_type) {
        case 0:
            return {0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1};
        case 1:
            return {0, 1, 2, 0, 1, 2, 0, 1, 0, 1, 2, 0, 1, 2, 0, 1};
        case 2:
            return {0, 0, 2, 2, 0, 0, 2, 2, 2, 2, 1, 1, 2, 2, 1, 1};
        case 3:
            return {1, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1};
        case 4:
            return {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
        case 5:
            return {1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0};
        case 6:
            return {0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0};
        default:
            return {};
    }
}

void test_map_derivation() {
    const char* label = "map derivation";

    for (int map_type = 0; map_type <= 6; ++map_type) {
        bs::avc::SequenceParameterSet sps;
        sps.pic_width_in_mbs_minus1 = 3;
        sps.pic_height_in_map_units_minus1 = 3;
        sps.frame_mbs_only_flag = true;

        bs::avc::PictureParameterSet pps;
        pps.num_slice_groups_minus1 = 1;
        auto& map = pps.slice_group_map;
        map.slice_group_map_type = static_cast<std::uint8_t>(map_type);

        switch (map_type) {
            case 0:
                pps.num_slice_groups_minus1 = 1;
                map.run_length_minus1[0] = 1;
                map.run_length_minus1[1] = 1;
                break;
            case 1:
                pps.num_slice_groups_minus1 = 2;
                break;
            case 2:
                pps.num_slice_groups_minus1 = 2;
                map.top_left[0] = 0;
                map.bottom_right[0] = 5;
                map.top_left[1] = 10;
                map.bottom_right[1] = 15;
                break;
            case 3:
            case 4:
            case 5:
                pps.num_slice_groups_minus1 = 1;
                map.slice_group_change_direction_flag = map_type == 5;
                map.slice_group_change_rate_minus1 = 3;  // rate = 4
                break;
            case 6:
                pps.num_slice_groups_minus1 = 2;
                map.pic_size_in_map_units_minus1 = 15;
                map.slice_group_id.resize(16);
                for (std::size_t i = 0; i < 16; ++i) {
                    map.slice_group_id[i] = static_cast<std::uint8_t>(i % 3);
                }
                break;
            default:
                break;
        }

        bs::avc::SliceHeader header;
        header.field_pic_flag = false;
        header.slice_group_change_cycle = 1;

        bs::avc::SliceGroupMaps maps;
        std::string error;
        const bool ok = bs::avc::derive_mb_to_slice_group_map(
                sps, pps, header, false, 16, maps, &error);
        expect(ok, "slice group map derives", label);
        const auto expected = map_from_spec(map_type);
        expect(maps.mb_to_group == expected,
               "slice group map matches the specification", label);
    }

    /* NextMbAddress must skip addresses of other slice groups. */
    const std::vector<std::uint8_t> alternating = {
            0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1};
    expect(bs::avc::next_mb_address(alternating, 1, 0) == 4,
           "NextMbAddress finds the next macroblock of group 0", label);
    expect(bs::avc::next_mb_address(alternating, 3, 1) == 6,
           "NextMbAddress finds the next macroblock of group 1", label);
    expect(bs::avc::next_mb_address(alternating, 15, 1) == 16,
           "NextMbAddress ends at the picture boundary", label);

    /* 8.2.2.8 conversion for field and interlaced frame pictures. */
    {
        bs::avc::SequenceParameterSet sps;
        sps.pic_width_in_mbs_minus1 = 3;
        sps.pic_height_in_map_units_minus1 = 1;
        sps.frame_mbs_only_flag = false;

        bs::avc::PictureParameterSet pps;
        pps.num_slice_groups_minus1 = 1;
        auto& map = pps.slice_group_map;
        map.slice_group_map_type = 4;
        map.slice_group_change_direction_flag = false;
        map.slice_group_change_rate_minus1 = 3;  // rate = 4

        bs::avc::SliceHeader header;
        header.slice_group_change_cycle = 1;
        header.field_pic_flag = true;

        bs::avc::SliceGroupMaps maps;
        std::string error;
        expect(bs::avc::derive_mb_to_slice_group_map(
                       sps, pps, header, false, 8, maps, &error),
               "field slice group map derives", label);
        const std::vector<std::uint8_t> units = {
                0, 0, 0, 0, 1, 1, 1, 1};
        expect(maps.mb_to_group == units,
               "field picture uses the map-unit map directly", label);

        header.field_pic_flag = false;
        expect(bs::avc::derive_mb_to_slice_group_map(
                       sps, pps, header, true, 16, maps, &error),
               "MBAFF slice group map derives", label);
        const std::vector<std::uint8_t> mbaff = {
                0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1};
        expect(maps.mb_to_group == mbaff,
               "MBAFF macroblock map duplicates each map unit", label);

        expect(bs::avc::derive_mb_to_slice_group_map(
                       sps, pps, header, false, 16, maps, &error),
               "interlaced frame slice group map derives", label);
        expect(maps.mb_to_group == mbaff,
               "interlaced frame map follows equation 8-27", label);
    }
}

void test_stream(const char* stream_path) {
    const Reference reference =
            read_reference(std::string(stream_path) + ".fmo.txt", stream_path);
    const auto bytes = read_file(stream_path);

    bs::utils::MotionBufferPool pool({4, 16, 32u << 20, 64});
    Context context{&pool, 0, 0, false, {}};

    auto state = bs::create_state(bs::Codec::Avc);
    bs::AvcParsedHandlers handlers{};
    handlers.motion_vectors.ctx = &context;
    handlers.motion_vectors.acquire = &acquire;
    handlers.motion_vectors.ready = &ready;
    handlers.motion_vectors.options.emit_intra = true;
    (void)bs::parse(*state,
                    std::span<const std::uint8_t>(bytes.data(), bytes.size()),
                    bs::NalFramingMode::AnnexB,
                    handlers,
                    4);

    expect(context.pictures == 2, "FMO stream emits IDR and P pictures", stream_path);
    expect(context.ok == context.pictures, "every FMO picture is fully parsed",
           stream_path);
    const bs::avc::MotionVectorExtractor* extractor = state->avc_motion_vectors();
    expect(extractor != nullptr && extractor->status() == bs::avc::MotionVectorStatus::Ok,
           "FMO extractor status is Ok", stream_path);

    if (context.grids.size() != 2) {
        state->clear();
        pool.trimAll();
        return;
    }

    const Grid& grid = context.grids[1];
    expect(grid.width4x4 == 16 && grid.height4x4 == 16 && grid.cells.size() == 256,
           "FMO P grid is complete", stream_path);
    if (grid.cells.size() != 256) {
        state->clear();
        pool.trimAll();
        return;
    }

    int coded_mbs = 0;
    for (std::uint32_t mb = 0; mb < 16; ++mb) {
        for (std::uint32_t y = 0; y < 4; ++y) {
            for (std::uint32_t x = 0; x < 4; ++x) {
                const auto& cell = at(grid, mb, x, y);
                expect((cell.flags & bs::avc::MvUnavailable) == 0,
                       "every slice-group macroblock is covered", stream_path);
                expect((cell.flags & bs::avc::MvList0) != 0,
                       "slice-group cells carry list 0", stream_path);
                if ((cell.flags & bs::avc::MvSkip) == 0) {
                    ++coded_mbs;
                }
            }
        }
    }

    if (reference.cabac) {
        expect(reference.coded.empty(), "CABAC fixture has no coded macroblocks",
               stream_path);
        for (std::uint32_t mb = 0; mb < 16; ++mb) {
            for (std::uint32_t y = 0; y < 4; ++y) {
                for (std::uint32_t x = 0; x < 4; ++x) {
                    const auto& cell = at(grid, mb, x, y);
                    expect(cell.flags ==
                                   static_cast<std::uint8_t>(bs::avc::MvSkip |
                                                             bs::avc::MvList0),
                           "CABAC slice-group macroblock is skipped", stream_path);
                    expect(cell.mv[0][0] == 0 && cell.mv[0][1] == 0,
                           "CABAC skipped macroblock has zero motion", stream_path);
                }
            }
        }
    } else {
        expect(coded_mbs == static_cast<int>(reference.coded.size()) * 16,
               "CAVLC fixture has one coded macroblock per slice group", stream_path);
        for (const auto& coded : reference.coded) {
            for (std::uint32_t y = 0; y < 4; ++y) {
                for (std::uint32_t x = 0; x < 4; ++x) {
                    const auto& cell = at(grid, coded.mb, x, y);
                    expect(cell.flags == static_cast<std::uint8_t>(bs::avc::MvList0),
                           "CAVLC coded macroblock is list 0", stream_path);
                    expect(cell.mv[0][0] == coded.dx && cell.mv[0][1] == coded.dy,
                           "CAVLC coded macroblock MVD matches the fixture",
                           stream_path);
                }
            }
        }
    }

    state->clear();
    pool.trimAll();
    expect(pool.stats().buffersInUse == 0, "clear releases all FMO buffers",
           stream_path);
}

void test_refs_stream(const char* stream_path) {
    const Reference reference =
            read_reference(std::string(stream_path) + ".fmo.txt", stream_path);
    expect(reference.refs == 2,
           "per-slice-reference fixture carries two reference pictures", stream_path);
    if (reference.refs != 2) return;

    const auto bytes = read_file(stream_path);

    bs::utils::MotionBufferPool pool({4, 16, 32u << 20, 64});
    Context context{&pool, 0, 0, true, {}};

    auto state = bs::create_state(bs::Codec::Avc);
    bs::AvcParsedHandlers handlers{};
    handlers.motion_vectors.ctx = &context;
    handlers.motion_vectors.acquire = &acquire;
    handlers.motion_vectors.ready = &ready;
    handlers.motion_vectors.options.emit_intra = true;
    (void)bs::parse(*state,
                    std::span<const std::uint8_t>(bytes.data(), bytes.size()),
                    bs::NalFramingMode::AnnexB,
                    handlers,
                    4);

    expect(context.pictures == 4,
           "reference FMO stream emits IDR, two references and the target",
           stream_path);
    expect(context.ok == context.pictures,
           "every reference FMO picture is fully parsed", stream_path);
    const bs::avc::MotionVectorExtractor* extractor = state->avc_motion_vectors();
    expect(extractor != nullptr &&
                   extractor->status() == bs::avc::MotionVectorStatus::Ok,
           "reference FMO extractor status is Ok", stream_path);

    if (context.grids.size() != 4) {
        state->clear();
        context.grids.clear();
        pool.trimAll();
        return;
    }

    const Grid& target = context.grids.back();
    const bs::avc::MotionVectorBufferView& view = target.view;
    expect(target.header.version == bs::avc::kMotionVectorBufferVersion,
           "target picture uses the current buffer version", stream_path);
    expect(target.width4x4 == 16 && target.height4x4 == 16,
           "reference FMO target grid is complete", stream_path);
    expect(target.header.slice_ref_count ==
                   static_cast<std::uint32_t>(reference.groups),
           "target picture exposes one slice entry per group", stream_path);
    expect(target.header.slice_map_count > 0 &&
                   target.header.slice_map_coverage == 16,
           "target picture exposes the exact macroblock slice map", stream_path);
    expect(reference.project_poc != target.header.poc,
           "projection target differs from the picture POC", stream_path);
    const auto runs = bs::avc::motionSliceRuns(view);
    expect(runs.size() == target.header.slice_map_count,
           "macroblock slice map is readable through the public view", stream_path);

    /*
     * The picture-level convenience table is the first slice.  At least one
     * macroblock must resolve to a different POC, otherwise this fixture
     * would not distinguish the exact map from the old first-slice fallback.
     */
    bool fallback_differs = false;
    for (std::uint32_t mb = 0; mb < 16; ++mb) {
        const int group = reference.mb_group[mb];
        if (group < 0 ||
            static_cast<std::size_t>(group) >= reference.group_info.size()) {
            continue;
        }
        if (reference.group_info[static_cast<std::size_t>(group)].pocs[0] !=
            target.header.ref_poc[0][0]) {
            fallback_differs = true;
        }
    }
    expect(fallback_differs,
           "first-slice fallback would report the wrong POC somewhere",
           stream_path);

    const auto slices = bs::avc::motionSliceReferences(view);
    for (std::uint32_t mb = 0; mb < 16; ++mb) {
        const int group = reference.mb_group[mb];
        if (group < 0 ||
            static_cast<std::size_t>(group) >= reference.group_info.size()) {
            expect(false, "sidecar covers every macroblock", stream_path);
            continue;
        }
        const Reference::GroupInfo& info =
                reference.group_info[static_cast<std::size_t>(group)];

        std::uint32_t slice_index = bs::avc::kMotionSliceIndexNone;
        expect(bs::avc::motionSliceIndexAt(view, mb, slice_index),
               "macroblock slice map covers every address", stream_path);
        if (slice_index != bs::avc::kMotionSliceIndexNone) {
            expect(slice_index < slices.size(),
                   "macroblock slice index is inside the slice table", stream_path);
            if (slice_index < slices.size()) {
                expect(slices[slice_index].ref_poc[0][0] == info.pocs[0],
                       "macroblock map resolves the group's slice entry",
                       stream_path);
            }
        }

        for (std::uint32_t y = 0; y < 4; ++y) {
            for (std::uint32_t x = 0; x < 4; ++x) {
                const std::uint32_t gx = (mb % 4) * 4 + x;
                const std::uint32_t gy = (mb / 4) * 4 + y;
                const bs::avc::MvCell* cell = bs::avc::motionCellAt(view, gx, gy);
                expect(cell != nullptr, "reference FMO cell exists", stream_path);
                if (cell == nullptr) continue;
                expect(cell->ref_idx[0] == 0,
                       "reference FMO cell selects list 0 ref_idx 0", stream_path);
                for (std::size_t i = 0; i < info.pocs.size(); ++i) {
                    expect(bs::avc::motionRefPocAt(
                                   view, gx, gy, 0, static_cast<int>(i)) ==
                                   info.pocs[i],
                           "per-cell ref POC comes from the exact slice map",
                           stream_path);
                    expect(!bs::avc::motionRefNonExistingAt(
                                   view, gx, gy, 0, static_cast<int>(i)),
                           "reference FMO lists do not contain gaps", stream_path);
                }
                expect(bs::avc::motionRefPocAt(
                               view, gx, gy, 0,
                               static_cast<int>(info.pocs.size())) ==
                               bs::avc::kMotionVectorUnknownPoc,
                       "inactive reference index is unknown", stream_path);

                /*
                 * motionTo must scale with the same per-slice reference POC;
                 * using the first slice's table changes the result for every
                 * macroblock whose group uses a different list.
                 */
                const double scale =
                        static_cast<double>(reference.project_poc - target.header.poc) /
                        static_cast<double>(info.pocs[0] - target.header.poc);
                const bs::avc::MotionSample sample =
                        bs::avc::motionTo(view, gx, gy, reference.project_poc);
                expect(sample.valid, "per-slice motionTo is valid", stream_path);
                expect(std::fabs(sample.x - cell->mv[0][0] / 4.0 * scale) < 1e-5 &&
                               std::fabs(sample.y - cell->mv[0][1] / 4.0 * scale) <
                                       1e-5,
                       "per-slice motionTo uses the exact reference POC", stream_path);
            }
        }
    }

    /* CAVLC fixtures also prove the coded macroblock motion itself. */
    for (const auto& coded : reference.coded) {
        const std::uint32_t gx = (coded.mb % 4) * 4 + 1;
        const std::uint32_t gy = (coded.mb / 4) * 4 + 1;
        const bs::avc::MvCell* cell = bs::avc::motionCellAt(view, gx, gy);
        expect(cell != nullptr, "coded reference FMO cell exists", stream_path);
        if (cell == nullptr) continue;
        expect(cell->mv[0][0] == coded.dx && cell->mv[0][1] == coded.dy,
               "coded reference FMO macroblock MVD matches the fixture",
               stream_path);
    }

    /*
     * The immediate path writes the same metadata into the consumer buffer.
     * Map types 1 and 6 need 16 runs, which also exercises growing the
     * initial map reservation through a buffer re-acquisition.
     */
    {
        bs::utils::MotionBufferPool immediate_pool({4, 16, 32u << 20, 64});
        Context immediate{&immediate_pool, 0, 0, true, {}};
        auto immediate_state = bs::create_state(bs::Codec::Avc);
        bs::AvcParsedHandlers immediate_handlers{};
        immediate_handlers.motion_vectors.ctx = &immediate;
        immediate_handlers.motion_vectors.acquire = &acquire;
        immediate_handlers.motion_vectors.ready = &ready;
        immediate_handlers.motion_vectors.options.emit_intra = true;
        immediate_handlers.motion_vectors.options.defer_ready = false;
        (void)bs::parse(*immediate_state,
                        std::span<const std::uint8_t>(bytes.data(), bytes.size()),
                        bs::NalFramingMode::AnnexB,
                        immediate_handlers,
                        4);

        expect(immediate.grids.size() == 4,
               "immediate reference FMO stream emits every picture", stream_path);
        if (immediate.grids.size() == 4) {
            const Grid& last = immediate.grids.back();
            expect(last.header.slice_map_count > 0 &&
                           last.header.slice_map_coverage == 16,
                   "immediate path writes the exact macroblock slice map",
                   stream_path);
            for (std::uint32_t mb = 0; mb < 16; ++mb) {
                const int group = reference.mb_group[mb];
                if (group < 0 ||
                    static_cast<std::size_t>(group) >= reference.group_info.size()) {
                    continue;
                }
                const auto& info =
                        reference.group_info[static_cast<std::size_t>(group)];
                const std::uint32_t gx = (mb % 4) * 4 + 1;
                const std::uint32_t gy = (mb / 4) * 4 + 1;
                expect(bs::avc::motionRefPocAt(last.view, gx, gy, 0, 0) ==
                               info.pocs[0],
                       "immediate path resolves the exact slice POC", stream_path);
            }
        }
        immediate_state->clear();
        immediate.grids.clear();
        immediate_pool.trimAll();
        expect(immediate_pool.stats().buffersInUse == 0,
               "immediate reference FMO buffers are released", stream_path);
    }

    state->clear();
    context.grids.clear();
    pool.trimAll();
    expect(pool.stats().buffersInUse == 0,
           "deferred reference FMO buffers are released", stream_path);
}

/*
 * A picture may mix P and I slices.  The P group is decoded first; the later
 * I slice must mark only its own macroblocks intra, not erase the P grid.
 */
void test_mixed_i_p(const char* stream_path) {
    const auto bytes = read_file(stream_path);

    bs::utils::MotionBufferPool pool({4, 16, 32u << 20, 64});
    Context context{&pool, 0, 0, false, {}};

    auto state = bs::create_state(bs::Codec::Avc);
    bs::AvcParsedHandlers handlers{};
    handlers.motion_vectors.ctx = &context;
    handlers.motion_vectors.acquire = &acquire;
    handlers.motion_vectors.ready = &ready;
    handlers.motion_vectors.options.emit_intra = true;
    (void)bs::parse(*state,
                    std::span<const std::uint8_t>(bytes.data(), bytes.size()),
                    bs::NalFramingMode::AnnexB,
                    handlers,
                    4);

    expect(context.pictures == 2, "mixed I/P stream emits IDR and P pictures",
           stream_path);
    expect(context.ok == context.pictures, "mixed I/P pictures parse completely",
           stream_path);
    const bs::avc::MotionVectorExtractor* extractor = state->avc_motion_vectors();
    expect(extractor != nullptr && extractor->status() == bs::avc::MotionVectorStatus::Ok,
           "mixed I/P extractor status is Ok", stream_path);

    if (context.grids.size() == 2) {
        const Grid& grid = context.grids[1];
        expect(grid.width4x4 == 16 && grid.height4x4 == 16 && grid.cells.size() == 256,
               "mixed I/P grid is complete", stream_path);
        if (grid.cells.size() == 256) {
            for (std::uint32_t mb = 0; mb < 16; ++mb) {
                /* Map type 0 with run_length_minus1 = 1: two MBs per group. */
                const bool intra_group = (mb & 2u) != 0;
                for (std::uint32_t y = 0; y < 4; ++y) {
                    for (std::uint32_t x = 0; x < 4; ++x) {
                        const auto& cell = at(grid, mb, x, y);
                        if (intra_group) {
                            expect((cell.flags & bs::avc::MvIntra) != 0,
                                   "I-slice macroblock is intra", stream_path);
                        } else {
                            expect((cell.flags & bs::avc::MvIntra) == 0,
                                   "P-slice macroblock survives a later I slice",
                                   stream_path);
                            expect((cell.flags & bs::avc::MvUnavailable) == 0,
                                   "P-slice macroblock keeps motion data", stream_path);
                        }
                    }
                }
            }
            const auto& coded = at(grid, 0, 1, 1);
            expect(coded.mv[0][0] == -3 && coded.mv[0][1] == -3,
                   "P-slice macroblock keeps its coded MVD", stream_path);
            const auto& intra = at(grid, 2, 1, 1);
            expect(intra.ref_idx[0] == -1 && intra.ref_idx[1] == -1,
                   "I-slice macroblock has no reference index", stream_path);
        }
    }

    state->clear();
    pool.trimAll();
    expect(pool.stats().buffersInUse == 0, "mixed I/P buffers are released", stream_path);
}

}  // namespace

int main(int argc, char** argv) {
    test_map_derivation();

    if (argc < 2) {
        std::fprintf(stderr, "usage: avc_fmo_test <stream.h264> [...]\n");
        return 1;
    }
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]).find("_mixed_") != std::string::npos) {
            test_mixed_i_p(argv[i]);
        } else if (std::string(argv[i]).find("_refs_") != std::string::npos) {
            test_refs_stream(argv[i]);
        } else {
            test_stream(argv[i]);
        }
    }

    if (failures != 0) {
        std::fprintf(stderr, "%d slice-group failure(s)\n", failures);
        return 1;
    }
    std::puts("avc slice groups: ok");
    return 0;
}
