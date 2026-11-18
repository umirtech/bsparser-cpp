// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "mux/stream.hpp"
#include "mux/file_sink.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace bs {
namespace mux {
namespace detail {

/*
 * Write a single-stream IVF (DKIF) container.
 *
 * IVF carries exactly one codec stream, so a single TrackConfig is used
 * and the codec must be VP8 or VP9.  The layout mirrors what
 * demux::build_ivf() expects: a 32-byte file header followed by one
 * 12-byte frame header + raw frame payload per sample.
 *
 * The file is written in streaming fashion: a 32-byte header with a
 * placeholder frame count is emitted first, frames are appended as they
 * arrive, and the count is patched at finish.
 *
 * Timebase: the header's framerate field carries the tick rate -- the
 * caller's TrackConfig::timescale, defaulting to 1000 -- and the timescale
 * field is 1.  Frame timestamps are stored verbatim from Sample::pts, in
 * those ticks (seconds = pts / framerate).  The caller is responsible for
 * supplying pts in that unit; the sidecar index records the same values.
 */

/* Append a little-endian integer to the sidecar index buffer. */
inline void ivf_push_le32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (unsigned i = 0; i < 4u; ++i) {
        out.push_back(static_cast<std::uint8_t>(value >> (8u * i)));
    }
}

inline void ivf_push_le64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (unsigned i = 0; i < 8u; ++i) {
        out.push_back(static_cast<std::uint8_t>(value >> (8u * i)));
    }
}

/*
 * Write an optional frame index next to an IVF file.
 *
 * `<ivf_path>.idx` is a flat binary table (all integers little-endian):
 *
 *   u32 frame_count
 *   repeated frame_count times:
 *       u32 frame_size    payload size of the frame in the IVF file
 *       u64 pts           presentation timestamp of the frame
 *       u64 file_offset   byte offset of the frame's 12-byte IVF frame
 *                         header (its payload starts 12 bytes later)
 *
 * Empty samples are skipped: they produce no frame
 * in the IVF file, so they get no index entry either.  Returns false if the
 * sidecar cannot be opened/written or a frame size does not fit in u32.
 */
inline bool write_ivf_index(const std::string& ivf_path,
                            const std::vector<Sample>& samples) {
    if (ivf_path.empty()) {
        return false;
    }

    std::vector<std::uint8_t> index;

    std::uint32_t count = 0;
    for (const Sample& sample : samples) {
        if (!sample.data.empty()) {
            ++count;
        }
    }
    ivf_push_le32(index, count);

    // A 32-byte IVF file header precedes the first 12-byte frame header.
    std::uint64_t offset = 32u;
    for (const Sample& sample : samples) {
        if (sample.data.empty()) {
            continue;
        }
        if (sample.data.size() > 0xFFFFFFFFull) {
            return false;  // IVF frame sizes are 32-bit
        }
        ivf_push_le32(index, static_cast<std::uint32_t>(sample.data.size()));
        ivf_push_le64(index, static_cast<std::uint64_t>(sample.pts));
        ivf_push_le64(index, offset);
        offset += 12u + sample.data.size();
    }

    std::ofstream out(ivf_path + ".idx", std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(index.data()),
              static_cast<std::streamsize>(index.size()));
    out.flush();
    return out.good();
}

/* ------------------------------------------------------------------ */
/* Streaming IVF writer                                                */
/* ------------------------------------------------------------------ */

/* 32-byte file header with a placeholder frame count (patched at finish). */
inline void start_ivf_stream(FileSink& sink, const TrackConfig& track) {
    std::array<std::uint8_t, 32> header{};
    header[0] = 'D';
    header[1] = 'K';
    header[2] = 'I';
    header[3] = 'F';
    header[6] = static_cast<std::uint8_t>(32);
    const char* const fourcc = ivf_fourcc(track.codec);
    header[8] = static_cast<std::uint8_t>(fourcc[0]);
    header[9] = static_cast<std::uint8_t>(fourcc[1]);
    header[10] = static_cast<std::uint8_t>(fourcc[2]);
    header[11] = static_cast<std::uint8_t>(fourcc[3]);
    const std::uint16_t width = static_cast<std::uint16_t>(track.width);
    const std::uint16_t height = static_cast<std::uint16_t>(track.height);
    header[12] = static_cast<std::uint8_t>(width);
    header[13] = static_cast<std::uint8_t>(width >> 8u);
    header[14] = static_cast<std::uint8_t>(height);
    header[15] = static_cast<std::uint8_t>(height >> 8u);
    // Timebase: framerate = tick rate (track.timescale, default 1000),
    // timescale = 1 => frame pts written below are in 1/framerate-second ticks.
    const std::uint32_t framerate =
        track.timescale != 0u ? track.timescale : 1000u;
    const std::uint32_t timescale = 1u;
    for (unsigned i = 0; i < 4u; ++i) {
        header[16u + i] = static_cast<std::uint8_t>(framerate >> (8u * i));
    }
    for (unsigned i = 0; i < 4u; ++i) {
        header[20u + i] = static_cast<std::uint8_t>(timescale >> (8u * i));
    }
    sink.write(header);
}

/* Frame timestamps are copied verbatim from Sample::pts, in the header's
 * tick unit (see start_ivf_stream()). */
inline void append_ivf_stream_sample(FileSink& sink, const Sample& sample) {
    if (sample.data.empty() || sample.data.size() > 0xFFFFFFFFull) {
        return;
    }
    std::array<std::uint8_t, 12> fh{};
    const std::uint32_t size = static_cast<std::uint32_t>(sample.data.size());
    for (unsigned i = 0; i < 4u; ++i) {
        fh[i] = static_cast<std::uint8_t>(size >> (8u * i));
    }
    const std::uint64_t ts = static_cast<std::uint64_t>(sample.pts);
    for (unsigned i = 0; i < 8u; ++i) {
        fh[4u + i] = static_cast<std::uint8_t>(ts >> (8u * i));
    }
    sink.write(fh);
    sink.write(sample.data);
}

inline bool finish_ivf_stream(FileSink& sink, std::uint32_t frame_count) {
    std::array<std::uint8_t, 4> count{};
    for (unsigned i = 0; i < 4u; ++i) {
        count[i] = static_cast<std::uint8_t>(frame_count >> (8u * i));
    }
    return sink.patch(24u, count);
}

}  // namespace detail
}  // namespace mux
}  // namespace bs
