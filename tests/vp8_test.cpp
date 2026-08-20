// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * ---------------------------------------------------------------------------
 * VP8 unified parse test
 * ---------------------------------------------------------------------------
 *
 * Reads a real IVF file containing VP8 frames and verifies the
 * unified bs::parse dispatches the frame-header handler.
 */
#include "bsparser.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <vector>

namespace {

bs::vp8::FrameHeader g_fh;
bool g_hit = false;

std::vector<std::uint8_t> read_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "cannot open " << path << "\n";
        return {};
    }
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()
    );
}

}  // namespace

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "tests/fuzz/corpus/vp8_sample.ivf";

    auto data = read_file(path);
    if (data.empty()) {
        return 2;
    }

    auto state = bs::create_state(bs::Codec::Vp8);

    bs::Vp8ParsedHandlers h{};
    h.frame_header = [](const bs::vp8::FrameHeader& f) {
        g_fh = f;
        g_hit = true;
    };

    (void)bs::parse(*state, data, bs::NalFramingMode::Ivf, h);

    int failures = 0;

    if (!g_hit) {
        std::cerr << "VP8: frame header handler never fired\n";
        ++failures;
    }

    if (failures == 0) {
        std::cout << "VP8 OK: key=" << g_fh.key_frame
                  << " version=" << static_cast<int>(g_fh.version)
                  << " first_part_size=" << g_fh.first_part_size << " " << g_fh.width << "x"
                  << g_fh.height << "\n";
    }

    return failures == 0 ? 0 : 1;
}
