// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * Streaming muxer test: build containers in memory, to a path and to an fd,
 * exercise the track/sink aliases and error paths, then feed the resulting
 * bytes back through the demuxer to verify structure (no parsed units are
 * expected from the deliberately fake frames).
 */
#include <mux/muxer.hpp>

#include <demux/demuxer.hpp>

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include <cstdio>

#include <filesystem>
#include <span>
#include <system_error>

#if !defined(_WIN32)
#include <cstdlib>
#include <unistd.h>
#endif

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cerr << "FAIL: " #cond " at line " << __LINE__ << "\n";     \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

const std::vector<std::uint8_t> kVp9Frame{0x82, 0x49, 0x83, 0x42, 0x00};
const std::vector<std::uint8_t> kVp9Frame2{0x82, 0x49, 0x83, 0x42, 0x01};
const std::vector<std::uint8_t> kAvcFrame{0, 0, 0, 1, 0x67, 0x42, 0, 0x1e, 0xaa};

bs::mux::TrackConfig vp9_config(std::uint32_t id, std::uint32_t w, std::uint32_t h) {
    bs::mux::TrackConfig cfg;
    cfg.id = id;
    cfg.codec = bs::Codec::Vp9;
    cfg.width = w;
    cfg.height = h;
    return cfg;
}

/* 1. In-memory sink + round-trip through the demuxer. */
void test_in_memory_sink() {
    auto muxer = bs::mux::Muxer::create(bs::mux::Container::Ivf);
    CHECK(muxer.ok());

    auto track = muxer.add_track(vp9_config(0, 320, 240));
    CHECK(track.valid());
    CHECK(track.config().codec == bs::Codec::Vp9);

    track.add_sample(kVp9Frame, /*pts=*/0, /*dts=*/0, /*duration=*/33, /*is_key=*/true);
    track.add_sample(kVp9Frame2, /*pts=*/33, /*dts=*/33, /*duration=*/33, /*is_key=*/false);
    CHECK(track.sampleCount() == 2);

    CHECK(muxer.finalize());
    CHECK(muxer.finalized());
    CHECK(!muxer.finalize());  // a second finalize is a no-op

    const std::span<const std::uint8_t> data = muxer.data();
    CHECK(!data.empty());
    CHECK(muxer.size() == data.size());
    CHECK(data.size() >= 4);
    CHECK(bs::demux::sniff(data) == bs::demux::Container::Ivf);
    CHECK(data[0] == 'D' && data[1] == 'K' && data[2] == 'I' && data[3] == 'F');

    auto demuxer = bs::demux::Demuxer::init(data);
    CHECK(demuxer.ok());
    CHECK(demuxer.container() == bs::demux::Container::Ivf);

    auto tracks = demuxer.tracks();
    CHECK(tracks.size() == 1);
    CHECK(tracks[0].codec == bs::Codec::Vp9);
    CHECK(tracks[0].width == 320);
    CHECK(tracks[0].height == 240);
    CHECK(tracks[0].sampleCount() == 2);

    const auto s0 = tracks[0].next();
    CHECK(s0);
    CHECK(s0.pts == 0);
    CHECK(s0.is_key);

    const auto s1 = tracks[0].next();
    CHECK(s1);
    CHECK(s1.pts == 33);
}

/* 2. Path sink: deferred write, then read the file back. */
void test_path_sink() {
    const std::string dir = "/tmp/opencode";
    const std::string path = dir + "/mux_stream_test.ivf";
    std::error_code ec;
    (void)std::filesystem::create_directories(dir, ec);
    (void)std::filesystem::remove(path, ec);
    CHECK(!std::filesystem::exists(path));

    auto muxer = bs::mux::Muxer::create(bs::mux::Container::Ivf, path);
    CHECK(muxer.ok());

    auto track = muxer.add_track(vp9_config(0, 64, 64));
    CHECK(track.valid());
    track.add_sample(kVp9Frame, 0);

    // open() records the destination only; the file appears at flush().
    CHECK(!std::filesystem::exists(path));
    CHECK(muxer.finalize());
    CHECK(std::filesystem::exists(path));

    auto demuxer = bs::demux::Demuxer::init(path);
    CHECK(demuxer.ok());
    const auto tracks = demuxer.tracks();
    CHECK(tracks.size() == 1);
    CHECK(tracks[0].codec == bs::Codec::Vp9);
    CHECK(tracks[0].sampleCount() == 1);

    (void)std::filesystem::remove(path, ec);
    CHECK(!std::filesystem::exists(path));
}

/* 3. fd sink: flush into a POSIX fd then re-read from offset zero. */
#if !defined(_WIN32)
void test_fd_sink() {
    char tmpl[] = "/tmp/opencode/mux_fd_XXXXXX";
    const int fd = ::mkstemp(tmpl);
    CHECK(fd >= 0);
    if (fd < 0) {
        return;
    }
    (void)::unlink(tmpl);  // the fd stays valid even after the name is gone

    auto muxer = bs::mux::Muxer::create(bs::mux::Container::Ivf, fd, /*close_on_destroy=*/false);
    CHECK(muxer.ok());

    auto track = muxer.add_track(vp9_config(0, 64, 64));
    CHECK(track.valid());
    track.add_sample(kVp9Frame, 0);

    CHECK(muxer.finalize());
    CHECK(::lseek(fd, 0, SEEK_SET) == 0);

    std::vector<std::uint8_t> bytes;
    std::uint8_t buf[256];
    ssize_t n = 0;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0) {
        bytes.insert(bytes.end(), buf, buf + n);
    }
    CHECK(n >= 0);
    CHECK(!bytes.empty());

    auto demuxer = bs::demux::Demuxer::init(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
    CHECK(demuxer.ok());
    const auto tracks = demuxer.tracks();
    CHECK(tracks.size() == 1);
    CHECK(tracks[0].codec == bs::Codec::Vp9);
    CHECK(tracks[0].sampleCount() == 1);

    ::close(fd);
}
#endif

