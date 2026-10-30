// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * 4:2:2/4:4:4/monochrome/PCM residual/MV fixture test.
 *
 * Each argument pair is "<stream.h264> <motion-reference.txt>".  A reference
 * of "-" skips the motion comparison and only verifies that the stream is
 * traversed completely.  Motion references are FFmpeg-derived per-4x4 list-0
 * fields for the P pictures; explicit status/rejection checks for I_PCM,
 * monochrome and separate colour planes live in avc_mv_pcm_test.
 */

#include "bsparser.hpp"
#include "utils/MotionBufferPool.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Grid {
    std::uint32_t width4x4 = 0;
    std::uint32_t height4x4 = 0;
    std::vector<bs::avc::MvCell> cells;
};

struct Stats {
    int pictures = 0;
    int ok = 0;
    int truncated = 0;
    int unsupported = 0;
    int other_failures = 0;
    std::vector<Grid> p_pictures;
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

    if ((view.header->flags & bs::avc::MvPictureIntra) != 0) return;
    if (view.header->num_ref_idx[1] != 0) return;

    Grid grid;
    grid.width4x4 = view.header->width4x4;
    grid.height4x4 = view.header->height4x4;
    const auto cells = bs::avc::motionCells(view);
    grid.cells.assign(cells.begin(), cells.end());
    stats->p_pictures.push_back(std::move(grid));
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

std::vector<Grid> read_reference(const char* path) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(1);
    }

    std::vector<Grid> pictures;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream header(line);
        std::string kind;
        header >> kind;
        if (kind != "picture") continue;

        Grid grid;
        int index = 0;
        header >> index >> grid.width4x4 >> grid.height4x4;
        grid.cells.resize(static_cast<std::size_t>(grid.width4x4) * grid.height4x4);
        for (auto& cell : grid.cells) {
            if (!std::getline(in, line)) {
                std::fprintf(stderr, "truncated reference %s\n", path);
                std::exit(1);
            }
            std::istringstream entry(line);
            int x = 0;
            int y = 0;
            int dx = 0;
            int dy = 0;
            entry >> x >> y >> dx >> dy;
            cell.mv[0][0] = static_cast<std::int16_t>(dx);
            cell.mv[0][1] = static_cast<std::int16_t>(dy);
        }
        pictures.push_back(std::move(grid));
    }
    return pictures;
}

int failures = 0;

void expect(bool condition, const char* message, const char* stream) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s (%s)\n", message, stream);
        ++failures;
    }
}

void test_stream(const char* stream_path, const char* reference_path) {
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

    expect(context.stats.pictures > 0, "stream produced pictures", stream_path);
    expect(context.stats.truncated == 0, "no truncated pictures", stream_path);
    expect(context.stats.unsupported == 0, "no unsupported pictures", stream_path);
    expect(context.stats.other_failures == 0, "no malformed pictures", stream_path);
    expect(context.stats.ok == context.stats.pictures,
           "every picture is fully parsed",
           stream_path);

    if (state->avc_motion_vectors() != nullptr) {
        expect(state->avc_motion_vectors()->status() == bs::avc::MotionVectorStatus::Ok,
               "extractor status is Ok",
               stream_path);
    } else {
        expect(false, "motion-vector extractor exists", stream_path);
    }
    if (std::string(reference_path) != "-") {
        const auto expected = read_reference(reference_path);
        expect(!expected.empty(), "reference contains P pictures", reference_path);
        expect(context.stats.p_pictures.size() >= expected.size(),
               "stream exposes the referenced P pictures",
               stream_path);

        const std::size_t count = std::min(expected.size(), context.stats.p_pictures.size());
        for (std::size_t i = 0; i < count; ++i) {
            const Grid& actual = context.stats.p_pictures[i];
            const Grid& want = expected[i];
            expect(actual.width4x4 == want.width4x4 && actual.height4x4 == want.height4x4,
                   "P-picture grid dimensions match FFmpeg",
                   stream_path);
            if (actual.width4x4 != want.width4x4 ||
                actual.height4x4 != want.height4x4) {
                continue;
            }
            expect(actual.cells.size() == want.cells.size(),
                   "P-picture cell count matches FFmpeg",
                   stream_path);
            if (actual.cells.size() != want.cells.size()) continue;

            bool match = true;
            for (std::size_t c = 0; c < want.cells.size(); ++c) {
                if (actual.cells[c].mv[0][0] != want.cells[c].mv[0][0] ||
                    actual.cells[c].mv[0][1] != want.cells[c].mv[0][1]) {
                    match = false;
                    break;
                }
            }
            expect(match, "P-picture list-0 motion matches FFmpeg", stream_path);
        }
    }

    state->clear();
    pool.trimAll();
    expect(pool.stats().buffersInUse == 0, "clear releases all buffers", stream_path);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3 || (argc % 2) == 0) {
        std::fprintf(stderr,
                     "usage: avc_mv_chroma_test <stream.h264> <reference.txt|-> "
                     "[<stream.h264> <reference.txt|-> ...]\n");
        return 1;
    }

    for (int i = 1; i + 1 < argc; i += 2) {
        test_stream(argv[i], argv[i + 1]);
    }

    if (failures != 0) {
        std::fprintf(stderr, "%d high-chroma motion-vector failure(s)\n", failures);
        return 1;
    }

    std::puts("avc 4:2:2/4:4:4 motion vectors: ok");
    return 0;
}
