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
    };

    (void)bs::parse(*state, data, bs::NalFramingMode::Ivf, h);

    int failures = 0;

    if (!g_hit || !g_fh.valid()) {
        std::cerr << "VP9: frame header handler missing\n";
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
