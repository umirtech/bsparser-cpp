// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * ---------------------------------------------------------------------------
 * Container demux test
 * ---------------------------------------------------------------------------
 *
 * Reads a muxed file (MP4 / TS / FLV / AVI / IVF), auto-detects
 * the container, demuxes it to an elementary stream and parses
 * it with the unified bs::parse API.
 *
 * Usage: bs_demux_test <muxed-file> <expected-codec>
 *        expected-codec: hevc|avc|vvc|av1|vp9|vp8
 */
#include "bsparser.hpp"
#include <demux/demuxer.hpp>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> read_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return {};
    }
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()
    );
}

bool g_fired = false;

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: bs_demux_test <file> <codec>\n";
        return 2;
    }

    auto data = read_file(argv[1]);
    if (data.empty()) {
        std::cerr << "cannot open " << argv[1] << "\n";
        return 2;
    }

    const bs::demux::Container container = bs::demux::sniff(data);

    if (container == bs::demux::Container::Unknown) {
        std::cerr << "container not detected\n";
        return 1;
    }

    const bs::demux::ElementaryStream es = bs::demux::demux(data);

    if (!es.ok || es.bytes.empty()) {
        std::cerr << "demux failed\n";
        return 1;
    }

    const char* want = argv[2];

    const char* got = es.codec == bs::Codec::Hevc  ? "hevc"
                      : es.codec == bs::Codec::Avc ? "avc"
                      : es.codec == bs::Codec::Vvc ? "vvc"
                      : es.codec == bs::Codec::Av1 ? "av1"
                      : es.codec == bs::Codec::Vp9 ? "vp9"
                      : es.codec == bs::Codec::Vp8 ? "vp8"
                                                   : "?";

    if (std::strcmp(want, got) != 0) {
        std::cerr << "codec mismatch: expected " << want << " got " << got << "\n";
        return 1;
    }

    /*
     * Parse the demuxed elementary stream and verify at least one
     * unit handler fired. The bs::parse return value counts raw
     * dispatch, so use handler flags instead.
     */
    auto state = bs::create_state(es.codec);

    g_fired = false;

    switch (es.codec) {
        case bs::Codec::Hevc: {
            bs::HevcParsedHandlers h{};
            h.vps = [](const bs::VideoParameterSet&) { g_fired = true; };
            h.sps = [](const bs::SequenceParameterSet&) { g_fired = true; };
            h.pps = [](const bs::PictureParameterSet&) { g_fired = true; };
            h.slice = [](const bs::SliceSegmentHeader&) { g_fired = true; };
            (void)bs::parse(*state, es.bytes, es.framing, h);
            break;
        }

        case bs::Codec::Avc: {
            bs::AvcParsedHandlers h{};
            h.sps = [](const bs::avc::SequenceParameterSet&) { g_fired = true; };
            h.pps = [](const bs::avc::PictureParameterSet&) { g_fired = true; };
            h.slice = [](const bs::avc::SliceHeader&) { g_fired = true; };
            (void)bs::parse(*state, es.bytes, es.framing, h);
            break;
        }

        case bs::Codec::Vvc: {
            bs::VvcParsedHandlers h{};
            h.sps = [](const bs::vvc::SequenceParameterSet&) { g_fired = true; };
            h.pps = [](const bs::vvc::PictureParameterSet&) { g_fired = true; };
            h.slice = [](const bs::vvc::SliceHeader&) { g_fired = true; };
            (void)bs::parse(*state, es.bytes, es.framing, h);
            break;
        }

        case bs::Codec::Av1: {
            bs::Av1ParsedHandlers h{};
            h.sequence_header = [](const bs::av1::SequenceHeader&) { g_fired = true; };
            h.frame_header = [](const bs::av1::FrameHeader&) { g_fired = true; };
            (void)bs::parse(*state, es.bytes, es.framing, h);
            break;
        }

        case bs::Codec::Vp9: {
            bs::Vp9ParsedHandlers h{};
            h.frame_header = [](const bs::vp9::FrameHeader&) { g_fired = true; };
            (void)bs::parse(*state, es.bytes, es.framing, h);
            break;
        }

        case bs::Codec::Vp8: {
            bs::Vp8ParsedHandlers h{};
            h.frame_header = [](const bs::vp8::FrameHeader&) { g_fired = true; };
            (void)bs::parse(*state, es.bytes, es.framing, h);
            break;
        }

        default:
            break;
    }

    if (!g_fired) {
        std::cerr << "no units parsed from demuxed stream\n";
        return 1;
    }

    std::cout << "demux OK: container=" << static_cast<unsigned>(container) << " codec=" << got
              << " bytes=" << es.bytes.size() << " " << es.width << "x" << es.height << "\n";

    return 0;
}
