// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * Streaming muxer tests: path/fd sinks write through OS-backed pages with
 * bounded memory, support caller-driven interleaving, and can force the
 * buffered fallback.  Output is verified by demuxing it back.
 */
#include <mux/muxer.hpp>
#include <demux/demuxer.hpp>
#include "test_temp.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cerr << "FAIL: " #cond " at line " << __LINE__ << "\n";     \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

// One AVC access unit: SPS followed by a minimal IDR slice NAL, so it is
// not mistaken for a parameter-set-only pseudo-sample.
const std::vector<std::uint8_t> kAvcNal{
    0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1E, 0xAA, 0xBB,
    0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00, 0x33, 0xFF
};

bs::mux::TrackConfig avc_cfg(std::uint32_t id) {
    bs::mux::TrackConfig c;
    c.id = id;
    c.codec = bs::Codec::Avc;
    c.width = 320;
    c.height = 240;
    c.timescale = 1000;
    return c;
}

void verify_mp4(const std::string& path, std::size_t expected_samples) {
    auto d = bs::demux::Demuxer::init(path);
    CHECK(d.ok());
    if (!d.ok()) {
        return;
    }
    CHECK(d.container() == bs::demux::Container::Mp4);
    auto tracks = d.tracks();
    CHECK(tracks.size() == 1);
    if (tracks.empty()) {
        return;
    }
    CHECK(tracks[0].codec == bs::Codec::Avc);
    CHECK(tracks[0].width == 320);
    CHECK(tracks[0].height == 240);
    CHECK(tracks[0].sampleCount() == expected_samples);
}

void test_mapped_stream() {
    const std::string path = bs_test::temp_dir() + "/mux_streaming_mapped.mp4";
    std::error_code ec;
    (void)std::filesystem::remove(path, ec);

    auto m = bs::mux::Muxer::create(bs::mux::Container::Mp4, path, 4096);
    CHECK(m.ok());
    auto t = m.add_track(avc_cfg(1));
    for (int i = 0; i < 20; ++i) {
        t.add_sample(kAvcNal, i * 1000, i * 1000, 1000, i % 4 == 0,
                     bs::NalFramingMode::AnnexB);
    }
    CHECK(m.finalize());
    verify_mp4(path, 20);
    (void)std::filesystem::remove(path, ec);
}

void test_buffered_fallback() {
    const std::string path = bs_test::temp_dir() + "/mux_streaming_buffered.mp4";
    std::error_code ec;
    (void)std::filesystem::remove(path, ec);

    // try_mmap=false forces the write-through fallback.
    auto m = bs::mux::Muxer::create(bs::mux::Container::Mp4, path, 4096, false);
    CHECK(m.ok());
    auto t = m.add_track(avc_cfg(1));
    for (int i = 0; i < 12; ++i) {
        t.add_sample(kAvcNal, i * 1000, i * 1000, 1000, true,
                     bs::NalFramingMode::AnnexB);
    }
    CHECK(m.finalize());
    verify_mp4(path, 12);
    (void)std::filesystem::remove(path, ec);
}

void test_growth() {
    // Tiny initial mapping forces several remaps during the run.
    const std::string path = bs_test::temp_dir() + "/mux_streaming_growth.mp4";
    std::error_code ec;
    (void)std::filesystem::remove(path, ec);

    auto m = bs::mux::Muxer::create(bs::mux::Container::Mp4, path, 4096);
    CHECK(m.ok());
    auto t = m.add_track(avc_cfg(1));
    for (int i = 0; i < 300; ++i) {
        t.add_sample(kAvcNal, i * 1000, i * 1000, 1000, true,
                     bs::NalFramingMode::AnnexB);
    }
    CHECK(m.finalize());
    const auto size = std::filesystem::file_size(path, ec);
    CHECK(size > 4096u);
    verify_mp4(path, 300);
    (void)std::filesystem::remove(path, ec);
}

