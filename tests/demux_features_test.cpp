// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * Feature tests for the demuxer's audio/subtitle model, timing, keyframe
 * seek and interleaved reader.  Uses real corpus samples where available.
 */
#include <demux/demuxer.hpp>

#include <cstdint>
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

}  // namespace

int main() {
    test_mp4_audio();
    test_mkv_audio();
    test_ts_audio();
    test_keyframe_seek();
    if (g_failures != 0) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "demux features: all checks passed\n";
    return 0;
}
