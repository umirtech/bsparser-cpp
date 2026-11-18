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
 *   "RIFF" <u32 riff_size> "WAVE"
 *   "fmt " <u32 16|40> <fmt body>
 *   "data" <u32 data_size> <raw samples> [<u8 pad>]
 *
 * All integers are little-endian.  Sample payloads are copied once into the
 * sink; the writer never interprets them as frames.
 *
 * RIFF chunks are word-aligned: an odd-sized data chunk is followed by one
 * zero pad byte.  That pad is NOT counted in the data chunk size (which is
 * the exact raw sample byte count), but IS counted in the RIFF size:
 *   padded_data = (data_size + 1) & ~1u
 *   riff_size   = 4 ("WAVE") + 8 (fmt header) + fmt_body
 *                 + 8 (data header) + padded_data
 *               = 20 + fmt_body + padded_data     (36 + padded for PCM)
 *
 * Mono/stereo use WAVE_FORMAT_PCM (tag 1, 16-byte fmt body).  For 3+
 * channels WAVE_FORMAT_EXTENSIBLE (tag 0xFFFE, 40-byte fmt body) is used so
 * the channel mask and the PCM sub-format GUID can be carried.
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

inline constexpr std::size_t kWavPcmFmtBodyBytes = 16u;
inline constexpr std::size_t kWavExtensibleFmtBodyBytes = 40u;

/* 0 or 1: the number of zero bytes needed to word-align the data chunk. */
[[nodiscard]]
inline std::size_t wav_pad_bytes(std::size_t n) noexcept {
    return n & std::size_t{1u};
}

[[nodiscard]]
inline std::uint64_t wav_padded_size(std::uint64_t n) noexcept {
    return (n + 1u) & ~std::uint64_t{1u};
}

[[nodiscard]]
inline bool wav_valid_bits(unsigned bits) noexcept {
    return bits == 8u || bits == 16u || bits == 24u || bits == 32u;
}

[[nodiscard]]
inline std::size_t wav_fmt_body_bytes(unsigned channels) noexcept {
    return channels > 2u ? kWavExtensibleFmtBodyBytes : kWavPcmFmtBodyBytes;
}

/* Byte offset of the data chunk's u32 size field in the RIFF/WAVE header:
 * 12 (RIFF) + 8 (fmt header) + fmt body + 4 ("data") = 24 + body. */
[[nodiscard]]
inline std::size_t wav_data_size_offset(unsigned channels) noexcept {
    return 24u + wav_fmt_body_bytes(channels); // 40 classic, 64 extensible
}

/* Build the complete "fmt " chunk (8-byte header + body). */
[[nodiscard]]
inline std::vector<std::uint8_t> make_wav_fmt(
    const TrackConfig& cfg, unsigned bits,
    std::uint32_t block_align, std::uint32_t byte_rate
) {
    const bool extensible = cfg.channels > 2u;
    const std::size_t body = wav_fmt_body_bytes(cfg.channels);

    std::vector<std::uint8_t> fmt;
    fmt.reserve(8u + body);
    fmt.insert(fmt.end(), {'f', 'm', 't', ' '});
    wav_push_u32(fmt, static_cast<std::uint32_t>(body));
    wav_push_u16(fmt, extensible ? 0xFFFEu : 1u); // PCM / EXTENSIBLE
    wav_push_u16(fmt, cfg.channels);
    wav_push_u32(fmt, cfg.sample_rate);
    wav_push_u32(fmt, byte_rate);
    wav_push_u16(fmt, static_cast<std::uint16_t>(block_align));
    wav_push_u16(fmt, static_cast<std::uint16_t>(bits));

    if (extensible) {
        /* WAVEFORMATEXTENSIBLE: cbSize, validBitsPerSample,
         * dwChannelMask and the KSDATAFORMAT_SUBTYPE_PCM GUID. */
        wav_push_u16(fmt, 22u); // cbSize
        wav_push_u16(fmt, static_cast<std::uint16_t>(bits));
        const std::uint32_t channels = cfg.channels;
        const std::uint32_t mask = channels >= 32u
                                       ? 0xFFFFFFFFu
                                       : ((1u << channels) - 1u);
        wav_push_u32(fmt, mask);

        static constexpr std::uint8_t kPcmSubFormat[16] = {
            0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x10u, 0x00u,
            0x80u, 0x00u, 0x00u, 0xAAu, 0x00u, 0x38u, 0x9Bu, 0x71u
        };
        fmt.insert(fmt.end(), kPcmSubFormat, kPcmSubFormat + 16u);
    }
    return fmt;
}

/* ------------------------------------------------------------------ */
/* Streaming WAV writer                                                */
/* ------------------------------------------------------------------ */

/* RIFF/WAVE header with placeholder RIFF/data sizes (patched at finish).
 * The header is 44 bytes for PCM and 68 bytes for WAVE_FORMAT_EXTENSIBLE. */
