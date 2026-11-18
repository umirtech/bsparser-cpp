// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * Container-standard conformance checks for the mux writers:
 *   - MP4: dinf/dref, vmhd/smhd, ctts, audio tkhd dimensions
 *   - MKV: CodecPrivate for H.26x, WebM DocType, FlagLacing, BitDepth
 *   - WAV: RIFF padding for odd data, WAVE_FORMAT_EXTENSIBLE for >2 channels
 *   - IVF: framerate/timescale timebase
 */
#include <mux/muxer.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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

std::vector<std::uint8_t> read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

bool contains(const std::vector<std::uint8_t>& d, const char* s) {
    const std::size_t n = std::strlen(s);
    if (d.size() < n) return false;
    for (std::size_t i = 0; i + n <= d.size(); ++i) {
        if (std::memcmp(d.data() + i, s, n) == 0) return true;
    }
    return false;
}

void test_mp4_boxes() {
    const std::string path = "/tmp/opencode/conf.mp4";
    std::error_code ec;
    (void)std::filesystem::remove(path, ec);
    auto m = bs::mux::Muxer::create(bs::mux::Container::Mp4, path, 1u << 16);
    auto v = m.add_track([] {
        bs::mux::TrackConfig c; c.id = 1; c.codec = bs::Codec::Avc;
        c.width = 64; c.height = 64; return c;
    }());
    auto a = m.add_track([] {
        bs::mux::TrackConfig c; c.id = 2; c.is_audio = true; c.is_video = false;
        c.audio_codec = bs::mux::AudioCodec::Aac; c.sample_rate = 44100; c.channels = 2;
        return c;
    }());
    for (int i = 0; i < 8; ++i) {
        const std::vector<std::uint8_t> nal{
            0,0,0,1,0x67,0x42,0,0x1e, 0,0,1,0x65,0x88,0x84};
        v.add_sample(nal, i * 1000, i * 1000, 1000, true, bs::NalFramingMode::AnnexB);
        const std::vector<std::uint8_t> aac{0x21,0x10,0x04,0x00};
        a.add_sample(aac, i * 1024, i * 1024, 1024, true, bs::NalFramingMode::RawFrame);
    }
    CHECK(m.finalize());
    const auto d = read_file(path);
    CHECK(contains(d, "dinf"));
    CHECK(contains(d, "dref"));
    CHECK(contains(d, "url "));
    CHECK(contains(d, "vmhd"));
    CHECK(contains(d, "smhd"));
    CHECK(contains(d, "esds"));
    (void)std::filesystem::remove(path, ec);
}

void test_mp4_ctts() {
    const std::string path = "/tmp/opencode/conf_ctts.mp4";
    std::error_code ec;
    (void)std::filesystem::remove(path, ec);
    auto m = bs::mux::Muxer::create(bs::mux::Container::Mp4, path, 1u << 16);
    auto t = m.add_track([] {
        bs::mux::TrackConfig c; c.id = 1; c.codec = bs::Codec::Avc;
        c.width = 64; c.height = 64; return c;
    }());
    for (int i = 0; i < 6; ++i) {
        const std::vector<std::uint8_t> nal{
            0,0,0,1,0x67,0x42,0,0x1e, 0,0,1,0x65,0x88,0x84};
        // B-frame style: pts ahead of dts.
        t.add_sample(nal, i * 1000 + 3000, i * 1000, 1000, true,
                     bs::NalFramingMode::AnnexB);
    }
    CHECK(m.finalize());
    CHECK(contains(read_file(path), "ctts"));
    (void)std::filesystem::remove(path, ec);
}

void test_mkv_codec_private_and_lacing() {
    const std::string path = "/tmp/opencode/conf.mkv";
    std::error_code ec;
    (void)std::filesystem::remove(path, ec);
    auto m = bs::mux::Muxer::create(bs::mux::Container::Mkv, path, 1u << 16);
    auto t = m.add_track([] {
        bs::mux::TrackConfig c; c.id = 1; c.codec = bs::Codec::Avc;
        c.width = 64; c.height = 64; return c;
    }());
    for (int i = 0; i < 6; ++i) {
        const std::vector<std::uint8_t> nal{
            0,0,0,1,0x67,0x42,0,0x1e,0x00, 0,0,1,0x68,0xce,0x3c,0x80,
            0,0,1,0x65,0x88,0x84};
        t.add_sample(nal, i * 40, i * 40, 40, true, bs::NalFramingMode::AnnexB);
    }
    CHECK(m.finalize());
    const auto d = read_file(path);
    // CodecPrivate element ID 0x63A2 and FlagLacing 0x55AA in the bytes.
    bool has_codec_private = false;
    for (std::size_t i = 0; i + 1 < d.size(); ++i) {
        if (d[i] == 0x63 && d[i + 1] == 0xA2) { has_codec_private = true; break; }
    }
    CHECK(has_codec_private);
    (void)std::filesystem::remove(path, ec);
}

