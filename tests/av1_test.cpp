/*
 * ---------------------------------------------------------------------------
 * AV1 unified parse test
 * ---------------------------------------------------------------------------
 *
 * Reads a real AV1 low-overhead OBU stream and verifies the
 * unified bs::parse dispatches the sequence-header and
 * frame-header handlers.
 */
#include "bsparser.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <vector>

namespace {

bs::av1::SequenceHeader g_seq;
bs::av1::FrameHeader g_fh;
bool g_seq_hit = false;
bool g_fh_hit = false;


std::vector<std::uint8_t>
read_file(const char* path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "cannot open " << path << "\n";
        return {};
    }
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(f),
        std::istreambuf_iterator<char>());
}

} // namespace


int main(int argc, char** argv)
{
    const char* path =
        argc > 1 ? argv[1]
                 : "tests/fuzz/corpus/av1_sample.obu";

    auto data = read_file(path);
    if (data.empty()) {
        return 2;
    }

    auto state = bs::create_state(bs::Codec::Av1);

    bs::Av1ParsedHandlers h{};
    h.sequence_header = [](const bs::av1::SequenceHeader& s) {
        g_seq = s;
        g_seq_hit = true;
    };
    h.frame_header = [](const bs::av1::FrameHeader& f) {
        g_fh = f;
        g_fh_hit = true;
    };

    (void)bs::parse(*state, data, bs::NalFramingMode::Obu, h);

    int failures = 0;

    if (!g_seq_hit || !g_seq.valid()) {
        std::cerr << "AV1: sequence header handler missing\n";
        ++failures;
    }

    if (!g_fh_hit) {
        std::cerr << "AV1: frame header handler never fired\n";
        ++failures;
    }

    if (static_cast<int>(g_fh.frame_type) > 3) {
        std::cerr << "AV1: bad frame type\n";
        ++failures;
    }

    if (failures == 0) {
        std::cout << "AV1 OK: profile="
                  << static_cast<int>(g_seq.seq_profile)
                  << " frame_type="
                  << static_cast<int>(g_fh.frame_type)
                  << "\n";
    }

    return failures == 0 ? 0 : 1;
}
