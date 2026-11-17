// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * WAV muxer tests: verify the RIFF/WAVE header, fmt and data chunks,
 * validation of audio parameters, and extension-based container selection.
 */
#include <mux/muxer.hpp>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
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

std::uint16_t rd16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8u));
}

std::uint32_t rd32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8u) |
           (static_cast<std::uint32_t>(p[2]) << 16u) | (static_cast<std::uint32_t>(p[3]) << 24u);
}

bool tag(std::span<const std::uint8_t> d, std::size_t at, std::string_view t) {
    return at + t.size() <= d.size() && std::memcmp(d.data() + at, t.data(), t.size()) == 0;
}

void test_basic() {
    auto muxer = bs::mux::Muxer::create(bs::mux::Container::Wav);
    CHECK(muxer.ok());

    bs::mux::TrackConfig cfg;
    cfg.audio_codec = bs::mux::AudioCodec::Pcm16;
    cfg.sample_rate = 44100;
    cfg.channels = 2;
    auto track = muxer.add_track(cfg);

    // Four 16-bit stereo frames = 16 bytes.
    const std::vector<std::uint8_t> pcm{
        0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00,
        0x04, 0x00, 0x05, 0x00, 0x06, 0x00, 0x07, 0x00
    };
    track.add_sample(pcm, 0, 0, 0, true, bs::NalFramingMode::RawFrame);
    CHECK(muxer.finalize());

    const auto d = muxer.data();
    CHECK(d.size() == 60u); // 44-byte header + 16 bytes of samples
    if (d.size() < 44u) {
        return;
    }

    CHECK(tag(d, 0, "RIFF"));
    CHECK(rd32(d.data() + 4) == 52u); // 36 + data size
    CHECK(tag(d, 8, "WAVE"));

    CHECK(tag(d, 12, "fmt "));
    CHECK(rd32(d.data() + 16) == 16u);
    CHECK(rd16(d.data() + 20) == 1u);              // PCM
    CHECK(rd16(d.data() + 22) == 2u);              // channels
    CHECK(rd32(d.data() + 24) == 44100u);          // sample rate
    CHECK(rd32(d.data() + 28) == 44100u * 4u);     // byte rate
    CHECK(rd16(d.data() + 32) == 4u);              // block align
    CHECK(rd16(d.data() + 34) == 16u);             // bits per sample

    CHECK(tag(d, 36, "data"));
    CHECK(rd32(d.data() + 40) == 16u);
    CHECK(std::memcmp(d.data() + 44, pcm.data(), pcm.size()) == 0);
}

void test_formats() {
    struct Case {
        bs::mux::AudioCodec codec;
        std::uint16_t bits;
        std::uint16_t align;
    };
    const Case cases[] = {
        {bs::mux::AudioCodec::Pcm8, 8u, 1u},
        {bs::mux::AudioCodec::Pcm16, 16u, 2u},
        {bs::mux::AudioCodec::Pcm24, 24u, 3u},
        {bs::mux::AudioCodec::Pcm32, 32u, 4u},
    };
    for (const auto& c : cases) {
        auto m = bs::mux::Muxer::create(bs::mux::Container::Wav);
        bs::mux::TrackConfig cfg;
        cfg.audio_codec = c.codec;
        cfg.sample_rate = 8000;
        cfg.channels = 1;
        auto t = m.add_track(cfg);
        const std::vector<std::uint8_t> pcm(c.align * 2u, 0x11);
        t.add_sample(pcm, 0, 0, 0, true, bs::NalFramingMode::RawFrame);
        CHECK(m.finalize());
        const auto d = m.data();
        CHECK(d.size() >= 44u);
        if (d.size() >= 44u) {
            CHECK(rd16(d.data() + 34) == c.bits);
            CHECK(rd16(d.data() + 32) == c.align);
            CHECK(rd32(d.data() + 4) == 36u + c.align * 2u);
        }
    }
}

void test_validation() {
    {
        auto m = bs::mux::Muxer::create(bs::mux::Container::Wav);
        bs::mux::TrackConfig cfg;
        cfg.sample_rate = 0;
        m.add_track(cfg);
        CHECK(!m.finalize());
    }
    {
        auto m = bs::mux::Muxer::create(bs::mux::Container::Wav);
        bs::mux::TrackConfig cfg;
        cfg.channels = 0;
        m.add_track(cfg);
        CHECK(!m.finalize());
    }
    {
        auto m = bs::mux::Muxer::create(bs::mux::Container::Wav);
        bs::mux::TrackConfig cfg;
        cfg.audio_codec = bs::mux::AudioCodec::Pcm16;
        cfg.channels = 2;
        auto t = m.add_track(cfg);
        const std::vector<std::uint8_t> odd{0x01, 0x02, 0x03}; // not a multiple of 4
        t.add_sample(odd, 0, 0, 0, true, bs::NalFramingMode::RawFrame);
        CHECK(!m.finalize());
    }
    {
        auto m = bs::mux::Muxer::create(bs::mux::Container::Wav);
        CHECK(!m.finalize()); // no tracks
    }
}

void test_extension_and_conversion() {
    auto m = bs::mux::Muxer::open("out.wav");
    CHECK(m.ok());
    CHECK(m.container() == bs::mux::Container::Wav);
    CHECK(bs::mux::to_demux(bs::mux::Container::Wav) == bs::demux::Container::Unknown);
}

} // namespace

int main() {
    test_basic();
    test_formats();
    test_validation();
    test_extension_and_conversion();

    if (g_failures != 0) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "wav muxer: all checks passed\n";
    return 0;
}