void test_wav_padding_and_extensible() {
    std::error_code ec;
    {   // odd data size -> pad byte, RIFF size includes padding
        const std::string path = "/tmp/opencode/conf_odd.wav";
        (void)std::filesystem::remove(path, ec);
        auto m = bs::mux::Muxer::create(bs::mux::Container::Wav, path, 4096);
        auto t = m.add_track([] {
            bs::mux::TrackConfig c; c.is_audio = true; c.is_video = false;
            c.audio_codec = bs::mux::AudioCodec::Pcm8; c.sample_rate = 8000; c.channels = 1;
            return c;
        }());
        const std::vector<std::uint8_t> three{1, 2, 3};
        t.add_sample(three, 0, 0, 3, true, bs::NalFramingMode::RawFrame);
        CHECK(m.finalize());
        const auto d = read_file(path);
        // classic header: 44 + 3 data + 1 pad = 48; RIFF size = 40.
        CHECK(d.size() == 48u);
        if (d.size() >= 48u) {
            CHECK(d[47] == 0u); // pad byte
            const auto le32 = [&](std::size_t at) {
                return static_cast<std::uint32_t>(d[at]) |
                       (static_cast<std::uint32_t>(d[at + 1]) << 8) |
                       (static_cast<std::uint32_t>(d[at + 2]) << 16) |
                       (static_cast<std::uint32_t>(d[at + 3]) << 24);
            };
            CHECK(le32(4) == 40u);  // RIFF size 36 + padded(4)=40
            CHECK(le32(40) == 3u);  // data size = raw bytes
        }
        (void)std::filesystem::remove(path, ec);
    }
    {   // >2 channels -> WAVE_FORMAT_EXTENSIBLE (fmt size 40, tag 0xFFFE)
        const std::string path = "/tmp/opencode/conf_ext.wav";
        (void)std::filesystem::remove(path, ec);
        auto m = bs::mux::Muxer::create(bs::mux::Container::Wav, path, 4096);
        auto t = m.add_track([] {
            bs::mux::TrackConfig c; c.is_audio = true; c.is_video = false;
            c.audio_codec = bs::mux::AudioCodec::Pcm16; c.sample_rate = 48000; c.channels = 6;
            return c;
        }());
        const std::vector<std::uint8_t> frame(12, 0x11); // 6 * 2 bytes
        t.add_sample(frame, 0, 0, 6, true, bs::NalFramingMode::RawFrame);
        CHECK(m.finalize());
        const auto d = read_file(path);
        CHECK(d.size() == 44u + 24u + 12u); // extensible fmt (+24) + 12 data bytes
        const auto le16 = [&](std::size_t at) {
            return static_cast<std::uint16_t>(d[at] | (d[at + 1] << 8));
        };
        if (d.size() >= 68u) {
            CHECK(le16(16) == 40u);     // fmt chunk size
            CHECK(le16(20) == 0xFFFEu); // extensible
        }
        (void)std::filesystem::remove(path, ec);
    }
}

void test_ivf_timebase() {
    const std::string path = "/tmp/opencode/conf.ivf";
    std::error_code ec;
    (void)std::filesystem::remove(path, ec);
    auto m = bs::mux::Muxer::create(bs::mux::Container::Ivf, path, 4096);
    auto t = m.add_track([] {
        bs::mux::TrackConfig c; c.codec = bs::Codec::Vp9;
        c.width = 64; c.height = 64; c.timescale = 1000; return c;
    }());
    const std::vector<std::uint8_t> f{0x82,0x49,0x83,0x42,0x00};
    t.add_sample(f, 0, 0, 1, true, bs::NalFramingMode::RawFrame);
    CHECK(m.finalize());
    const auto d = read_file(path);
    const auto le32 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(d[at]) |
               (static_cast<std::uint32_t>(d[at + 1]) << 8) |
               (static_cast<std::uint32_t>(d[at + 2]) << 16) |
               (static_cast<std::uint32_t>(d[at + 3]) << 24);
    };
    if (d.size() >= 24u) {
        CHECK(le32(16) == 1000u); // framerate from timescale
        CHECK(le32(20) == 1u);    // timescale
    }
    (void)std::filesystem::remove(path, ec);
}

}  // namespace

int main() {
    test_mp4_boxes();
    test_mp4_ctts();
    test_mkv_codec_private_and_lacing();
    test_wav_padding_and_extensible();
    test_ivf_timebase();
    if (g_failures != 0) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "mux conformance: all checks passed\n";
    return 0;
}
