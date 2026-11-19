// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * Mux/demux round-trip integration test.
 *
 * For every source file passed on the command line we demux its video
 * tracks, feed the resulting zero-copy samples through the mux/ layer into
 * each compatible container (using a streaming path sink), then demux the
 * produced file again and verify that the container/track metadata survives
 * and that the samples still parse.  Sample payloads point into the source
 * demuxer's mmap, so the source Demuxer is kept alive until after
 * muxing/finalizing.
 */
#include <mux/muxer.hpp>
#include <demux/demuxer.hpp>
#include "test_temp.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

int g_failures = 0;
std::size_t g_parse_units = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::cerr << "FAIL: " #cond " at line " << __LINE__ << "\n";  \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

/* Mirrors parse_sample() in tests/demux_stream_test.cpp. */
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

        default:
            break;
    }

    return g_parse_units - before;
}

const char* container_name(bs::mux::Container container) noexcept {
    switch (container) {
        case bs::mux::Container::Ivf:
            return "ivf";
        case bs::mux::Container::Mp4:
            return "mp4";
        case bs::mux::Container::Mkv:
            return "mkv";
        default:
            return "unknown";
    }
}

/* Unique output path for a streaming round-trip case. */
std::string temp_output_path(bs::mux::Container container) {
    const std::string name = container_name(container);
    return bs_test::temp_path("mux_rt_" + name, "." + name);
}

void test_file(const std::string& path) {
    auto demuxer = bs::demux::Demuxer::init(path);
    if (!demuxer.ok()) {
        std::cerr << "cannot open " << path << "\n";
        ++g_failures;
        return;
    }

    /*
     * Collect all video tracks and their samples, in track order.  The
     * Sample data spans point into demuxer's mmap, so `demuxer` must stay
     * alive until after every finalize() below.
     */
    std::vector<bs::mux::TrackConfig> configs;
    std::vector<std::vector<bs::mux::Sample>> samples;
    std::vector<std::size_t> expected_counts;

    for (auto& track : demuxer.tracks()) {
        if (!track.is_video) {
            continue;
        }

        bs::mux::TrackConfig cfg;
        cfg.id = track.id;
        cfg.codec = track.codec;
        cfg.framing = track.framing;
        cfg.codec_name = track.codec_name;
        cfg.width = track.width;
        cfg.height = track.height;
        cfg.timescale = track.timescale;
        cfg.language = track.language;
        cfg.is_video = track.is_video;
        cfg.length_size = track.length_size;

        std::vector<bs::mux::Sample> track_samples;
        while (track.hasNext()) {
            const bs::demux::SampleView sv = track.next();
            if (!sv) {
                break;
            }

            bs::mux::Sample sample;
            sample.data = sv.data;
            sample.pts = sv.pts;
            sample.dts = sv.dts;
            sample.duration = sv.duration;
            sample.is_key = sv.is_key;
            sample.framing = sv.framing;
            sample.track_id = cfg.id;
            track_samples.push_back(sample);
        }

        configs.push_back(cfg);
        samples.push_back(std::move(track_samples));
        expected_counts.push_back(samples.back().size());
    }

    if (configs.empty()) {
        std::cerr << "no video tracks in " << path << "\n";
        ++g_failures;
        return;
    }

    const bs::mux::Container containers[] = {
        bs::mux::Container::Ivf,
        bs::mux::Container::Mp4,
        bs::mux::Container::Mkv,
    };

    for (const bs::mux::Container container : containers) {
        /* IVF is single-stream: use only the first video track. */
        const std::size_t written = (container == bs::mux::Container::Ivf)
                                        ? std::size_t{1}
                                        : configs.size();

        /* Skip the container if any track we would write is incompatible. */
        bool supported = true;
        for (std::size_t i = 0; i < written; ++i) {
            if (!bs::mux::container_supports_codec(container, configs[i].codec)) {
                supported = false;
                break;
            }
        }
        if (!supported) {
            continue;
        }

        const int failures_before = g_failures;

        const std::string out_path = temp_output_path(container);
        std::error_code ec;
        (void)std::filesystem::remove(out_path, ec);

        auto muxer = bs::mux::Muxer::create(container, out_path);
        CHECK(muxer.ok());
        std::vector<bs::mux::Track> handles;
        for (std::size_t i = 0; i < written; ++i) {
            handles.push_back(muxer.add_track(configs[i]));
        }
        for (std::size_t i = 0; i < written; ++i) {
            for (const auto& sample : samples[i]) {
                handles[i].add_sample(sample);
            }
        }

        CHECK(muxer.finalize());
        CHECK(muxer.size() > 0);

        /* Feed the finalized file straight back through the demuxer. */
        auto back = bs::demux::Demuxer::init(out_path);
        CHECK(back.ok());
        if (!back.ok()) {
            std::cout << path << " -> " << container_name(container) << ": "
                      << (g_failures == failures_before ? "OK" : "FAIL") << "\n";
            (void)std::filesystem::remove(out_path, ec);
            continue;
        }

        CHECK(back.container() == bs::mux::to_demux(container));
        auto back_tracks = back.tracks();
        CHECK(back_tracks.size() == written);

        for (std::size_t i = 0; i < written && i < back_tracks.size(); ++i) {
            auto& bt = back_tracks[i];
            const bs::mux::TrackConfig& cfg = configs[i];

            CHECK(bt.codec == cfg.codec);
            CHECK(bt.sampleCount() > 0);
            CHECK(bt.hasNext());

            /* Source dimensions are nonzero in the corpus; the MKV writer
             * only falls back to 16 when they were zero. */
            if (cfg.width != 0u && cfg.height != 0u) {
                CHECK(bt.width == cfg.width);
                CHECK(bt.height == cfg.height);
            }

            auto state = bs::create_state(bt.codec);
            std::size_t count = 0;
            std::size_t parsed_units = 0;
            while (bt.hasNext()) {
                const auto sample = bt.next();
                if (!sample) {
                    break;
                }
                parsed_units += parse_sample(bt, sample, *state);
                ++count;
            }

            /* Exact PTS is not required (MKV clamps to int16, MP4 uses ms),
             * but the number of samples must round-trip and at least one
             * sample over the whole track must parse.  A leading
             * parameter-set pseudo-sample in the source is folded into the
             * next access unit by the writer, so the demuxed count may be
             * one lower; MKV may instead surface its CodecPrivate parameter
             * sets as one extra synthetic sample. */
            CHECK(count == expected_counts[i] ||
                  (expected_counts[i] > 0u && count == expected_counts[i] - 1u) ||
                  count == expected_counts[i] + 1u);
            CHECK(parsed_units > 0);
        }

        std::cout << path << " -> " << container_name(container) << ": "
                  << (g_failures == failures_before ? "OK" : "FAIL") << "\n";
        (void)std::filesystem::remove(out_path, ec);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: bs_mux_test <file> [file...]\n";
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