void test_interleaved() {
    // Caller drives the physical order: track A, B, A, B, ...
    const std::string path = bs_test::temp_dir() + "/mux_streaming_interleaved.mp4";
    std::error_code ec;
    (void)std::filesystem::remove(path, ec);

    auto m = bs::mux::Muxer::create(bs::mux::Container::Mp4, path, 4096);
    CHECK(m.ok());
    auto a = m.add_track(avc_cfg(1));
    auto b = m.add_track(avc_cfg(2));
    for (int i = 0; i < 12; ++i) {
        a.add_sample(kAvcNal, i * 2000, i * 2000, 1000, i % 3 == 0,
                     bs::NalFramingMode::AnnexB);
        b.add_sample(kAvcNal, i * 2000 + 1000, i * 2000 + 1000, 1000, true,
                     bs::NalFramingMode::AnnexB);
    }
    CHECK(m.finalize());

    auto d = bs::demux::Demuxer::init(path);
    CHECK(d.ok());
    if (d.ok()) {
        auto tracks = d.tracks();
        CHECK(tracks.size() == 2);
        if (tracks.size() == 2) {
            CHECK(tracks[0].sampleCount() == 12);
            CHECK(tracks[1].sampleCount() == 12);
            std::size_t keys = 0;
            while (tracks[0].hasNext()) {
                auto s = tracks[0].next();
                if (!s) break;
                if (s.is_key) ++keys;
            }
            CHECK(keys == 4);
        }
    }
    (void)std::filesystem::remove(path, ec);
}

void test_fast_start_and_callbacks() {
    const std::string path = bs_test::temp_dir() + "/mux_streaming_faststart.mp4";
    std::error_code ec;
    (void)std::filesystem::remove(path, ec);

    struct State {
        int progress = 0;
        int errors = 0;
        std::uint64_t last = 0;
    } state;
    bs::mux::MuxerOptions opts;
    opts.fast_start = true;
    opts.expected_size = 1u << 16;
    opts.ctx = &state;
    opts.on_progress = [](void* p, std::uint64_t bytes, std::uint64_t) {
        auto* s = static_cast<State*>(p);
        ++s->progress;
        s->last = bytes;
    };
    opts.on_error = [](void* p, const char*) { ++static_cast<State*>(p)->errors; };

    auto m = bs::mux::Muxer::create(bs::mux::Container::Mp4, path);
    m.with_options(opts);
    CHECK(m.ok());
    auto t = m.add_track(avc_cfg(1));
    for (int i = 0; i < 8; ++i) {
        t.add_sample(kAvcNal, i * 1000, i * 1000, 1000, true,
                     bs::NalFramingMode::AnnexB);
    }
    CHECK(m.finalize());
    CHECK(state.progress >= 8);
    CHECK(state.errors == 0);
    CHECK(state.last > 0);

    // Verify the moov box is at the front (fast-start) and the file demuxes.
    std::ifstream in(path, std::ios::binary);
    std::vector<char> head(64);
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    const std::string s(head.data(), head.size());
    CHECK(s.find("ftyp") != std::string::npos);
    CHECK(s.find("moov") != std::string::npos);
    verify_mp4(path, 8);
    (void)std::filesystem::remove(path, ec);
}

void test_audio_track() {
    const std::string path = bs_test::temp_dir() + "/mux_streaming_audio.mp4";
    std::error_code ec;
    (void)std::filesystem::remove(path, ec);

    auto m = bs::mux::Muxer::create(bs::mux::Container::Mp4, path, 1u << 16);
    CHECK(m.ok());
    bs::mux::TrackConfig cfg;
    cfg.id = 1;
    cfg.is_audio = true;
    cfg.is_video = false;
    cfg.audio_codec = bs::mux::AudioCodec::Aac;
    cfg.sample_rate = 44100;
    cfg.channels = 2;
    auto t = m.add_track(cfg);
    for (int i = 0; i < 6; ++i) {
        const std::vector<std::uint8_t> frame{0x21, 0x10, 0x05, 0x00};
        t.add_sample(frame, i * 1024, i * 1024, 1024, true,
                     bs::NalFramingMode::RawFrame);
    }
    CHECK(m.finalize());

    auto d = bs::demux::Demuxer::init(path);
    CHECK(d.ok());
    if (d.ok()) {
        auto tracks = d.tracks();
        CHECK(!tracks.empty());
    }
    (void)std::filesystem::remove(path, ec);
}

