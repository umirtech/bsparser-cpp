/*
 * ---------------------------------------------------------------------------
 * VVC (H.266) unified parse test
 * ---------------------------------------------------------------------------
 *
 * Reads a real Annex-B VVC stream and verifies the unified
 * bs::parse dispatches the typed handlers and auto-stores the
 * parameter sets.
 */
#include "bsparser.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <vector>

namespace {

bs::vvc::SequenceParameterSet g_sps;
bs::vvc::PictureParameterSet g_pps;
bs::vvc::SliceHeader g_slice;
bool g_sps_hit = false;
bool g_pps_hit = false;
bool g_slice_hit = false;


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
                 : "tests/fuzz/corpus/vvc_sample.266";

    auto data = read_file(path);
    if (data.empty()) {
        return 2;
    }

    auto state = bs::create_state(bs::Codec::Vvc);

    bs::VvcParsedHandlers h{};
    h.vps = [](const bs::vvc::VideoParameterSet&) {};
    h.sps = [](const bs::vvc::SequenceParameterSet& s) {
        g_sps = s;
        g_sps_hit = true;
    };
    h.pps = [](const bs::vvc::PictureParameterSet& p) {
        g_pps = p;
        g_pps_hit = true;
    };
    h.slice = [](const bs::vvc::SliceHeader& s) {
        g_slice = s;
        g_slice_hit = true;
    };

    (void)bs::parse(*state, data, bs::NalFramingMode::AnnexB, h);

    int failures = 0;

    if (!g_sps_hit || !g_sps.valid()) {
        std::cerr << "VVC: SPS handler missing/invalid\n";
        ++failures;
    }

    if (!g_pps_hit ||
        !g_pps.valid() ||
        g_pps.pic_width_in_luma_samples != 160) {
        std::cerr << "VVC: PPS handler wrong (w="
                  << g_pps.pic_width_in_luma_samples << ")\n";
        ++failures;
    }

    if (!g_slice_hit) {
        std::cerr << "VVC: slice handler never fired\n";
        ++failures;
    }

    const auto* stored = state->vvc_sets();
    if (stored == nullptr ||
        stored->find_pps(0) == nullptr ||
        stored->find_sps(0) == nullptr) {
        std::cerr << "VVC: state did not store PPS/SPS\n";
        ++failures;
    }

    if (failures == 0) {
        std::cout << "VVC OK: " << g_pps.pic_width_in_luma_samples
                  << "x" << g_pps.pic_height_in_luma_samples
                  << " chroma="
                  << static_cast<int>(g_sps.chroma_format_idc)
                  << " slices=" << g_slice_hit << "\n";
    }

    return failures == 0 ? 0 : 1;
}
