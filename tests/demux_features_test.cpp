// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * Feature tests for the demuxer's audio/subtitle model, timing, keyframe
 * seek and interleaved reader.  Uses real corpus samples where available.
 */
#include <demux/demuxer.hpp>
#include "test_temp.hpp"

#include <cstdint>
#include <cstdio>
#include <iostream>
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

const char* corpus(const char* name) {
    static std::string base = "tests/fuzz/corpus/";
    static std::string real = "tests/real/";
    static std::string p;
    if (std::string(name).rfind("real/", 0) == 0) {
        p = real + std::string(name).substr(5);
    } else {
        p = base + name;
    }
    return p.c_str();
}

void test_mp4_audio() {
    auto d = bs::demux::Demuxer::init(corpus("real/avc_mp4_envivio.mp4"));
    if (!d.ok()) {
        return; // sample not present
    }
    auto tracks = d.tracks();
    CHECK(tracks.size() == 2);
    bool video = false, audio = false;
    for (const auto& t : tracks) {
        if (t.is_video) {
            video = true;
            CHECK(t.codec == bs::Codec::Avc);
            CHECK(t.sampleCount() > 0);
        }
        if (t.is_audio) {
            audio = true;
            CHECK(t.audio_codec == bs::demux::AudioCodec::Aac);
            CHECK(t.sample_rate == 22050);
            CHECK(t.channels == 2);
            CHECK(t.bits_per_sample == 16);
            CHECK(!t.audio_config.empty());
            CHECK(t.sampleCount() > 0);
        }
    }
    CHECK(video);
    CHECK(audio);

    // Interleaved reader walks both tracks in DTS order.
    auto reader = d.interleaved();
    std::size_t count = 0;
    std::int64_t last = -1;
    bool sorted = true;
    while (reader.hasNext()) {
        auto s = reader.next();
        if (!s) break;
        if (s.dts < last) sorted = false;
        last = s.dts;
        ++count;
    }
    CHECK(sorted);
    CHECK(count > 0);
}

void test_mkv_audio() {
    auto d = bs::demux::Demuxer::init(corpus("real/webm_vp8.webm"));
    if (!d.ok()) {
        return;
    }
    auto tracks = d.tracks();
    bool audio = false;
    for (const auto& t : tracks) {
        if (t.is_audio) {
            audio = true;
            CHECK(t.audio_codec == bs::demux::AudioCodec::Vorbis);
            CHECK(t.sample_rate == 44100);
            CHECK(t.channels == 2);
            CHECK(t.sampleCount() > 0);
            CHECK(t.timescale > 0);
        }
    }
    CHECK(audio);
}

void test_ts_audio() {
    auto d = bs::demux::Demuxer::init(corpus("multi.ts"));
    if (!d.ok()) {
        return;
    }
    bool audio = false;
    for (const auto& t : d.tracks()) {
        if (t.is_audio) {
            audio = true;
            CHECK(t.audio_codec == bs::demux::AudioCodec::Aac);
            CHECK(t.sampleCount() > 0);
        }
    }
    CHECK(audio);
}

void test_keyframe_seek() {
    auto d = bs::demux::Demuxer::init(corpus("h264.mp4"));
    if (!d.ok()) {
        return;
    }
    auto t = d.track(0);
    CHECK(t.sampleCount() > 0);
    // Seeking to a late time should land on a keyframe.
    std::size_t total = t.sampleCount();
    auto target_pts = static_cast<std::int64_t>(total / 2) * 1000;
    CHECK(t.seek_time_keyframe(target_pts));
    auto s = t.next();
    CHECK(s);
    if (s) {
        CHECK(s.is_key);
        CHECK(s.pts <= target_pts);
    }
}

/* Phase 2 regression tests with synthetic containers. */

void put16(std::vector<std::uint8_t>& d, std::uint16_t v) {
    d.push_back(static_cast<std::uint8_t>(v));
    d.push_back(static_cast<std::uint8_t>(v >> 8));
}