void test_interleaved_av_from_corpus() {
    auto src = bs::demux::Demuxer::init(
        std::string("tests/fuzz/corpus/h264.mp4"));
    if (!src.ok()) {
        return;
    }

    const std::string path = bs_test::temp_dir() + "/mux_streaming_av.mp4";
    std::error_code ec;
    (void)std::filesystem::remove(path, ec);

    auto m = bs::mux::Muxer::create(bs::mux::Container::Mp4, path, 1u << 20);
    CHECK(m.ok());
    auto vt = m.add_track(avc_cfg(1));
    bs::mux::TrackConfig ac;
    ac.id = 2; ac.is_audio = true; ac.is_video = false;
    ac.audio_codec = bs::mux::AudioCodec::Aac;
    ac.sample_rate = 44100; ac.channels = 2; ac.timescale = 44100;
    auto at = m.add_track(ac);

    std::size_t video_count = 0, audio_count = 0;
    for (auto& t : src.tracks()) {
        if (!t.is_video) continue;
        std::size_t i = 0;
        while (t.hasNext()) {
            auto s = t.next();
            if (!s) break;
            vt.add_sample(s.data, s.pts, s.dts,
                          s.duration ? s.duration : 1000, s.is_key, s.framing);
            ++video_count;
            // Interleave one synthetic AAC frame per video sample.
            const std::vector<std::uint8_t> aac{0x21, 0x10, 0x04, 0x00};
            at.add_sample(aac, static_cast<std::int64_t>(i) * 1024,
                          static_cast<std::int64_t>(i) * 1024, 1024, true,
                          bs::NalFramingMode::RawFrame);
            ++audio_count;
            ++i;
        }
        break;
    }
    CHECK(m.finalize());

    auto d = bs::demux::Demuxer::init(path);
    CHECK(d.ok());
    if (d.ok()) {
        auto ts = d.tracks();
        CHECK(ts.size() == 2);
        if (ts.size() == 2) {
            bool saw_video = false, saw_audio = false;
            for (const auto& t : ts) {
                if (t.is_video) {
                    saw_video = true;
                    CHECK(t.sampleCount() > 0);
                }
                if (t.is_audio) {
                    saw_audio = true; // demux indexes audio metadata, not samples
                }
            }
            CHECK(saw_video);
            CHECK(saw_audio);
        }
    }
    (void)std::filesystem::remove(path, ec);
}

