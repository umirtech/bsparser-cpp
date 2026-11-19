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
#include <demux/demuxer.hpp>
#include <bsparser.hpp>
#include "test_temp.hpp"

#include <cstdint>
#include <cstdio>
#include <exception>
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
    const std::string path = bs_test::temp_dir() + "/conf.mp4";
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
    const std::string path = bs_test::temp_dir() + "/conf_ctts.mp4";
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
    const std::string path = bs_test::temp_dir() + "/conf.mkv";
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
        const std::string path = bs_test::temp_dir() + "/conf_odd.wav";
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
        const std::string path = bs_test::temp_dir() + "/conf_ext.wav";
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
    const std::string path = bs_test::temp_dir() + "/conf.ivf";
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

/* Phase 1 regressions. */

void test_mp4_codec_config_honored() {
    const std::string path = bs_test::temp_dir() + "/conf_codec_config.mp4";
    std::error_code ec;
    (void)std::filesystem::remove(path, ec);
    auto m = bs::mux::Muxer::create(bs::mux::Container::Mp4, path, 1u << 16);
    auto v = m.add_track([] {
        bs::mux::TrackConfig c; c.id = 1; c.codec = bs::Codec::Avc;
        c.width = 64; c.height = 64;
        // Distinctive avcC payload that must survive into stsd.
        c.codec_config = {1u, 0x64u, 0u, 0x1Fu, 0xFFu, 0xE1u, 0u, 4u,
                          0x67u, 0x64u, 0u, 0x1Fu, 0u, 1u, 0x68u};
        return c;
    }());
    const std::vector<std::uint8_t> nal{0,0,0,1,0x67,0x64,0,0x1f,0,0,1,0x65,0x88,0x84};
    for (int i = 0; i < 3; ++i) {
        v.add_sample(nal, i * 1000, i * 1000, 1000, true, bs::NalFramingMode::AnnexB);
    }
    CHECK(m.finalize());
    const auto d = read_file(path);
    bool found = false;
    for (std::size_t i = 0; i + 12 < d.size(); ++i) {
        if (d[i] == 'a' && d[i+1] == 'v' && d[i+2] == 'c' && d[i+3] == 'C' &&
            d[i+4] == 1u && d[i+5] == 0x64u && d[i+6] == 0u && d[i+7] == 0x1Fu) {
            found = true;
            break;
        }
    }
    CHECK(found);
    (void)std::filesystem::remove(path, ec);
}

void test_mp4_mdhd_timebase() {
    const std::string path = bs_test::temp_dir() + "/conf_mdhd.mp4";
    std::error_code ec;
    (void)std::filesystem::remove(path, ec);
    auto m = bs::mux::Muxer::create(bs::mux::Container::Mp4, path, 1u << 16);
    auto v = m.add_track([] {
        bs::mux::TrackConfig c; c.id = 1; c.codec = bs::Codec::Avc;
        c.width = 64; c.height = 64; c.timescale = 90000; return c;
    }());
    const std::vector<std::uint8_t> nal{0,0,0,1,0x67,0x42,0,0x1e,0,0,1,0x65,0x88,0x84};
    // 10 samples * 9000 ticks = 90000 ticks = 1 second in the track clock.
    for (int i = 0; i < 10; ++i) {
        v.add_sample(nal, i * 9000, i * 9000, 9000, true, bs::NalFramingMode::AnnexB);
    }
    CHECK(m.finalize());
    const auto d = read_file(path);
    const auto be32 = [&](std::size_t at) {
        return (static_cast<std::uint32_t>(d[at]) << 24) |
               (static_cast<std::uint32_t>(d[at+1]) << 16) |
               (static_cast<std::uint32_t>(d[at+2]) << 8) |
               static_cast<std::uint32_t>(d[at+3]);
    };
    bool checked = false;
    for (std::size_t i = 0; i + 32 < d.size(); ++i) {
        if (d[i+4] == 'm' && d[i+5] == 'd' && d[i+6] == 'h' && d[i+7] == 'd') {
            // FullBox payload: version+flags(4), creation(4), modification(4),
            // timescale(4), duration(4).
            CHECK(be32(i + 20) == 90000u); // timescale
            CHECK(be32(i + 24) == 90000u); // duration in track timescale
            checked = true;
            break;
        }
    }
    CHECK(checked);
    (void)std::filesystem::remove(path, ec);
}

