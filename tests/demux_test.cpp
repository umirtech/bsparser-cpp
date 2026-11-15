// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * ---------------------------------------------------------------------------
 * Container demux test
 * ---------------------------------------------------------------------------
 *
 * Reads a muxed file (MP4 / TS / FLV / AVI / IVF / MKV / Ogg / PS),
 * auto-detects the container through the streaming Demuxer, selects the
 * first video track and parses every zero-copy sample with the unified
 * bs::parse API.
 *
 * Usage: bs_demux_test <muxed-file> <expected-codec>
 *        expected-codec: hevc|avc|vvc|av1|vp9|vp8
 */
#include "bsparser.hpp"
#include <demux/demuxer.hpp>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

namespace {

bool g_fired = false;

void parse_sample(
    const bs::demux::Track& track, const bs::demux::SampleView& sample, bs::State& state
) {
    switch (track.codec) {
        case bs::Codec::Hevc: {
            bs::HevcParsedHandlers h{};
            h.vps = [](const bs::VideoParameterSet&) { g_fired = true; };
            h.sps = [](const bs::SequenceParameterSet&) { g_fired = true; };
            h.pps = [](const bs::PictureParameterSet&) { g_fired = true; };
            h.slice = [](const bs::SliceSegmentHeader&) { g_fired = true; };
            (void)bs::parse(state, sample.data, sample.framing, h);
            break;
        }

        case bs::Codec::Avc: {
            bs::AvcParsedHandlers h{};
            h.sps = [](const bs::avc::SequenceParameterSet&) { g_fired = true; };
            h.pps = [](const bs::avc::PictureParameterSet&) { g_fired = true; };
            h.slice = [](const bs::avc::SliceHeader&) { g_fired = true; };
            (void)bs::parse(state, sample.data, sample.framing, h);
            break;
        }

        case bs::Codec::Vvc: {
            bs::VvcParsedHandlers h{};
            h.sps = [](const bs::vvc::SequenceParameterSet&) { g_fired = true; };
            h.pps = [](const bs::vvc::PictureParameterSet&) { g_fired = true; };
            h.slice = [](const bs::vvc::SliceHeader&) { g_fired = true; };
            (void)bs::parse(state, sample.data, sample.framing, h);
            break;
        }

        case bs::Codec::Av1: {
            bs::Av1ParsedHandlers h{};
            h.sequence_header = [](const bs::av1::SequenceHeader&) { g_fired = true; };
            h.frame_header = [](const bs::av1::FrameHeader&) { g_fired = true; };
            (void)bs::parse(state, sample.data, sample.framing, h);
            break;
        }

        case bs::Codec::Vp9: {
            bs::Vp9ParsedHandlers h{};
            h.frame_header = [](const bs::vp9::FrameHeader&) { g_fired = true; };
            (void)bs::parse(state, sample.data, sample.framing, h);
            break;
        }

        case bs::Codec::Vp8: {
            bs::Vp8ParsedHandlers h{};
            h.frame_header = [](const bs::vp8::FrameHeader&) { g_fired = true; };
            (void)bs::parse(state, sample.data, sample.framing, h);
            break;
        }
    }
}

const char* codec_name(bs::Codec codec) {
    switch (codec) {
        case bs::Codec::Hevc:
            return "hevc";
        case bs::Codec::Avc:
            return "avc";
        case bs::Codec::Vvc:
            return "vvc";
        case bs::Codec::Av1:
            return "av1";
        case bs::Codec::Vp9:
            return "vp9";
        case bs::Codec::Vp8:
            return "vp8";
    }

    return "?";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: bs_demux_test <file> <codec>\n";
        return 2;
    }

    const auto demuxer = bs::demux::Demuxer::init(argv[1]);

    if (!demuxer.ok()) {
        std::cerr << "demux failed\n";
        return 1;
    }

    auto tracks = demuxer.tracks();
    bs::demux::Track* video = nullptr;

    for (auto& track : tracks) {
        if (track.is_video && track.sampleCount() > 0) {
            video = &track;
            break;
        }
    }

    if (video == nullptr) {
        std::cerr << "no video track\n";
        return 1;
    }

    const char* want = argv[2];
    const char* got = codec_name(video->codec);

    if (std::strcmp(want, got) != 0) {
        std::cerr << "codec mismatch: expected " << want << " got " << got << "\n";
        return 1;
    }

    g_fired = false;

    auto state = bs::create_state(video->codec);
    bs::demux::SampleView sample;

    while (video->hasNext() && video->next(sample)) {
        parse_sample(*video, sample, *state);
    }

    if (!g_fired) {
        std::cerr << "no units parsed from demuxed stream\n";
        return 1;
    }

    std::cout << "demux OK: container=" << static_cast<unsigned>(demuxer.container())
              << " codec=" << got << " samples=" << video->sampleCount() << " " << video->width
              << "x" << video->height << "\n";

    return 0;
}
