// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * Metadata round-trip test: write file-level and per-track tags through the
 * mux/ layer into MP4 / MKV and read them back through demux::Demuxer.
 */
#include <mux/muxer.hpp>
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

const char* container_name(bs::mux::Container c) noexcept {
    return c == bs::mux::Container::Mp4 ? "mp4" : "mkv";
}

bs::mux::Metadata make_file_metadata() {
    bs::mux::Metadata m;
    m.title = "Round Trip";
    m.artist = "bsparser";
    m.comment = "metadata smoke test";
    m.copyright = "BSD-2-Clause";
    m.creation_time = 1700000000; // 2023-11-14T22:13:20Z
    m.custom.emplace_back("ENCODER", "bsparser-mux");
    m.custom.emplace_back("SOURCE", "unit-test");
    return m;
}

void test_container(bs::mux::Container container) {
    const bs::mux::Metadata file_meta = make_file_metadata();

    auto muxer = bs::mux::Muxer::create(container);
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

    auto demuxer = bs::demux::Demuxer::init(muxer.data());
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
    CHECK(back.custom.size() == file_meta.custom.size());
    if (back.custom.size() == file_meta.custom.size()) {
        for (std::size_t i = 0; i < back.custom.size(); ++i) {
            CHECK(back.custom[i].first == file_meta.custom[i].first);
            CHECK(back.custom[i].second == file_meta.custom[i].second);
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
    auto muxer = bs::mux::Muxer::create(bs::mux::Container::Mp4);
    bs::mux::TrackConfig cfg;
    cfg.codec = bs::Codec::Hevc;
    muxer.add_track(cfg);
    CHECK(muxer.finalize());

    auto demuxer = bs::demux::Demuxer::init(muxer.data());
    CHECK(demuxer.ok());
    if (demuxer.ok()) {
        CHECK(demuxer.metadata().title.empty());
        CHECK(demuxer.metadata().custom.empty());
        CHECK(demuxer.metadata().creation_time == 0);
    }
}

} // namespace

int main() {
    test_container(bs::mux::Container::Mp4);
    test_container(bs::mux::Container::Mkv);
    test_empty_metadata();

    if (g_failures != 0) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