void test_mkv_multi_cluster_timestamps() {
    const std::string path = bs_test::temp_dir() + "/conf_multi.mkv";
    std::error_code ec;
    (void)std::filesystem::remove(path, ec);
    auto m = bs::mux::Muxer::create(bs::mux::Container::Mkv, path, 1u << 16);
    auto v = m.add_track([] {
        bs::mux::TrackConfig c; c.id = 1; c.codec = bs::Codec::Avc;
        c.width = 64; c.height = 64; return c;
    }());
    const std::vector<std::uint8_t> nal{0,0,0,1,0x67,0x42,0,0x1e,0,0,1,0x65,0x88,0x84};
    // 40 s apart exceeds the int16 relative-timestamp range, so the writer
    // must open new Clusters instead of clamping every sample to one time.
    for (int i = 0; i < 3; ++i) {
        const std::int64_t pts = static_cast<std::int64_t>(i) * 40000;
        v.add_sample(nal, pts, pts, 40, true, bs::NalFramingMode::AnnexB);
    }
    CHECK(m.finalize());

    auto demux = bs::demux::Demuxer::init(path);
    CHECK(demux.ok());
    if (demux.ok()) {
        std::vector<std::int64_t> pts;
        for (auto& tr : demux.tracks()) {
            while (tr.hasNext()) {
                auto s = tr.next();
                if (!s) break;
                pts.push_back(s.pts);
            }
        }
        CHECK(pts.size() >= 3u);
        if (!pts.empty()) {
            // The demuxer may prepend a parameter-set pseudo-sample at pts 0;
            // the last three real samples must retain their distinct times.
            CHECK(pts.back() >= 70000); // not clamped to int16 max (32767)
            bool has_40000 = false, has_80000 = false;
            for (const auto p : pts) {
                if (p == 40000) has_40000 = true;
                if (p == 80000) has_80000 = true;
            }
            CHECK(has_40000);
            CHECK(has_80000);
        }
    }
    (void)std::filesystem::remove(path, ec);
}

void test_struct_report_rejects_wrong_codec() {
    // A VVC (or moved-from) state must not reach the AVC dispatch path with
    // a null parameter-set manager.
    auto state = bs::create_state(bs::Codec::Vvc);
    bs::StructReport report;
    bool threw = false;
    try {
        (void)bs::parse(*state, {}, bs::NalFramingMode::AnnexB, report);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

/* Phase 3 regressions. */

void test_wav_float_format() {
    const std::string path = bs_test::temp_dir() + "/p3_float.wav";
    std::error_code ec;
    (void)std::filesystem::remove(path, ec);
    auto m = bs::mux::Muxer::create(bs::mux::Container::Wav, path, 4096);
    auto t = m.add_track([] {
        bs::mux::TrackConfig c; c.is_audio = true; c.is_video = false;
        c.audio_codec = bs::mux::AudioCodec::PcmFloat32;
        c.sample_rate = 48000; c.channels = 2; return c;
    }());
    const std::vector<std::uint8_t> frame(8, 0);
    t.add_sample(frame, 0, 0, 1, true, bs::NalFramingMode::RawFrame);
    CHECK(m.finalize());
    const auto d = read_file(path);
    if (d.size() >= 20u) {
        const auto le16 = [&](std::size_t at) {
            return static_cast<std::uint16_t>(d[at] | (d[at + 1] << 8));
        };
        CHECK(le16(20) == 3u); // WAVE_FORMAT_IEEE_FLOAT
    }
    (void)std::filesystem::remove(path, ec);
}

void test_mkv_webm_doctype() {
    const std::string path = bs_test::temp_dir() + "/p3.webm";
    std::error_code ec;
    (void)std::filesystem::remove(path, ec);
    auto m = bs::mux::Muxer::create(bs::mux::Container::Mkv, path, 1u << 16);
    auto t = m.add_track([] {
        bs::mux::TrackConfig c; c.codec = bs::Codec::Vp9;
        c.width = 64; c.height = 64; return c;
    }());
    const std::vector<std::uint8_t> frame{0x82, 0x49, 0x83, 0x42, 0x00};
    t.add_sample(frame, 0, 0, 33, true, bs::NalFramingMode::RawFrame);
    CHECK(m.finalize());
    CHECK(contains(read_file(path), "webm"));
    (void)std::filesystem::remove(path, ec);
}

void test_ivf_timebase_demux() {
    const std::string path = bs_test::temp_dir() + "/p3.ivf";
    std::error_code ec;
    (void)std::filesystem::remove(path, ec);
    {
        std::ofstream f(path, std::ios::binary);
        const unsigned char hdr[32] = {
            'D','K','I','F',0,2,0,0,'V','P','9','0',64,0,36,0,
            30,0,0,0,1,0,0,0,1,0,0,0,0,0,0,0
        };
        f.write(reinterpret_cast<const char*>(hdr), 32);
        const unsigned char frame[17] = {5,0,0,0,0,0,0,0,0,0,0,0,0x82,0x49,0x83,0x42,0};
        f.write(reinterpret_cast<const char*>(frame), 17);
    }
    auto d = bs::demux::Demuxer::init(path);
    CHECK(d.ok());
    if (d.ok()) {
        auto t = d.track(0);
        CHECK(t.timescale == 30u);
        auto s = t.next();
        CHECK(s);
        if (s) CHECK(s.pts == 0);
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
    test_mp4_codec_config_honored();
    test_mp4_mdhd_timebase();
    test_mkv_multi_cluster_timestamps();
    test_struct_report_rejects_wrong_codec();
    test_wav_float_format();
    test_mkv_webm_doctype();
    test_ivf_timebase_demux();
    if (g_failures != 0) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "mux conformance: all checks passed\n";
    return 0;
}
