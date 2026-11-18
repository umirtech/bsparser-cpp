// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * Metadata round-trip test: write file-level and per-track tags through the
 * mux/ layer into MP4 / MKV and read them back through demux::Demuxer.
 *
 * The muxer is streaming-only: it writes to a path/fd sink instead of
 * retaining an in-memory buffer. Each test therefore muxes into a unique
 * temporary file and reads the bytes back for inspection.
 */
#include <mux/muxer.hpp>
#include <demux/demuxer.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
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

const char* container_name(bs::mux::Container c) noexcept {
    return c == bs::mux::Container::Mp4 ? "mp4" : "mkv";
}

/* Unique temp path per invocation so tests never collide. */
std::string temp_path(const std::string& prefix, bs::mux::Container container) {
    static int counter = 0;
    const char* ext = "bin";
    switch (container) {
        case bs::mux::Container::Mp4:
            ext = "mp4";
            break;
        case bs::mux::Container::Mkv:
            ext = "mkv";
            break;
        case bs::mux::Container::Ivf:
            ext = "ivf";
            break;
        default:
            break;
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

bs::mux::Metadata make_file_metadata() {
    bs::mux::Metadata m;
    m.title = "Round Trip";
    m.artist = "bsparser";
    m.comment = "metadata smoke test";
    m.copyright = "BSD-2-Clause";
    m.creation_time = 1700000000; // 2023-11-14T22:13:20Z
    m.custom.emplace_back("SOURCE", "bsparser-mux");
    m.custom.emplace_back("NOTE", "unit-test");
    return m;
}

void test_container(bs::mux::Container container) {
    const bs::mux::Metadata file_meta = make_file_metadata();
    TempFile tmp(temp_path("mux_meta", container));

    auto muxer = bs::mux::Muxer::create(container, tmp.path);
    CHECK(muxer.ok());
    muxer.set_metadata(file_meta);

    bs::mux::TrackConfig cfg;
    cfg.id = 1;
    cfg.codec = bs::Codec::Avc;
    cfg.width = 64;
    cfg.height = 64;
    cfg.timescale = 1000;
    cfg.language = "eng";
    cfg.metadata.title = "Video Track";
    cfg.metadata.artist = "Track Artist";
    auto track = muxer.add_track(cfg);

    // One Annex-B AVC sample so the demuxer indexes a video track.
    const std::vector<std::uint8_t> sample{
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1E, 0xAA, 0xBB
    };
    track.add_sample(sample, 0, 0, 1000, true, bs::NalFramingMode::AnnexB);

    CHECK(muxer.finalize());
    if (!muxer.finalized()) {
        return;
    }

    auto demuxer = bs::demux::Demuxer::init(tmp.path);
    CHECK(demuxer.ok());
    if (!demuxer.ok()) {
        return;
    }

    const bs::demux::ContainerMetadata& back = demuxer.metadata();
    CHECK(back.title == file_meta.title);
    CHECK(back.artist == file_meta.artist);
    CHECK(back.comment == file_meta.comment);
    CHECK(back.copyright == file_meta.copyright);
    CHECK(back.creation_time == file_meta.creation_time);
    // The demuxer maps MP4's (c)too to a dedicated field but leaves MKV's
    // ENCODER tag in custom[]. Filter it out either way so the caller's own
    // custom tags can be compared.
    std::vector<std::pair<std::string, std::string>> user_custom;
    for (const auto& kv : back.custom) {
        if (kv.first == "ENCODER" && kv.second == bs::mux::encoder_signature()) {
            continue;
        }
        user_custom.push_back(kv);
    }
    CHECK(user_custom.size() == file_meta.custom.size());
    if (user_custom.size() == file_meta.custom.size()) {
        for (std::size_t i = 0; i < user_custom.size(); ++i) {
            CHECK(user_custom[i].first == file_meta.custom[i].first);
            CHECK(user_custom[i].second == file_meta.custom[i].second);
        }
    }

    auto tracks = demuxer.tracks();
    CHECK(tracks.size() == 1);
    if (tracks.empty()) {
        return;
    }

    CHECK(tracks[0].language == "eng");
    CHECK(tracks[0].metadata.title == "Video Track");

    std::cout << container_name(container) << " metadata: "
              << (back.title == file_meta.title ? "OK" : "FAIL") << "\n";
}

void test_empty_metadata() {
    TempFile tmp(temp_path("mux_meta_empty", bs::mux::Container::Mp4));
    auto muxer = bs::mux::Muxer::create(bs::mux::Container::Mp4, tmp.path);
    bs::mux::TrackConfig cfg;
    cfg.codec = bs::Codec::Hevc;
    muxer.add_track(cfg);
    CHECK(muxer.finalize());

    auto demuxer = bs::demux::Demuxer::init(tmp.path);
    CHECK(demuxer.ok());
    if (demuxer.ok()) {
        CHECK(demuxer.metadata().title.empty());
        CHECK(demuxer.metadata().custom.empty());
        CHECK(demuxer.metadata().creation_time == 0);
    }
}

bool contains(std::span<const std::uint8_t> bytes, std::string_view needle) {
    if (needle.empty() || bytes.size() < needle.size()) {
        return false;
    }
    for (std::size_t i = 0; i + needle.size() <= bytes.size(); ++i) {
        if (std::memcmp(bytes.data() + i, needle.data(), needle.size()) == 0) {
            return true;
        }
    }
    return false;
}

void test_auto_encoder_tag(bs::mux::Container container) {
    TempFile tmp(temp_path("mux_meta_tag", container));
    auto muxer = bs::mux::Muxer::create(container, tmp.path);

    bs::mux::Metadata meta;
    meta.title = "Tagged";
    meta.custom.emplace_back("encoder", "SPOOFED");
    meta.custom.emplace_back("\xC2\xA9" "too", "SPOOFED-TOO");
    muxer.set_metadata(meta);

    bs::mux::TrackConfig cfg;
    cfg.codec = container == bs::mux::Container::Ivf ? bs::Codec::Vp9 : bs::Codec::Avc;
    cfg.width = 64;
    cfg.height = 64;
    cfg.metadata.custom.emplace_back("ENCODER", "TRACK-SPOOF");
    auto track = muxer.add_track(cfg);

    const std::vector<std::uint8_t> sample{
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1E, 0xAA, 0xBB
    };
    if (container == bs::mux::Container::Ivf) {
        const std::vector<std::uint8_t> frame{0x82, 0x49, 0x83, 0x42, 0x00};
        track.add_sample(frame, 0, 0, 33, true, bs::NalFramingMode::RawFrame);
    } else {
        track.add_sample(sample, 0, 0, 1000, true, bs::NalFramingMode::AnnexB);
    }

    CHECK(muxer.finalize());
    const std::vector<std::uint8_t> bytes = read_file_bytes(tmp.path);
    const std::span<const std::uint8_t> out(bytes);

    if (container == bs::mux::Container::Ivf) {
        // IVF has no metadata fields: neither the signature nor a spoof survives.
        CHECK(!contains(out, bs::mux::encoder_signature()));
        CHECK(!contains(out, "SPOOFED"));
        return;
    }

    CHECK(contains(out, bs::mux::encoder_signature()));
    CHECK(!contains(out, "SPOOFED"));
    CHECK(!contains(out, "TRACK-SPOOF"));

    // Demuxer is unchanged: it reports whatever the file actually contains.
    auto demuxer = bs::demux::Demuxer::init(tmp.path);
    CHECK(demuxer.ok());
    if (demuxer.ok()) {
        bool found = false;
        for (const auto& kv : demuxer.metadata().custom) {
            if (kv.second == bs::mux::encoder_signature()) {
                found = true;
            }
            CHECK(kv.second != "SPOOFED");
        }
        CHECK(found || demuxer.metadata().title == "Tagged");
    }
}

} // namespace

int main() {
    test_container(bs::mux::Container::Mp4);
    test_container(bs::mux::Container::Mkv);
    test_empty_metadata();
    test_auto_encoder_tag(bs::mux::Container::Mp4);
    test_auto_encoder_tag(bs::mux::Container::Mkv);
    test_auto_encoder_tag(bs::mux::Container::Ivf);

    if (g_failures != 0) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
