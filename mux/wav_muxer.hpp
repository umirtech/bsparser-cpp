// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "mux/stream.hpp"
#include "mux/file_sink.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace bs {
namespace mux {
namespace detail {

/*
 * -----------------------------------------------------------
 * WAV / RIFF PCM writer
 * -----------------------------------------------------------
 *
 * Single-stream uncompressed PCM.  Layout:
 *   "RIFF" <u32 size> "WAVE"
 *   "fmt " <u32 16> <u16 format=1> <u16 channels> <u32 rate>
 *                  <u32 byte_rate> <u16 block_align> <u16 bits>
 *   "data" <u32 size> <raw samples>
 *
 * All integers are little-endian.  Sample payloads are copied once into the
 * sink; the writer never interprets them as frames.
 */
inline void wav_push_u16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8u));
}

inline void wav_push_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8u));
    out.push_back(static_cast<std::uint8_t>(v >> 16u));
    out.push_back(static_cast<std::uint8_t>(v >> 24u));
}

[[nodiscard]]
inline bool write_wav(
    FileSink& sink, const std::vector<TrackConfig>& tracks,
    const std::vector<std::vector<Sample>>& samples
) {
    if (tracks.empty() || samples.empty()) {
        return false;
    }

    const TrackConfig& cfg = tracks.front();
    if (cfg.sample_rate == 0u || cfg.channels == 0u) {
        return false;
    }

    const unsigned bits = audio_bits_per_sample(cfg.audio_codec);
    const std::uint32_t block_align =
        static_cast<std::uint32_t>(cfg.channels) * (bits / 8u);
    const std::uint64_t byte_rate =
        static_cast<std::uint64_t>(cfg.sample_rate) * block_align;
    if (byte_rate > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    // Concatenate sample payloads first so the data chunk size is known.
    std::vector<std::uint8_t> data;
    for (const auto& s : samples.front()) {
        if (s.data.empty()) {
            continue;
        }
        if (block_align != 0u && s.data.size() % block_align != 0u) {
            return false;
        }
        data.insert(data.end(), s.data.begin(), s.data.end());
    }
    if (data.empty() || data.size() > std::numeric_limits<std::uint32_t>::max() - 44u) {
        return false;
    }

    std::vector<std::uint8_t> out;
    out.reserve(44u + data.size());

    out.insert(out.end(), {'R', 'I', 'F', 'F'});
    wav_push_u32(out, static_cast<std::uint32_t>(36u + data.size()));
    out.insert(out.end(), {'W', 'A', 'V', 'E'});

    out.insert(out.end(), {'f', 'm', 't', ' '});
    wav_push_u32(out, 16u);                     // PCM fmt chunk size
    wav_push_u16(out, 1u);                      // WAVE_FORMAT_PCM
    wav_push_u16(out, cfg.channels);
    wav_push_u32(out, cfg.sample_rate);
    wav_push_u32(out, static_cast<std::uint32_t>(byte_rate));
    wav_push_u16(out, static_cast<std::uint16_t>(block_align));
    wav_push_u16(out, static_cast<std::uint16_t>(bits));

    out.insert(out.end(), {'d', 'a', 't', 'a'});
    wav_push_u32(out, static_cast<std::uint32_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());

    sink.write(out);
    return sink.ok();
}

}  // namespace detail
}  // namespace mux
}  // namespace bs