/* 4. Container inference from the file extension. */
void test_extension_open() {
    auto ivf = bs::mux::Muxer::open("out.ivf");
    CHECK(ivf.ok());
    CHECK(ivf.container() == bs::mux::Container::Ivf);

    auto unknown = bs::mux::Muxer::open("/tmp/x.unknown");
    CHECK(!unknown.ok());
    CHECK(unknown.container() == bs::mux::Container::Unknown);
}

/* 5. Error handling. */
void test_errors() {
    auto no_tracks = bs::mux::Muxer::create(bs::mux::Container::Mp4);
    CHECK(no_tracks.ok());
    CHECK(!no_tracks.finalize());

    auto bad_codec = bs::mux::Muxer::create(bs::mux::Container::Ivf);
    bs::mux::TrackConfig avc;
    avc.codec = bs::Codec::Avc;
    avc.width = 64;
    avc.height = 64;
    auto avc_track = bad_codec.add_track(avc);
    CHECK(avc_track.valid());
    avc_track.add_sample(kAvcFrame, 0);
    CHECK(!bad_codec.finalize());

    auto unsupported = bs::mux::Muxer::create(bs::mux::Container::Unknown);
    CHECK(!unsupported.ok());
    CHECK(!unsupported.finalize());

    bs::mux::Track def;
    CHECK(!def.valid());
    def.add_sample(kVp9Frame, 0);
    def.addSample(kVp9Frame, 0);
    CHECK(def.sampleCount() == 0);
    CHECK(def.sample_count() == 0);
}

/* 6. Track-handle and muxer aliases. */
void test_aliases() {
    auto muxer = bs::mux::Muxer::create(bs::mux::Container::Ivf);
    CHECK(static_cast<bool>(muxer));
    CHECK(muxer.sink() != nullptr);
    CHECK(muxer.sink()->ok());
    CHECK(muxer.size() == 0);
    CHECK(muxer.data().empty());

    auto track = muxer.addTrack(vp9_config(7, 64, 64));
    CHECK(track.valid());
    CHECK(track.view().codec == bs::Codec::Vp9);
    CHECK(track.config().width == 64);

    track.addSample(kVp9Frame, 0);
    track.add_sample(kVp9Frame2, 33);
    CHECK(track.sampleCount() == 2);
    CHECK(track.sample_count() == 2);

    CHECK(muxer.tracks().size() == 1);
    CHECK(muxer.track(0).valid());
    CHECK(muxer.select(7).valid());
    CHECK(!muxer.select(99).valid());
    CHECK(!muxer.track(9).valid());

    CHECK(muxer.finalize());
    CHECK(muxer.done());
    CHECK(muxer.size() == muxer.data().size());
    CHECK(!muxer.data().empty());

    bs::mux::Muxer reused;
    CHECK(reused.load(bs::mux::Container::Ivf));
    CHECK(reused.ok());
    CHECK(reused.container() == bs::mux::Container::Ivf);
}

/* 7. Multi-track MKV with an Annex-B AVC sample and a VP9 sample. */
void test_multi_track_mkv() {
    auto muxer = bs::mux::Muxer::create(bs::mux::Container::Mkv);
    CHECK(muxer.ok());

    bs::mux::TrackConfig avc;
    avc.id = 1;
    avc.codec = bs::Codec::Avc;
    avc.width = 64;
    avc.height = 64;
    auto avc_track = muxer.add_track(avc);

    auto vp9_track = muxer.add_track(vp9_config(2, 64, 64));
    CHECK(avc_track.valid());
    CHECK(vp9_track.valid());

    avc_track.add_sample(kAvcFrame, 0);
    vp9_track.add_sample(kVp9Frame, 0);

    CHECK(muxer.finalize());
    CHECK(!muxer.data().empty());
    CHECK(bs::demux::sniff(muxer.data()) == bs::demux::Container::Mkv);

    auto demuxer = bs::demux::Demuxer::init(muxer.data());
    CHECK(demuxer.ok());
    const auto tracks = demuxer.tracks();
    CHECK(tracks.size() == 2);

    bool saw_avc = false;
    bool saw_vp9 = false;
    for (const auto& track : tracks) {
        if (track.codec == bs::Codec::Avc) {
            saw_avc = true;
            CHECK(track.sampleCount() == 1);
        } else if (track.codec == bs::Codec::Vp9) {
            saw_vp9 = true;
            CHECK(track.sampleCount() == 1);
        }
    }
    CHECK(saw_avc);
    CHECK(saw_vp9);

    const auto selected = demuxer.select(2);
    CHECK(selected.valid());
    CHECK(selected.codec == bs::Codec::Vp9);
}

}  // namespace

int main() {
    test_in_memory_sink();
    test_path_sink();
#if !defined(_WIN32)
    test_fd_sink();
#endif
    test_extension_open();
    test_errors();
    test_aliases();
    test_multi_track_mkv();

    if (g_failures != 0) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }

    std::cout << "mux_stream_test: all checks passed\n";
    return 0;
}