void test_other_containers_streaming() {
    {   // MKV (video)
        const std::string path = bs_test::temp_dir() + "/mux_stream_other.mkv";
        std::error_code ec;
        (void)std::filesystem::remove(path, ec);
        auto m = bs::mux::Muxer::create(bs::mux::Container::Mkv, path, 4096);
        CHECK(m.ok());
        auto t = m.add_track(avc_cfg(1));
        for (int i = 0; i < 10; ++i) {
            t.add_sample(kAvcNal, i * 100, i * 100, 100, i % 3 == 0,
                         bs::NalFramingMode::AnnexB);
        }
        CHECK(m.finalize());
        auto d = bs::demux::Demuxer::init(path);
        CHECK(d.ok());
        if (d.ok()) {
            auto ts = d.tracks();
            CHECK(ts.size() == 1);
            if (!ts.empty()) {
                CHECK(ts[0].codec == bs::Codec::Avc);
                // CodecPrivate carries SPS/PPS; the demuxer exposes those as
                // an extra synthetic parameter-set sample.
                CHECK(ts[0].sampleCount() == 10 || ts[0].sampleCount() == 11);
            }
        }
        (void)std::filesystem::remove(path, ec);
    }
    {   // IVF (VP9)
        const std::string path = bs_test::temp_dir() + "/mux_stream_other.ivf";
        std::error_code ec;
        (void)std::filesystem::remove(path, ec);
        auto m = bs::mux::Muxer::create(bs::mux::Container::Ivf, path, 4096);
        CHECK(m.ok());
        auto t = m.add_track([]{
            bs::mux::TrackConfig c;
            c.codec = bs::Codec::Vp9; c.width = 192; c.height = 108;
            return c;
        }());
        for (int i = 0; i < 10; ++i) {
            const std::vector<std::uint8_t> f{0x82, 0x49, 0x83, 0x42, 0x00};
            t.add_sample(f, i, i, 1, true, bs::NalFramingMode::RawFrame);
        }
        CHECK(m.finalize());
        auto d = bs::demux::Demuxer::init(path);
        CHECK(d.ok());
        if (d.ok()) {
            auto ts = d.tracks();
            CHECK(ts.size() == 1);
            if (!ts.empty()) {
                CHECK(ts[0].codec == bs::Codec::Vp9);
                CHECK(ts[0].sampleCount() == 10);
            }
        }
        // Frame count in the 32-byte header (offset 24, little-endian).
        std::ifstream in(path, std::ios::binary);
        std::vector<char> h(28);
        in.read(h.data(), static_cast<std::streamsize>(h.size()));
        const std::uint32_t count = static_cast<unsigned char>(h[24]) |
                                    (static_cast<std::uint32_t>(static_cast<unsigned char>(h[25])) << 8) |
                                    (static_cast<std::uint32_t>(static_cast<unsigned char>(h[26])) << 16);
        CHECK(count == 10u);
        (void)std::filesystem::remove(path, ec);
    }
    {   // WAV (PCM) — demuxer has no WAV reader, verify header bytes only.
        const std::string path = bs_test::temp_dir() + "/mux_stream_other.wav";
        std::error_code ec;
        (void)std::filesystem::remove(path, ec);
        auto m = bs::mux::Muxer::create(bs::mux::Container::Wav, path, 4096);
        CHECK(m.ok());
        auto t = m.add_track([]{
            bs::mux::TrackConfig c;
            c.is_audio = true; c.is_video = false;
            c.audio_codec = bs::mux::AudioCodec::Pcm16;
            c.sample_rate = 8000; c.channels = 1;
            return c;
        }());
        for (int i = 0; i < 10; ++i) {
            const std::vector<std::uint8_t> pcm{0x01, 0x02, 0x03, 0x04};
            t.add_sample(pcm, i * 4, i * 4, 4, true, bs::NalFramingMode::RawFrame);
        }
        CHECK(m.finalize());
        std::ifstream in(path, std::ios::binary);
        std::vector<char> h(44);
        in.read(h.data(), static_cast<std::streamsize>(h.size()));
        const std::string s(h.data(), h.size());
        CHECK(s.find("RIFF") != std::string::npos);
        CHECK(s.find("WAVE") != std::string::npos);
        CHECK(s.find("fmt ") != std::string::npos);
        CHECK(s.find("data") != std::string::npos);
        // RIFF size (offset 4) = 36 + 40 = 76; data size (offset 40) = 40.
        const auto le32 = [&](std::size_t at) {
            return static_cast<std::uint32_t>(static_cast<unsigned char>(h[at])) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(h[at + 1])) << 8) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(h[at + 2])) << 16);
        };
        CHECK(le32(40) == 40u);
        CHECK(le32(4) == 76u);
        (void)std::filesystem::remove(path, ec);
    }
}

void test_remux_from_corpus() {
    auto demuxer = bs::demux::Demuxer::init(
        std::string("tests/fuzz/corpus/h264.mp4"));
    if (!demuxer.ok()) {
        return; // corpus not present when running from a different cwd
    }
    auto tracks = demuxer.tracks();

    const std::string path = bs_test::temp_dir() + "/mux_streaming_remux.mp4";
    std::error_code ec;
    (void)std::filesystem::remove(path, ec);

    auto m = bs::mux::Muxer::create(bs::mux::Container::Mp4, path, 1u << 20);
    CHECK(m.ok());
    auto out = m.add_track(avc_cfg(1));
    std::size_t count = 0;
    for (auto& t : tracks) {
        if (!t.is_video) continue;
        while (t.hasNext()) {
            auto s = t.next();
            if (!s) break;
            out.add_sample(s.data, s.pts, s.dts,
                           s.duration ? s.duration : 1000, s.is_key, s.framing);
            ++count;
        }
        break;
    }
    CHECK(m.finalize());

    auto back = bs::demux::Demuxer::init(path);
    CHECK(back.ok());
    if (back.ok()) {
        auto bt = back.tracks();
        CHECK(!bt.empty());
        if (!bt.empty()) {
            // A leading parameter-set pseudo-sample is folded into the next
            // real access unit, so the count can be one lower than the source.
            CHECK(bt[0].sampleCount() == count || bt[0].sampleCount() == count - 1u);
        }
    }
    (void)std::filesystem::remove(path, ec);
}

