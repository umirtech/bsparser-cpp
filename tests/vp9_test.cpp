// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * ---------------------------------------------------------------------------
 * VP9 unified parse test
 * ---------------------------------------------------------------------------
 *
 * Reads a real IVF file containing VP9 frames and verifies the
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

bs::vp9::FrameHeader g_fh;
bool g_hit = false;
bool g_raw_hit = false;
std::uint32_t g_raw_width = 0;
std::uint32_t g_raw_height = 0;
std::uint32_t g_key_width = 0;
std::uint32_t g_key_height = 0;

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
    const char* path = argc > 1 ? argv[1] : "tests/fuzz/corpus/vp9_sample.ivf";

    auto data = read_file(path);
    if (data.empty()) {
        return 2;
    }

    auto state = bs::create_state(bs::Codec::Vp9);

    bs::Vp9ParsedHandlers h{};
    h.frame_header = [](const bs::vp9::FrameHeader& f) {
        g_fh = f;
        g_hit = true;
        if (f.frame_type == bs::vp9::FrameType::KeyFrame) {
            g_key_width = f.width;
            g_key_height = f.height;
        }
    };

    (void)bs::parse(*state, data, bs::NalFramingMode::Ivf, h);

    /*
     * RawFrame path: parse the first IVF frame directly, without the
     * IVF 12-byte frame header.
     */
    if (data.size() >= 32 + 12) {
        const std::uint32_t frame_size =
            static_cast<std::uint32_t>(data[32]) |
            (static_cast<std::uint32_t>(data[33]) << 8) |
            (static_cast<std::uint32_t>(data[34]) << 16) |
            (static_cast<std::uint32_t>(data[35]) << 24);

        if (32 + 12 + frame_size <= data.size()) {
            bs::Vp9ParsedHandlers rh{};
            rh.frame_header = [](const bs::vp9::FrameHeader& f) {
                g_raw_hit = true;
                g_raw_width = f.width;
                g_raw_height = f.height;
            };

            (void)bs::parse(
                *state,
                std::span<const std::uint8_t>(data.data() + 32 + 12, frame_size),
                bs::NalFramingMode::RawFrame,
                rh
            );
        }
    }

    int failures = 0;

    if (!g_raw_hit) {
        std::cerr << "VP9: RawFrame handler missing\n";
        ++failures;
    }

    if (g_raw_width != g_key_width || g_raw_height != g_key_height) {
        std::cerr << "VP9: RawFrame dimensions wrong (" << g_raw_width << "x" << g_raw_height
                  << ")\n";
        ++failures;
    }

    if (!g_hit || !g_fh.valid()) {
        std::cerr << "VP9: frame header handler missing\n";
        ++failures;
    }

    if (g_key_width != 192 || g_key_height != 108) {
        std::cerr << "VP9: keyframe dimensions wrong (" << g_key_width << "x" << g_key_height
                  << ")\n";
        ++failures;
    }

    if (failures == 0) {
        std::cout << "VP9 OK: marker=" << static_cast<int>(g_fh.frame_marker)
                  << " profile=" << static_cast<int>(g_fh.profile)
                  << " frame_type=" << static_cast<int>(g_fh.frame_type) << " " << g_fh.width << "x"
                  << g_fh.height << "\n";
    }

    return failures == 0 ? 0 : 1;
}
