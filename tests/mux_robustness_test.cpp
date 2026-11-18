// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * Writer hardening regression tests:
 *   - MP4 stss keyframe table survives a mux/demux round trip
 *   - metadata strings with embedded NULs / invalid UTF-8 are sanitized
 *   - 64-bit ISO-BMFF box headers are selected above the 32-bit threshold
 *   - nothrow framer wrappers return an empty result instead of throwing
 *
 * The muxer is streaming-only: output goes to a path sink rather than an
 * in-memory buffer, so each round trip uses a unique temporary file.
 */
#include <mux/muxer.hpp>
#include <demux/demuxer.hpp>
#include <bsparser.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <utility>
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

const std::vector<std::uint8_t> kSample{
    0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1E, 0xAA, 0xBB
};

/* Unique temp path per invocation so tests never collide. */
std::string temp_path(const std::string& prefix, bs::mux::Container container) {
    static int counter = 0;
    const char* ext = "mp4";
    if (container == bs::mux::Container::Mkv) {
        ext = "mkv";
    }
    return "/tmp/opencode/" + prefix + "_" + std::to_string(counter++) + "." + ext;
}

/* Remove the temp file on every exit path, including early returns. */
struct TempFile {
    std::string path;

    explicit TempFile(std::string p) : path(std::move(p)) {}
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
};

std::vector<std::uint8_t> read_file_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()
    );
}

void test_stss_roundtrip() {
    TempFile tmp(temp_path("mux_robust_stss", bs::mux::Container::Mp4));
    auto muxer = bs::mux::Muxer::create(bs::mux::Container::Mp4, tmp.path);
    bs::mux::TrackConfig cfg;
    cfg.codec = bs::Codec::Avc;
    cfg.width = 64;
    cfg.height = 64;
    auto track = muxer.add_track(cfg);

    // The streaming muxer folds parameter-set-only samples into the next
    // access unit, so append a VCL NAL to make each sample a real frame.
    std::vector<std::uint8_t> frame = kSample;
    frame.insert(frame.end(), {0x00, 0x00, 0x00, 0x01, 0x41, 0x9A, 0x20});

    const bool keys[] = {true, false, false, true, false};
    for (std::size_t i = 0; i < 5; ++i) {
        track.add_sample(frame, static_cast<std::int64_t>(i) * 1000,
                         static_cast<std::int64_t>(i) * 1000, 1000, keys[i],
                         bs::NalFramingMode::AnnexB);
    }

    CHECK(muxer.finalize());
    const std::vector<std::uint8_t> bytes = read_file_bytes(tmp.path);
    auto demuxer = bs::demux::Demuxer::init(std::span<const std::uint8_t>(bytes));
    CHECK(demuxer.ok());
    if (!demuxer.ok()) {
        return;
    }

    auto tracks = demuxer.tracks();
    CHECK(tracks.size() == 1);
    if (tracks.empty()) {
        return;
    }

    std::vector<bool> got;
    while (tracks[0].hasNext()) {
        auto s = tracks[0].next();
        if (!s) {
            break;
        }
        got.push_back(s.is_key);
    }

    CHECK(got.size() == 5);
    if (got.size() == 5) {
        for (std::size_t i = 0; i < 5; ++i) {
            CHECK(got[i] == keys[i]);
        }
    }
}

void test_metadata_sanitization(bs::mux::Container container) {
    TempFile tmp(temp_path("mux_robust_sanitize", container));
    auto muxer = bs::mux::Muxer::create(container, tmp.path);

    bs::mux::Metadata meta;
    // "Ti\0tle" plus an invalid UTF-8 lead byte followed by ASCII.
    meta.title = std::string("Ti\0tle", 6) + "\xFF" + "X";
    meta.artist = "artist";
    meta.custom.emplace_back(std::string("K\0E", 3), std::string("V\0\xFF", 3));
    muxer.set_metadata(meta);

    bs::mux::TrackConfig cfg;
    cfg.codec = bs::Codec::Avc;
    cfg.width = 64;
    cfg.height = 64;
    cfg.language = "en\0g"; // 4 bytes -> invalid -> fallback "und"
    cfg.metadata.title = std::string("Track\0Name", 10);
    auto track = muxer.add_track(cfg);
    track.add_sample(kSample, 0, 0, 1000, true, bs::NalFramingMode::AnnexB);

    CHECK(muxer.finalize());
    const std::vector<std::uint8_t> bytes = read_file_bytes(tmp.path);
    auto demuxer = bs::demux::Demuxer::init(std::span<const std::uint8_t>(bytes));
    CHECK(demuxer.ok());
    if (!demuxer.ok()) {
        return;
    }

    const auto& back = demuxer.metadata();
    CHECK(back.title.find('\0') == std::string::npos);
    CHECK(back.title.find("\xEF\xBF\xBD") != std::string::npos);
    CHECK(back.artist == "artist");
    CHECK(!back.custom.empty());
    if (!back.custom.empty()) {
        CHECK(back.custom[0].first.find('\0') == std::string::npos);
        CHECK(back.custom[0].second.find('\0') == std::string::npos);
    }

    auto tracks = demuxer.tracks();
    CHECK(!tracks.empty());
    if (!tracks.empty()) {
        CHECK(tracks[0].language.find('\0') == std::string::npos);
        CHECK(tracks[0].metadata.title.find('\0') == std::string::npos);
    }
}

void test_box_size_threshold() {
    using bs::mux::detail::box_size_bytes;
    CHECK(box_size_bytes(0) == 8u);
    CHECK(box_size_bytes(1000) == 8u);
    CHECK(box_size_bytes(0xFFFFFFFFull - 8ull) == 8u);
    CHECK(box_size_bytes(0xFFFFFFFFull - 7ull) == 16u);
    CHECK(box_size_bytes(0x1'0000'0000ull) == 16u);
}

void test_nothrow_framers() {
    // Truncated length-prefixed NAL: the length field declares 100 bytes but
    // only 2 follow. The throwing version rejects it; the wrapper must not throw.
    const std::vector<std::uint8_t> truncated{0x00, 0x00, 0x00, 0x64, 0x65, 0x66};
    auto nals = bs::split_nal_units_nothrow(
        truncated, bs::NalFramingMode::LengthPrefixed, 4
    );
    CHECK(nals.empty());

    auto annexb = bs::split_annex_b_nothrow(kSample);
    CHECK(annexb.size() == 1);

    auto obus = bs::av1::split_obus_nothrow(kSample);
    (void)obus; // no OBUs in this AVC sample; must simply return, not throw

    bool threw = false;
    try {
        (void)bs::split_nal_units(truncated, bs::NalFramingMode::LengthPrefixed, 4);
    } catch (...) {
        threw = true;
    }
    CHECK(threw); // the original throwing API remains unchanged
}

} // namespace

int main() {
    test_stss_roundtrip();
    test_metadata_sanitization(bs::mux::Container::Mp4);
    test_metadata_sanitization(bs::mux::Container::Mkv);
    test_box_size_threshold();
    test_nothrow_framers();

    if (g_failures != 0) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "mux robustness: all checks passed\n";
    return 0;
}