void put32(std::vector<std::uint8_t>& d, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) {
        d.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

std::vector<std::int64_t> collect_pts(bs::demux::Track& t) {
    std::vector<std::int64_t> pts;
    std::vector<std::int64_t> dur;
    while (t.hasNext()) {
        auto s = t.next();
        if (!s) break;
        pts.push_back(s.pts);
        dur.push_back(s.duration);
    }
    return pts;
}

void test_wav_timing_regression() {
    const std::string path = bs_test::temp_path("p2_wav", ".wav");
    std::vector<std::uint8_t> d;
    const auto tag = [&](const char* s) { for (int i = 0; i < 4; ++i) d.push_back(s[i]); };
    tag("RIFF"); put32(d, 36 + 8); tag("WAVE");
    tag("fmt "); put32(d, 16); put16(d, 1); put16(d, 1);
    put32(d, 8000); put32(d, 16000); put16(d, 2); put16(d, 16);
    tag("data"); put32(d, 8);
    for (int i = 0; i < 8; ++i) d.push_back(0);
    CHECK(bs_test::write_bytes(path, d));

    auto demux = bs::demux::Demuxer::init(path);
    CHECK(demux.ok());
    if (demux.ok()) {
        auto t = demux.track(0);
        CHECK(t.sampleCount() == 4);
        const auto pts = collect_pts(t);
        CHECK(pts.size() == 4);
        for (std::size_t i = 0; i < pts.size(); ++i) {
            CHECK(pts[i] == static_cast<std::int64_t>(i));
        }
    }
    bs_test::remove_file(path);
}

void ogg_page(
    std::vector<std::uint8_t>& d, std::uint8_t type, std::uint64_t granule,
    std::uint32_t serial, std::uint32_t seq, const std::vector<std::uint8_t>& packet
) {
    const auto begin = d.size();
    const auto put = [&](const char* s) { for (int i = 0; i < 4; ++i) d.push_back(s[i]); };
    put("OggS");
    d.push_back(0); d.push_back(type);
    for (unsigned i = 0; i < 8; ++i) d.push_back(static_cast<std::uint8_t>(granule >> (8 * i)));
    put32(d, serial); put32(d, seq); put32(d, 0); // CRC (not validated)
    d.push_back(1); // one segment
    d.push_back(static_cast<std::uint8_t>(packet.size()));
    d.insert(d.end(), packet.begin(), packet.end());
    // Patch the CRC field so the page is structurally complete.
    d[begin + 22] = 0; d[begin + 23] = 0; d[begin + 24] = 0; d[begin + 25] = 0;
}

void test_ogg_granule_regression() {
    const std::string path = bs_test::temp_path("p2", ".ogg");
    std::vector<std::uint8_t> d;
    std::vector<std::uint8_t> ident{0x01};
    const char vorbis[] = "vorbis";
    for (int i = 0; i < 6; ++i) ident.push_back(vorbis[i]);
    for (int i = 0; i < 4; ++i) ident.push_back(0);
    ident.push_back(2); // channels
    for (unsigned i = 0; i < 4; ++i) ident.push_back(static_cast<std::uint8_t>(44100u >> (8 * i)));
    while (ident.size() < 30) ident.push_back(0);

    ogg_page(d, 0x02, 0, 1, 0, ident);         // BOS identification header
    ogg_page(d, 0x00, 0, 1, 1, {0x03, 'v', 'o', 'r', 'b', 'i', 's'}); // comment
    ogg_page(d, 0x00, 0, 1, 2, {0x05, 's', 'e', 't', 'u', 'p'});      // setup
    ogg_page(d, 0x00, 1024, 1, 3, {1, 2, 3, 4, 5});
    ogg_page(d, 0x00, 2048, 1, 4, {6, 7, 8, 9, 10});
    CHECK(bs_test::write_bytes(path, d));

    auto demux = bs::demux::Demuxer::init(path);
    CHECK(demux.ok());
    if (demux.ok()) {
        auto t = demux.track(0);
        CHECK(t.codec == bs::Codec::Vorbis);
        CHECK(t.sample_rate == 44100);
        const auto pts = collect_pts(t);
        CHECK(pts.size() == 2);
        if (pts.size() >= 2) {
            CHECK(pts[0] == 1024);
            CHECK(pts[1] == 2048);
        }
    }
    bs_test::remove_file(path);
}

void test_flv_mixed_codec_regression() {
    const std::string path = bs_test::temp_path("p2", ".flv");
    std::vector<std::uint8_t> d;
    d.push_back('F'); d.push_back('L'); d.push_back('V');
    d.push_back(1); d.push_back(5);
    // FLV header size is a 32-bit BIG-endian field at byte 5.
    d.push_back(0); d.push_back(0); d.push_back(0); d.push_back(9);
    put32(d, 0); // PreviousTagSize0

    const auto tag = [&](std::uint8_t type, std::uint32_t ts, const std::vector<std::uint8_t>& body) {
        d.push_back(type);
        d.push_back(static_cast<std::uint8_t>(body.size() >> 16));
        d.push_back(static_cast<std::uint8_t>(body.size() >> 8));
        d.push_back(static_cast<std::uint8_t>(body.size()));
        d.push_back(static_cast<std::uint8_t>(ts >> 16));
        d.push_back(static_cast<std::uint8_t>(ts >> 8));
        d.push_back(static_cast<std::uint8_t>(ts));
        d.push_back(0); // timestamp extension
        d.push_back(0); d.push_back(0); d.push_back(0); // stream id (3 bytes)
        d.insert(d.end(), body.begin(), body.end());
        put32(d, static_cast<std::uint32_t>(11 + body.size()));
    };

    // AVC: keyframe + codec 7, AVC config packet.
    std::vector<std::uint8_t> avc_cfg{0x17, 0x00, 0, 0, 0, 1, 0x67, 0x42, 0, 0x1e};
    tag(9, 0, avc_cfg);
    // AVC: inter frame with a composition offset.
    tag(9, 40, {0x27, 0x01, 0, 0, 10, 0x65, 0x88, 0x84, 0, 0, 0, 0, 0, 0, 0});
    // VP9: independent frame -> must become a second track.
    tag(9, 80, {0x1B, 0x82, 0x49, 0x83, 0x42}); // keyframe + VP9 codec id 11

    CHECK(bs_test::write_bytes(path, d));
    auto demux = bs::demux::Demuxer::init(path);
    CHECK(demux.ok());
    if (demux.ok()) {
        CHECK(demux.tracks().size() == 2);
        if (demux.tracks().size() >= 2) {
            CHECK(demux.track(0).codec == bs::Codec::Avc);
            CHECK(demux.track(1).codec == bs::Codec::Vp9);
        }
    }
    bs_test::remove_file(path);
}

}  // namespace

int main() {
    test_mp4_audio();
    test_mkv_audio();
    test_ts_audio();
    test_keyframe_seek();
    test_wav_timing_regression();
    test_ogg_granule_regression();
    test_flv_mixed_codec_regression();
    if (g_failures != 0) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "demux features: all checks passed\n";
    return 0;
}
