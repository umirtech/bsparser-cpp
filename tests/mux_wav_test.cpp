// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * WAV muxer tests: verify the RIFF/WAVE header, fmt and data chunks,
 * validation of audio parameters, and extension-based container selection.
 *
 * The muxer is streaming-only: output is written to a temporary file path and
 * read back from disk for inspection.
 */
#include <mux/muxer.hpp>
#include "test_temp.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
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

std::vector<std::uint8_t> read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

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
    const std::string path = bs_test::temp_dir() + "/mux_wav_basic.wav";
    std::error_code ec;
    (void)std::filesystem::remove(path, ec);
    auto muxer = bs::mux::Muxer::create(bs::mux::Container::Wav, path);
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

    const auto d = read_file(path);
    CHECK(d.size() == 60u); // 44-byte header + 16 bytes of samples
    if (d.size() < 44u) {
        (void)std::filesystem::remove(path, ec);
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
    (void)std::filesystem::remove(path, ec);
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
    int idx = 0;
    for (const auto& c : cases) {
        const std::string path = bs_test::temp_dir() + "/mux_wav_fmt_" +
                                 std::to_string(idx++) + ".wav";
        std::error_code ec;
        (void)std::filesystem::remove(path, ec);
        auto m = bs::mux::Muxer::create(bs::mux::Container::Wav, path);
        bs::mux::TrackConfig cfg;
        cfg.audio_codec = c.codec;
        cfg.sample_rate = 8000;
        cfg.channels = 1;
        auto t = m.add_track(cfg);
        const std::vector<std::uint8_t> pcm(c.align * 2u, 0x11);
        t.add_sample(pcm, 0, 0, 0, true, bs::NalFramingMode::RawFrame);
        CHECK(m.finalize());
        const auto d = read_file(path);
        CHECK(d.size() >= 44u);
        if (d.size() >= 44u) {
            CHECK(rd16(d.data() + 34) == c.bits);
            CHECK(rd16(d.data() + 32) == c.align);
            CHECK(rd32(d.data() + 4) == 36u + c.align * 2u);
        }
        (void)std::filesystem::remove(path, ec);
    }
}

void test_validation() {
    std::error_code ec;
    int idx = 0;
    const auto make_path = [&idx]() {
        return bs_test::temp_dir() + "/mux_wav_val_" + std::to_string(idx++) + ".wav";
    };
    {
        const std::string path = make_path();
        (void)std::filesystem::remove(path, ec);
        auto m = bs::mux::Muxer::create(bs::mux::Container::Wav, path);
        bs::mux::TrackConfig cfg;
        cfg.sample_rate = 0;
        m.add_track(cfg);
        CHECK(!m.finalize());
        (void)std::filesystem::remove(path, ec);
    }
    {
        const std::string path = make_path();
        (void)std::filesystem::remove(path, ec);
        auto m = bs::mux::Muxer::create(bs::mux::Container::Wav, path);
        bs::mux::TrackConfig cfg;
        cfg.channels = 0;
        m.add_track(cfg);
        CHECK(!m.finalize());
        (void)std::filesystem::remove(path, ec);
    }
    {
        const std::string path = make_path();
        (void)std::filesystem::remove(path, ec);
        auto m = bs::mux::Muxer::create(bs::mux::Container::Wav, path);
        bs::mux::TrackConfig cfg;
        cfg.audio_codec = bs::mux::AudioCodec::Pcm16;
        cfg.channels = 2;
        auto t = m.add_track(cfg);
        const std::vector<std::uint8_t> odd{0x01, 0x02, 0x03}; // not a multiple of 4
        t.add_sample(odd, 0, 0, 0, true, bs::NalFramingMode::RawFrame);
        // The streaming writer rejects a sample that is not a whole number of
        // frames by dropping it, so the finalized file has an empty data
        // chunk: the malformed payload must not reach the output.
        CHECK(m.finalize());
        const auto d = read_file(path);
        CHECK(d.size() == 44u);
        if (d.size() >= 44u) {
            CHECK(rd32(d.data() + 40) == 0u);
        }
        (void)std::filesystem::remove(path, ec);
    }
    {
        const std::string path = make_path();
        (void)std::filesystem::remove(path, ec);
        auto m = bs::mux::Muxer::create(bs::mux::Container::Wav, path);
        CHECK(!m.finalize()); // no tracks
        (void)std::filesystem::remove(path, ec);
    }
}

void test_extension_and_conversion() {
    const std::string path = bs_test::temp_dir() + "/mux_wav_ext.wav";
    std::error_code ec;
    (void)std::filesystem::remove(path, ec);
    auto m = bs::mux::Muxer::open(path);
    CHECK(m.ok());
    CHECK(m.container() == bs::mux::Container::Wav);
    CHECK(bs::mux::to_demux(bs::mux::Container::Wav) == bs::demux::Container::Unknown);
    (void)std::filesystem::remove(path, ec);
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