void test_ordered_interleave() {
    const std::string path = bs_test::temp_dir() + "/mux_streaming_ordered.mp4";
    std::error_code ec;
    (void)std::filesystem::remove(path, ec);

    auto m = bs::mux::Muxer::create(bs::mux::Container::Mp4, path, 4096);
    bs::mux::MuxerOptions options;
    options.reorder_window = 8;
    m.with_options(options);
    auto video = m.add_track(avc_cfg(1));
    bs::mux::TrackConfig audio_cfg;
    audio_cfg.id = 2;
    audio_cfg.codec = bs::Codec::Aac;
    audio_cfg.is_video = false;
    audio_cfg.is_audio = true;
    audio_cfg.timescale = 1000;
    audio_cfg.sample_rate = 48000;
    audio_cfg.channels = 2;
    audio_cfg.framing = bs::NalFramingMode::RawFrame;
    auto audio = m.add_track(audio_cfg);
    const std::vector<std::uint8_t> aac{0x21, 0x10, 0x05, 0x60};

    // Deliberately out of decode order across the two tracks.
    video.add_sample(kAvcNal, 0, 0, 0, true, bs::NalFramingMode::AnnexB);
    video.add_sample(kAvcNal, 4000, 4000, 0, false, bs::NalFramingMode::AnnexB);
    audio.add_sample(aac, 1000, 1000, 0, true, bs::NalFramingMode::RawFrame);
    video.add_sample(kAvcNal, 2000, 2000, 0, false, bs::NalFramingMode::AnnexB);
    audio.add_sample(aac, 3000, 3000, 0, true, bs::NalFramingMode::RawFrame);
    CHECK(m.finalize());

    auto back = bs::demux::Demuxer::init(path);
    CHECK(back.ok());
    if (back.ok()) {
        auto tracks = back.tracks();
        CHECK(tracks.size() == 2);
        bool saw_video = false;
        bool saw_audio = false;
        for (auto& track : tracks) {
            std::int64_t previous_dts = -1;
            std::size_t samples = 0;
            while (track.hasNext()) {
                auto sample = track.next();
                if (!sample) break;
                CHECK(sample.dts >= previous_dts);
                previous_dts = sample.dts;
                ++samples;
            }
            if (track.is_video) {
                saw_video = true;
                CHECK(samples == 3);
            } else if (track.is_audio) {
                saw_audio = true;
                CHECK(samples == 2);
            }
        }
        CHECK(saw_video && saw_audio);
    }
    (void)std::filesystem::remove(path, ec);
}


struct TestBox {
    std::string type;
    std::size_t offset = 0;
    std::size_t size = 0;
    std::size_t header = 8;
};

std::uint32_t test_be32(const std::vector<std::uint8_t>& data, std::size_t pos) {
    return (static_cast<std::uint32_t>(data[pos]) << 24u) |
           (static_cast<std::uint32_t>(data[pos + 1u]) << 16u) |
           (static_cast<std::uint32_t>(data[pos + 2u]) << 8u) |
           static_cast<std::uint32_t>(data[pos + 3u]);
}

std::vector<TestBox> test_boxes(const std::vector<std::uint8_t>& data,
                                std::size_t start, std::size_t end) {
    std::vector<TestBox> out;
    std::size_t pos = start;
    while (pos + 8u <= end) {
        const std::uint32_t size32 = test_be32(data, pos);
        TestBox box;
        box.type.assign(reinterpret_cast<const char*>(data.data() + pos + 4u), 4u);
        box.offset = pos;
        box.header = 8u;
        box.size = size32;
        if (size32 == 1u) {
            if (pos + 16u > end) break;
            const std::uint64_t large =
                    (static_cast<std::uint64_t>(test_be32(data, pos + 8u)) << 32u) |
                    test_be32(data, pos + 12u);
            box.header = 16u;
            box.size = static_cast<std::size_t>(large);
        } else if (size32 == 0u) {
            box.size = end - pos;
        }
        if (box.size < box.header || pos + box.size > end) break;
        out.push_back(box);
        pos += box.size;
    }
    return out;
}