inline bool start_wav_stream(FileSink& sink, const TrackConfig& cfg,
                             std::uint32_t& block_align_out) {
    if (cfg.sample_rate == 0u || cfg.channels == 0u) {
        return false;
    }
    const unsigned bits = audio_bits_per_sample(cfg.audio_codec);
    if (!wav_valid_bits(bits)) {
        return false;
    }
    const std::uint32_t block_align =
        static_cast<std::uint32_t>(cfg.channels) * (bits / 8u);
    const std::uint64_t byte_rate =
        static_cast<std::uint64_t>(cfg.sample_rate) * block_align;
    if (byte_rate > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    block_align_out = block_align;

    std::vector<std::uint8_t> h;
    h.reserve(12u + 8u + wav_fmt_body_bytes(cfg.channels) + 8u);
    h.insert(h.end(), {'R', 'I', 'F', 'F'});
    wav_push_u32(h, 0u); // RIFF size, patched at finish
    h.insert(h.end(), {'W', 'A', 'V', 'E'});

    const std::vector<std::uint8_t> fmt = make_wav_fmt(
        cfg, bits, block_align, static_cast<std::uint32_t>(byte_rate)
    );
    h.insert(h.end(), fmt.begin(), fmt.end());

    h.insert(h.end(), {'d', 'a', 't', 'a'});
    wav_push_u32(h, 0u); // data size, patched at finish
    sink.write(h);
    return true;
}

inline bool append_wav_stream_sample(
    FileSink& sink, const Sample& sample, std::uint32_t block_align
) {
    if (sample.data.empty()) {
        return true;
    }
    if (block_align != 0u && sample.data.size() % block_align != 0u) {
        return false;
    }
    sink.write(sample.data);
    return true;
}

/* Emit the single RIFF word-alignment pad byte, if needed.  The caller
 * invokes this after the last sample and before finish_wav_stream(). */
inline void append_wav_stream_pad(FileSink& sink, std::uint64_t data_bytes) {
    if (wav_pad_bytes(static_cast<std::size_t>(data_bytes)) == 0u) {
        return;
    }
    static constexpr std::uint8_t kZero[1] = {0u};
    sink.write(kZero);
}

/*
 * Patch RIFF and data sizes once every sample byte (and the optional pad
 * byte) has been written.  `data_bytes` is the raw, unpadded sample count;
 * the pad is emitted separately by the caller (append_wav_stream_pad()).
 *
 * When `data_size_offset` is 0 (the default) the data size field offset is
 * derived from the bytes already in the sink, which keeps both classic
 * (offset 40) and extensible (offset 64) headers correct with or without
 * the pad byte having been emitted.  An explicit non-zero offset (e.g.
 * wav_data_size_offset(channels)) overrides the derivation.  The RIFF size
 * is always 20 + fmt_body + padded, i.e. 36 + padded for PCM and 60 +
 * padded for WAVE_FORMAT_EXTENSIBLE.
 */
inline bool finish_wav_stream(FileSink& sink, std::uint64_t data_bytes,
                              std::size_t data_size_offset = 0u) {
    if (data_bytes > std::numeric_limits<std::uint32_t>::max()) {
        return false; // the data chunk size is a u32
    }
    const std::uint64_t pad = wav_pad_bytes(static_cast<std::size_t>(data_bytes));

    std::uint64_t offset = data_size_offset;
    if (offset == 0u) {
        const std::uint64_t total = sink.size();
        if (total < data_bytes + 4u) {
            return false;
        }
        // header (44 or 68 bytes) plus an optional pad byte, minus the
        // 4-byte "data" tag at its end.
        const std::uint64_t raw = total - data_bytes - 4u;
        if (raw == 40u || raw == 64u) {
            offset = raw; // no pad byte emitted
        } else if (raw == 41u || raw == 65u) {
            offset = raw - 1u; // pad byte already emitted
        } else {
            // Unknown header shape: assume the pad was emitted.
            if (total < data_bytes + pad + 44u) {
                return false;
            }
            offset = total - data_bytes - pad - 4u;
        }
    }
    if (offset < 24u) { // must leave room for RIFF and fmt chunks
        return false;
    }

    const std::uint64_t body = offset - 24u;
    const std::uint64_t riff = 20u + body + wav_padded_size(data_bytes);
    if (riff > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    std::vector<std::uint8_t> riff_bytes;
    wav_push_u32(riff_bytes, static_cast<std::uint32_t>(riff));
    std::vector<std::uint8_t> data_bytes_le;
    wav_push_u32(data_bytes_le, static_cast<std::uint32_t>(data_bytes));
    return sink.patch(4u, riff_bytes) && sink.patch(offset, data_bytes_le);
}

/* Convenience overload when the track config is at hand. */
inline bool finish_wav_stream(FileSink& sink, std::uint64_t data_bytes,
                              const TrackConfig& cfg) {
    return finish_wav_stream(sink, data_bytes, wav_data_size_offset(cfg.channels));
}

}  // namespace detail
}  // namespace mux
}  // namespace bs
