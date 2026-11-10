// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * Streaming demuxer test: open a muxed file, enumerate tracks, pull
 * samples one at a time, parse each zero-copy sample, and exercise
 * index / timestamp seeking.
 */
#include "demux/demuxer.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
std::size_t g_parse_units = 0;

#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            std::cerr << "FAIL: " #cond " at line " << __LINE__ << "\n"; \
            ++g_failures;                                        \
        }                                                        \
    } while (0)

std::size_t parse_sample(
    const bs::demux::Track& track, const bs::demux::SampleView& sample, bs::State& state
) {
    const std::size_t before = g_parse_units;

    switch (track.codec) {
        case bs::Codec::Hevc: {
            bs::HevcParsedHandlers h{};
            h.vps = [](const bs::VideoParameterSet&) { ++g_parse_units; };
            h.sps = [](const bs::SequenceParameterSet&) { ++g_parse_units; };
            h.pps = [](const bs::PictureParameterSet&) { ++g_parse_units; };
            h.slice = [](const bs::SliceSegmentHeader&) { ++g_parse_units; };
            (void)bs::parse(state, sample.data, sample.framing, h);
            return g_parse_units - before;
        }

        case bs::Codec::Avc: {
            bs::AvcParsedHandlers h{};
            h.sps = [](const bs::avc::SequenceParameterSet&) { ++g_parse_units; };
            h.pps = [](const bs::avc::PictureParameterSet&) { ++g_parse_units; };
            h.slice = [](const bs::avc::SliceHeader&) { ++g_parse_units; };
            (void)bs::parse(state, sample.data, sample.framing, h);
            return g_parse_units - before;
        }

        case bs::Codec::Vvc: {
            bs::VvcParsedHandlers h{};
            h.sps = [](const bs::vvc::SequenceParameterSet&) { ++g_parse_units; };
            h.pps = [](const bs::vvc::PictureParameterSet&) { ++g_parse_units; };
            h.slice = [](const bs::vvc::SliceHeader&) { ++g_parse_units; };
            (void)bs::parse(state, sample.data, sample.framing, h);
            return g_parse_units - before;
        }

        case bs::Codec::Av1: {
            bs::Av1ParsedHandlers h{};
            h.sequence_header = [](const bs::av1::SequenceHeader&) { ++g_parse_units; };
            h.frame_header = [](const bs::av1::FrameHeader&) { ++g_parse_units; };
            (void)bs::parse(state, sample.data, sample.framing, h);
            return g_parse_units - before;
        }

        case bs::Codec::Vp9: {
            bs::Vp9ParsedHandlers h{};
            h.frame_header = [](const bs::vp9::FrameHeader&) { ++g_parse_units; };
            (void)bs::parse(state, sample.data, sample.framing, h);
            return g_parse_units - before;
        }

        case bs::Codec::Vp8: {
            bs::Vp8ParsedHandlers h{};
            h.frame_header = [](const bs::vp8::FrameHeader&) { ++g_parse_units; };
            (void)bs::parse(state, sample.data, sample.framing, h);
            return g_parse_units - before;
        }
    }

    return g_parse_units - before;
}

void test_file(const std::string& path) {
    auto demuxer = bs::demux::Demuxer::init(path);

    if (!demuxer.ok()) {
        std::cerr << "cannot open " << path << "\n";
        ++g_failures;
        return;
    }

    auto tracks = demuxer.tracks();
    CHECK(!tracks.empty());

    bool saw_video = false;

    for (auto& track : tracks) {
        if (!track.is_video) {
            continue;
        }

        saw_video = true;
        CHECK(track.sampleCount() > 0);
        CHECK(track.hasNext());

        auto state = bs::create_state(track.codec);
        std::size_t count = 0;
        std::size_t parsed_units = 0;
        std::int64_t last_pts = -1;
        bool pts_sorted = true;

        while (track.hasNext()) {
            auto sample = track.next();
            if (!sample) {
                break;
            }

            CHECK(!sample.data.empty());
            CHECK(sample.mmap_backed == track.mmap_backed || sample.index == 0);

            if (sample.pts < last_pts) {
                pts_sorted = false;
            }
            last_pts = sample.pts;

            parsed_units += parse_sample(track, sample, *state);
            ++count;
        }

        CHECK(count == track.sampleCount());
        CHECK(parsed_units > 0);

        if (track.sampleCount() > 1) {
            CHECK(track.seek(1));
            auto sample = track.next();
            CHECK(sample);
            CHECK(sample.index == 1);

            CHECK(track.seek(0));
            sample = track.next();
            CHECK(sample);
            CHECK(sample.index == 0);

            /*
             * Timestamp seeking is only meaningful when the container
             * exposes a monotonic PTS index (TS PTS may be decode order).
             */
            if (pts_sorted && demuxer.container() != bs::demux::Container::MpegTs) {
                CHECK(track.seek_time(sample.pts));
                sample = track.next();
                CHECK(sample);
                CHECK(sample.index == 0);
            }
        }
    }

    CHECK(saw_video);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bs_demux_stream_test <file> [file...]\n";
        return 2;
    }

    for (int i = 1; i < argc; ++i) {
        test_file(argv[i]);
    }

    if (g_failures != 0) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }

    return 0;
}
