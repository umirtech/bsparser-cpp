// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "mux/stream.hpp"
#include "mux/file_sink.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bs {
namespace mux {
namespace detail {

/*
 * Write a single-stream IVF (DKIF) container.
 *
 * IVF carries exactly one codec stream, so only tracks[0] / samples[0] are
 * used and the codec must be VP8 or VP9.  The layout mirrors what
 * demux::build_ivf() expects: a 32-byte file header followed by one
 * 12-byte frame header + raw frame payload per sample.
 *
 * FileSink is a pure append buffer, so the frame block is assembled first
 * (allowing an accurate frame count) and the header is emitted just before
 * it in a second write().
 */
inline bool write_ivf(FileSink& sink,
                      const std::vector<TrackConfig>& tracks,
                      const std::vector<std::vector<Sample>>& samples) {
    if (tracks.empty() || samples.empty()) {
        return false;
    }

    const TrackConfig& track = tracks.front();

    // IVF supports VP8 / VP9 only.
    if (!container_supports_codec(Container::Ivf, track.codec)) {
        return false;
    }

    const std::vector<Sample>& frames = samples.front();

    // Assemble the frame block first so the header can carry the real count.
    std::vector<std::uint8_t> frame_block;
    std::uint32_t frame_count = 0;

    for (const Sample& sample : frames) {
        if (sample.data.empty()) {
            continue;
        }
        if (sample.data.size() > 0xFFFFFFFFull) {
            return false;  // IVF frame sizes are 32-bit
        }

        std::array<std::uint8_t, 12> fh{};

        const std::uint32_t size = static_cast<std::uint32_t>(sample.data.size());
        fh[0] = static_cast<std::uint8_t>(size);
        fh[1] = static_cast<std::uint8_t>(size >> 8);
        fh[2] = static_cast<std::uint8_t>(size >> 16);
        fh[3] = static_cast<std::uint8_t>(size >> 24);

        const std::uint64_t ts = static_cast<std::uint64_t>(sample.pts);
        for (unsigned i = 0; i < 8; ++i) {
            fh[4 + i] = static_cast<std::uint8_t>(ts >> (i * 8));
        }

        frame_block.insert(frame_block.end(), fh.begin(), fh.end());
        frame_block.insert(frame_block.end(), sample.data.begin(), sample.data.end());
        ++frame_count;
    }

    // The demuxer rejects a file with no frames; don't emit one either.
    if (frame_count == 0) {
        return false;
    }

    // 32-byte IVF file header (all little-endian).
    std::array<std::uint8_t, 32> header{};
    header[0] = 'D';
    header[1] = 'K';
    header[2] = 'I';
    header[3] = 'F';

    // bytes 4..5: version = 0
    // bytes 6..7: header length = 32
    header[6] = static_cast<std::uint8_t>(32);

    const char* const fourcc = ivf_fourcc(track.codec);
    header[8] = static_cast<std::uint8_t>(fourcc[0]);
    header[9] = static_cast<std::uint8_t>(fourcc[1]);
    header[10] = static_cast<std::uint8_t>(fourcc[2]);
    header[11] = static_cast<std::uint8_t>(fourcc[3]);

    const std::uint16_t width = static_cast<std::uint16_t>(track.width);
    const std::uint16_t height = static_cast<std::uint16_t>(track.height);
    header[12] = static_cast<std::uint8_t>(width);
    header[13] = static_cast<std::uint8_t>(width >> 8);
    header[14] = static_cast<std::uint8_t>(height);
    header[15] = static_cast<std::uint8_t>(height >> 8);

    // bytes 16..19: framerate = 30
    // bytes 20..23: timescale = 1
    header[16] = static_cast<std::uint8_t>(30);

    // bytes 24..27: frame count
    header[24] = static_cast<std::uint8_t>(frame_count);
    header[25] = static_cast<std::uint8_t>(frame_count >> 8);
    header[26] = static_cast<std::uint8_t>(frame_count >> 16);
    header[27] = static_cast<std::uint8_t>(frame_count >> 24);

    // bytes 28..31 remain zero (unused).

    sink.write(header);
    sink.write(frame_block);

    return sink.ok();
}

}  // namespace detail
}  // namespace mux
}  // namespace bs
