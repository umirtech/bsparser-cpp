// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * Streaming muxer tests: path/fd sinks write through OS-backed pages with
 * bounded memory, support caller-driven interleaving, and can force the
 * buffered fallback.  Output is verified by demuxing it back.
 */
#include <mux/muxer.hpp>
#include <demux/demuxer.hpp>

#include <cstdint>
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

const std::vector<std::uint8_t> kAvcNal{
    0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1E, 0xAA, 0xBB
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
    const std::string path = "/tmp/opencode/mux_streaming_mapped.mp4";
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
    const std::string path = "/tmp/opencode/mux_streaming_buffered.mp4";
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
    const std::string path = "/tmp/opencode/mux_streaming_growth.mp4";
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
    const std::string path = "/tmp/opencode/mux_streaming_interleaved.mp4";
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
    const std::string path = "/tmp/opencode/mux_streaming_faststart.mp4";
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
    const std::string path = "/tmp/opencode/mux_streaming_audio.mp4";
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

void test_remux_from_corpus() {
    auto demuxer = bs::demux::Demuxer::init(
        std::string("tests/fuzz/corpus/h264.mp4"));
    if (!demuxer.ok()) {
        return; // corpus not present when running from a different cwd
    }
    auto tracks = demuxer.tracks();

    const std::string path = "/tmp/opencode/mux_streaming_remux.mp4";
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
            CHECK(bt[0].sampleCount() == count);
        }
    }
    (void)std::filesystem::remove(path, ec);
}

}  // namespace

int main() {
    test_mapped_stream();
    test_buffered_fallback();
    test_growth();
    test_interleaved();
    test_fast_start_and_callbacks();
    test_audio_track();
    test_remux_from_corpus();

    if (g_failures != 0) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "mux streaming: all checks passed\n";
    return 0;
}