bool test_find_box(const std::vector<std::uint8_t>& data, std::size_t start,
                   std::size_t end, const std::string& type, TestBox& out) {
    for (const auto& box : test_boxes(data, start, end)) {
        if (box.type == type) {
            out = box;
            return true;
        }
    }
    return false;
}



void test_ordered_mixed_timescales() {
    const std::string path =
            bs_test::temp_dir() + "/mux_streaming_mixed_timescales.mp4";
    std::error_code ec;
    (void)std::filesystem::remove(path, ec);

    auto m = bs::mux::Muxer::create(bs::mux::Container::Mp4, path, 1u << 16);
    bs::mux::MuxerOptions options;
    options.reorder_window = 8;
    m.with_options(options);

    auto video_cfg = avc_cfg(1);
    video_cfg.timescale = 1000000;
    video_cfg.max_samples_per_chunk = 1;
    auto video = m.add_track(video_cfg);

    bs::mux::TrackConfig audio_cfg;
    audio_cfg.id = 2;
    audio_cfg.codec = bs::Codec::Aac;
    audio_cfg.is_video = false;
    audio_cfg.is_audio = true;
    audio_cfg.timescale = 48000;
    audio_cfg.sample_rate = 48000;
    audio_cfg.channels = 2;
    audio_cfg.framing = bs::NalFramingMode::RawFrame;
    audio_cfg.max_samples_per_chunk = 1;
    auto audio = m.add_track(audio_cfg);

    const std::vector<std::uint8_t> aac{0x21, 0x10, 0x05, 0x60};
    // Raw dts values are not comparable: video is in microseconds and audio
    // in 48 kHz samples.  Wall-clock order is V0, A0, V20ms, A21.3ms,
    // V40ms, A42.7ms.
    video.add_sample(kAvcNal, 0, 0, 0, true, bs::NalFramingMode::AnnexB);
    video.add_sample(kAvcNal, 40000, 40000, 0, false,
                     bs::NalFramingMode::AnnexB);
    audio.add_sample(aac, 0, 0, 1024, true, bs::NalFramingMode::RawFrame);
    audio.add_sample(aac, 1024, 1024, 1024, true,
                     bs::NalFramingMode::RawFrame);
    audio.add_sample(aac, 2048, 2048, 1024, true,
                     bs::NalFramingMode::RawFrame);
    video.add_sample(kAvcNal, 20000, 20000, 0, false,
                     bs::NalFramingMode::AnnexB);
    CHECK(m.finalize());

    std::ifstream in(path, std::ios::binary);
    const std::vector<std::uint8_t> data{
            std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
    CHECK(!data.empty());

    TestBox moov;
    CHECK(test_find_box(data, 0, data.size(), "moov", moov));
    std::vector<TestBox> traks;
    for (const auto& box : test_boxes(data, moov.offset + moov.header,
                                      moov.offset + moov.size)) {
        if (box.type == "trak") traks.push_back(box);
    }
    CHECK(traks.size() == 2u);

    const auto chunk_offsets = [&data](const TestBox& trak) {
        std::vector<std::uint32_t> offsets;
        TestBox mdia;
        TestBox minf;
        TestBox stbl;
        TestBox stco;
        if (!test_find_box(data, trak.offset + trak.header,
                           trak.offset + trak.size, "mdia", mdia) ||
            !test_find_box(data, mdia.offset + mdia.header,
                           mdia.offset + mdia.size, "minf", minf) ||
            !test_find_box(data, minf.offset + minf.header,
                           minf.offset + minf.size, "stbl", stbl) ||
            !test_find_box(data, stbl.offset + stbl.header,
                           stbl.offset + stbl.size, "stco", stco)) {
            return offsets;
        }
        const std::uint32_t count =
                test_be32(data, stco.offset + stco.header + 4u);
        for (std::uint32_t i = 0; i < count; ++i) {
            offsets.push_back(test_be32(
                    data, stco.offset + stco.header + 8u + i * 4u));
        }
        return offsets;
    };

    const auto video_offsets = chunk_offsets(traks[0]);
    const auto audio_offsets = chunk_offsets(traks[1]);
    CHECK(video_offsets.size() == 3u);
    CHECK(audio_offsets.size() == 3u);
    if (video_offsets.size() == 3u && audio_offsets.size() == 3u) {
        CHECK(video_offsets[0] < audio_offsets[0]);
        CHECK(audio_offsets[0] < video_offsets[1]);
        CHECK(video_offsets[1] < audio_offsets[1]);
        CHECK(audio_offsets[1] < video_offsets[2]);
        CHECK(video_offsets[2] < audio_offsets[2]);
    }

    (void)std::filesystem::remove(path, ec);
}

void test_mkv_cluster_duration_and_info() {
    const std::string path =
            bs_test::temp_dir() + "/mux_streaming_mkv_clusters.mkv";
    std::error_code ec;
    (void)std::filesystem::remove(path, ec);

    auto m = bs::mux::Muxer::create(bs::mux::Container::Mkv, path, 1u << 16);
    bs::mux::MuxerOptions options;
    options.cluster_duration_ms = 3000;
    m.with_options(options);
    auto t = m.add_track(avc_cfg(1));
    for (int i = 0; i <= 9; ++i) {
        const std::int64_t pts = static_cast<std::int64_t>(i) * 1000;
        t.add_sample(kAvcNal, pts, pts, 1000, true,
                     bs::NalFramingMode::AnnexB);
    }
    CHECK(m.finalize());

    std::ifstream in(path, std::ios::binary);
    const std::vector<std::uint8_t> data{
            std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
    CHECK(!data.empty());

    // One Cluster ID at 0 ms and one at each 3 s keyframe: 0/3000/6000/9000.
    std::size_t clusters = 0;
    for (std::size_t i = 0; i + 4u <= data.size(); ++i) {
        if (data[i] == 0x1Fu && data[i + 1u] == 0x43u &&
            data[i + 2u] == 0xB6u && data[i + 3u] == 0x75u) {
            ++clusters;
        }
    }
    CHECK(clusters == 4u);

    // Info/Duration: ID 0x4489, size 0x88 (8 bytes), big-endian float64 ms.
    std::size_t duration_at = data.size();
    for (std::size_t i = 0; i + 11u <= data.size(); ++i) {
        if (data[i] == 0x44u && data[i + 1u] == 0x89u &&
            data[i + 2u] == 0x88u) {
            duration_at = i + 3u;
            break;
        }
    }
    CHECK(duration_at + 8u <= data.size());
    if (duration_at + 8u <= data.size()) {
        std::uint64_t bits = 0;
        for (unsigned i = 0; i < 8u; ++i) {
            bits = (bits << 8u) | data[duration_at + i];
        }
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        CHECK(value == 10000.0);
    }

    (void)std::filesystem::remove(path, ec);
}

void test_audio_single_sample_chunks() {
    const std::string path =
            bs_test::temp_dir() + "/mux_streaming_audio_chunks.mp4";
    std::error_code ec;
    (void)std::filesystem::remove(path, ec);

    auto m = bs::mux::Muxer::create(bs::mux::Container::Mp4, path, 4096);
    auto video = m.add_track(avc_cfg(1));

    bs::mux::TrackConfig audio_cfg;
    audio_cfg.id = 2;
    audio_cfg.codec = bs::Codec::Aac;
    audio_cfg.is_video = false;
    audio_cfg.is_audio = true;
    audio_cfg.timescale = 48000;
    audio_cfg.sample_rate = 48000;
    audio_cfg.channels = 2;
    audio_cfg.framing = bs::NalFramingMode::RawFrame;
    // Force one audio sample per chunk; without this the two consecutive
    // audio samples below would share a chunk and stsc would carry a run.
    audio_cfg.max_samples_per_chunk = 1;
    auto audio = m.add_track(audio_cfg);

    const std::vector<std::uint8_t> aac{0x21, 0x10, 0x05, 0x60};
    video.add_sample(kAvcNal, 0, 0, 0, true, bs::NalFramingMode::AnnexB);
    audio.add_sample(aac, 0, 0, 1024, true, bs::NalFramingMode::RawFrame);
    audio.add_sample(aac, 1024, 1024, 1024, true, bs::NalFramingMode::RawFrame);
    video.add_sample(kAvcNal, 40000, 40000, 0, false, bs::NalFramingMode::AnnexB);
    CHECK(m.finalize());

    std::ifstream in(path, std::ios::binary);
    const std::vector<std::uint8_t> data{
            std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
    CHECK(!data.empty());

    TestBox moov;
    CHECK(test_find_box(data, 0, data.size(), "moov", moov));
    const auto top = test_boxes(data, moov.offset + moov.header,
                                moov.offset + moov.size);
    std::vector<TestBox> traks;
    for (const auto& box : top) {
        if (box.type == "trak") traks.push_back(box);
    }
    CHECK(traks.size() == 2u);

    TestBox mdia;
    TestBox minf;
    TestBox stbl;
    TestBox stsc;
    TestBox stco;
    const auto& audio_trak = traks[1];
    CHECK(test_find_box(data, audio_trak.offset + audio_trak.header,
                        audio_trak.offset + audio_trak.size, "mdia", mdia));
    CHECK(test_find_box(data, mdia.offset + mdia.header,
                        mdia.offset + mdia.size, "minf", minf));
    CHECK(test_find_box(data, minf.offset + minf.header,
                        minf.offset + minf.size, "stbl", stbl));
    CHECK(test_find_box(data, stbl.offset + stbl.header,
                        stbl.offset + stbl.size, "stsc", stsc));
    CHECK(test_find_box(data, stbl.offset + stbl.header,
                        stbl.offset + stbl.size, "stco", stco));

    const std::uint32_t stsc_count =
            test_be32(data, stsc.offset + stsc.header + 4u);
    const std::uint32_t samples_per_chunk =
            test_be32(data, stsc.offset + stsc.header + 12u);
    const std::uint32_t stco_count =
            test_be32(data, stco.offset + stco.header + 4u);
    CHECK(stsc_count == 1u);
    CHECK(samples_per_chunk == 1u);
    CHECK(stco_count == 2u);

    (void)std::filesystem::remove(path, ec);
}

void test_derived_durations() {
    const std::string path = bs_test::temp_dir() + "/mux_streaming_durations.mp4";
    std::error_code ec;
    (void)std::filesystem::remove(path, ec);

    bs::mux::TrackConfig cfg = avc_cfg(1);
    cfg.timescale = 1000000;
    auto m = bs::mux::Muxer::create(bs::mux::Container::Mp4, path, 4096);
    auto track = m.add_track(cfg);
    for (int i = 0; i < 3; ++i) {
        const std::int64_t pts = static_cast<std::int64_t>(i) * 40000;
        track.add_sample(kAvcNal, pts, pts, 0, i == 0, bs::NalFramingMode::AnnexB);
    }
    CHECK(m.finalize());

    auto back = bs::demux::Demuxer::init(path);
    CHECK(back.ok());
    if (back.ok()) {
        auto tracks = back.tracks();
        CHECK(tracks.size() == 1);
        if (!tracks.empty()) {
            std::size_t index = 0;
            while (tracks[0].hasNext()) {
                auto sample = tracks[0].next();
                if (!sample) break;
                CHECK(sample.pts == static_cast<std::int64_t>(index) * 40000);
                CHECK(sample.duration == 40000);
                ++index;
            }
            CHECK(index == 3);
        }
    }
    (void)std::filesystem::remove(path, ec);
}

}  // namespace

int main() {
    test_mapped_stream();
    test_buffered_fallback();
    test_growth();
    test_ordered_interleave();
    test_audio_single_sample_chunks();
    test_ordered_mixed_timescales();
    test_mkv_cluster_duration_and_info();
    test_derived_durations();
    test_interleaved();
    test_fast_start_and_callbacks();
    test_audio_track();
    test_interleaved_av_from_corpus();
    test_other_containers_streaming();
    test_remux_from_corpus();

    if (g_failures != 0) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "mux streaming: all checks passed\n";
    return 0;
}
