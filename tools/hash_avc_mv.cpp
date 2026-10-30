// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * hash_avc_mv — deterministic H.264 motion-vector output hash.
 *
 * Development tool.  It parses one or more Annex-B H.264 streams with MV
 * extraction in immediate mode and folds every delivered value into an
 * FNV-1a 64-bit hash, so two builds/platforms can be compared without
 * storing the multi-megabyte dense grids:
 *
 *     cmake -B build-bench -DBS_BUILD_BENCHMARKS=ON
 *     cmake --build build-bench --target hash_avc_mv
 *     ./build-bench/hash_avc_mv bench_720p.h264 > hashes.txt
 *
 * Each line is:
 *
 *     <16-hex hash> <pictures> <cells> <path>
 *
 * The hash covers the picture header fields (poc, frame_num, flags,
 * dimensions, status, colour plane, plane count, reference POCs, active
 * reference counts and non-existing masks), the packed MvCell grid, the
 * per-slice reference tables and the per-macroblock slice runs.  Integer
 * fields are hashed in native byte order; hashes are therefore comparable
 * between builds on the same architecture and byte order.
 *
 * This tool is not built by default and is not part of ctest.
 */

#include "bsparser.hpp"
#include "utils/MotionBufferPool.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <span>
#include <vector>

namespace {

constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

std::uint64_t g_hash = kFnvOffsetBasis;
std::size_t g_pictures = 0;
std::size_t g_cells = 0;

void mix(const void* data, std::size_t bytes) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < bytes; ++i) {
        g_hash ^= p[i];
        g_hash *= kFnvPrime;
    }
}

template <typename T>
void mixValue(const T& value) {
    mix(&value, sizeof(value));
}

struct Context {
    bs::utils::MotionBufferPool* pool = nullptr;
};

bs::avc::MotionVectorBuffer acquire(void* opaque,
                                    const bs::avc::MotionVectorBufferRequest& request) {
    return static_cast<Context*>(opaque)->pool->acquire(request);
}

void ready(void* opaque, const bs::avc::MotionVectorBufferView& view) {
    (void)opaque;
    if (view.header == nullptr) {
        return;
    }

    const auto& h = *view.header;
    mixValue(h.poc);
    mixValue(h.frame_num);
    mixValue(h.flags);
    mixValue(h.width4x4);
    mixValue(h.height4x4);
    mixValue(h.status);
    mixValue(h.colour_plane_id);
    mixValue(h.plane_count);
    mixValue(h.ref_poc);
    mixValue(h.num_ref_idx);
    mixValue(h.ref_non_existing_mask);

    const auto cells = bs::avc::motionCells(view);
    g_cells += cells.size();
    mix(cells.data(), cells.size() * sizeof(bs::avc::MvCell));

    const auto slices = bs::avc::motionSliceReferences(view);
    for (const auto& s : slices) {
        mixValue(s.mb_addr_begin);
        mixValue(s.mb_addr_end);
        mixValue(s.colour_plane_id);
        mixValue(s.flags);
        mixValue(s.num_ref_idx);
        mixValue(s.non_existing_mask);
        mixValue(s.ref_poc);
    }

    const auto runs = bs::avc::motionSliceRuns(view);
    for (const auto& r : runs) {
        mixValue(r.mb_addr);
        mixValue(r.slice_index);
    }

    ++g_pictures;
}

std::vector<std::uint8_t> readFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "hash_avc_mv: cannot open %s\n", path);
        std::exit(1);
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

void usage(const char* program) {
    std::fprintf(stderr,
                 "usage: %s <stream.h264> [...]\n"
                 "\n"
                 "Prints one line per input:\n"
                 "  <fnv1a-64 hash> <pictures> <cells> <path>\n"
                 "\n"
                 "Exit status is non-zero when an input cannot be opened.\n",
                 program);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    if (std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0) {
        usage(argv[0]);
        return 0;
    }

    for (int i = 1; i < argc; ++i) {
        g_hash = kFnvOffsetBasis;
        g_pictures = 0;
        g_cells = 0;

        const auto bytes = readFile(argv[i]);

        bs::utils::MotionBufferPool pool({8, 64, 512u << 20, 64});
        Context context{&pool};

        auto state = bs::create_state(bs::Codec::Avc);
        bs::AvcParsedHandlers handlers{};
        handlers.motion_vectors.ctx = &context;
        handlers.motion_vectors.acquire = &acquire;
        handlers.motion_vectors.ready = &ready;
        handlers.motion_vectors.options.emit_intra = true;
        handlers.motion_vectors.options.defer_ready = false;

        (void)bs::parse(*state,
                        std::span<const std::uint8_t>(bytes.data(), bytes.size()),
                        bs::NalFramingMode::AnnexB,
                        handlers,
                        4);

        std::printf("%016llx %5zu %8zu %s\n",
                    static_cast<unsigned long long>(g_hash),
                    g_pictures,
                    g_cells,
                    argv[i]);

        const auto* extractor = state->avc_motion_vectors();
        if (extractor != nullptr && extractor->status() != bs::avc::MotionVectorStatus::Ok) {
            std::printf("  status=%d\n", static_cast<int>(extractor->status()));
        }

        state->clear();
        pool.trimAll();
    }

    return 0;
}
