// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "es_reconstruct.hpp"
#include "stream.hpp"

#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace bs {
namespace demux {
namespace mkv {

/*
 * -----------------------------------------------------------
 * Matroska / WebM (EBML) demuxer (limited)
 * -----------------------------------------------------------
 *
 * Parses the EBML element stream, finds video tracks
 * (Tracks > TrackEntry[TrackType=1]) and indexes their
 * SimpleBlocks/Blocks from every Cluster:
 *
 *   AVC / HEVC / VVC  -> Annex-B or length-prefixed NAL spans
 *                        (parameter sets from CodecPrivate)
 *   AV1               -> OBU spans
 *   VP8 / VP9         -> raw frame spans
 *
 * Only no-lacing blocks are handled.
 */

namespace detail {

constexpr std::uint64_t kSegment = 0x18538067ull;
constexpr std::uint64_t kTracks = 0x1654AE6Bull;
constexpr std::uint64_t kTrackEntry = 0xAEull;
constexpr std::uint64_t kTrackNumber = 0xD7ull;
constexpr std::uint64_t kTrackType = 0x83ull;
constexpr std::uint64_t kCodecId = 0x86ull;
constexpr std::uint64_t kCodecPrivate = 0x63A2ull;
constexpr std::uint64_t kVideo = 0xE0ull;
constexpr std::uint64_t kPixelWidth = 0xB0ull;
constexpr std::uint64_t kPixelHeight = 0xBAull;
constexpr std::uint64_t kAudio = 0xE1ull;
constexpr std::uint64_t kSamplingFrequency = 0xB5ull;
constexpr std::uint64_t kChannels = 0x9Full;
constexpr std::uint64_t kBitDepth = 0x6264ull;
constexpr std::uint64_t kLanguage = 0x22B59Cull;
constexpr std::uint64_t kCluster = 0x1F43B675ull;
constexpr std::uint64_t kSimpleBlock = 0xA3ull;
constexpr std::uint64_t kBlockGroup = 0xA0ull;
constexpr std::uint64_t kBlock = 0xA1ull;

/*
 * Read an EBML vint (ID or size). For IDs the marker bits are
 * part of the value; for sizes they are not. Returns false when
 * the stream is exhausted.
 */
inline bool read_vint(
    std::span<const std::uint8_t> d,
    std::size_t& pos,
    std::uint64_t& value,
    std::uint64_t& length,
    bool id
) {
    if (pos >= d.size()) {
        return false;
    }

    std::uint8_t b = d[pos];

    std::size_t n = 1;
    std::uint64_t marker = 0x80;

    while (n <= 8 && (b & marker) == 0) {
        marker >>= 1;
        ++n;
    }

    if (n > 8) {
        return false;
    }

    if (pos + n > d.size()) {
        return false;
    }

    if (id) {
        value = 0;

        for (std::size_t i = 0; i < n; ++i) {
            value = (value << 8) | d[pos + i];
        }

    } else {
        value = d[pos] & (marker - 1);

        for (std::size_t i = 1; i < n; ++i) {
            value = (value << 8) | d[pos + i];
        }
    }

    length = n;

    pos += n;

    return true;
}

inline bool read_size(std::span<const std::uint8_t> d, std::size_t& pos, std::uint64_t& value) {
    std::uint64_t len = 0;
    return read_vint(d, pos, value, len, false);
}

inline bool read_id(std::span<const std::uint8_t> d, std::size_t& pos, std::uint64_t& value) {
    std::uint64_t len = 0;
    return read_vint(d, pos, value, len, true);
}

struct VideoTrack {
    bool found = false;
    std::uint64_t number = 0;
    std::uint64_t track_type = 0; /* 1=video, 2=audio, 0x11/0x12=subtitle */
    std::string codec_id;
    std::vector<std::uint8_t> codec_private;
    std::uint64_t width = 0;
    std::uint64_t height = 0;
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits_per_sample = 0;
    std::string language;
    std::uint32_t length_size = 4;
};

inline void parse_track_entry(std::span<const std::uint8_t> d, VideoTrack& track) {
    std::size_t p = 0;

    while (p < d.size()) {
        std::uint64_t id, size, id_len;

        if (!read_vint(d, p, id, id_len, true)) {
            break;
        }

        if (!read_size(d, p, size)) {
            break;
        }

        if (p + size > d.size() || size == 0) {
            break;
        }

        const auto data = d.subspan(p, static_cast<std::size_t>(size));

        switch (id) {
            case kTrackNumber:
                track.number = 0;
                for (std::uint8_t byte : data) {
                    track.number = (track.number << 8) | byte;
                }
                break;

            case kTrackType:
                if (!data.empty()) {
                    track.track_type = data[0];
                    track.found = (data[0] == 1u || data[0] == 2u ||
                                   data[0] == 0x11u || data[0] == 0x12u);
                }
                break;

            case kCodecId:
                track.codec_id.assign(reinterpret_cast<const char*>(data.data()), data.size());
                break;

            case kCodecPrivate:
                track.codec_private.assign(data.begin(), data.end());
                break;

            case kVideo: {
                std::size_t q = 0;
                while (q < data.size()) {
                    std::uint64_t vid, vsize, vlen;
                    if (!read_vint(data, q, vid, vlen, true))
                        break;
                    if (!read_size(data, q, vsize))
                        break;
                    if (q + vsize > data.size() || vsize == 0)
                        break;
                    if (vid == kPixelWidth && vsize >= 1)
                        track.width = data[q];
                    if (vid == kPixelWidth && vsize >= 2)
                        track.width = (track.width << 8) | data[q + 1];
                    if (vid == kPixelHeight && vsize >= 1)
                        track.height = data[q];
                    if (vid == kPixelHeight && vsize >= 2)
                        track.height = (track.height << 8) | data[q + 1];
                    q += vsize;
                }
                break;
            }

            case kAudio: {
                std::size_t q = 0;
                while (q < data.size()) {
                    std::uint64_t aid, asize, alen;
                    if (!read_vint(data, q, aid, alen, true)) break;
                    if (!read_size(data, q, asize)) break;
                    if (q + asize > data.size() || asize == 0) break;
                    if ((aid == kSamplingFrequency || aid == kBitDepth) && asize >= 4) {
                        std::uint64_t v = 0;
                        for (std::size_t k = 0; k < asize && k < 8; ++k) {
                            v = (v << 8) | data[q + k];
                        }
                        if (aid == kSamplingFrequency) {
                            const double dbl = std::bit_cast<double>(v);
                            if (dbl > 0.0 && dbl < 1000000.0) {
                                track.sample_rate = static_cast<std::uint32_t>(dbl);
                            }
                        } else if (v <= 0xFFFFu) {
                            track.bits_per_sample = static_cast<std::uint16_t>(v);
                        }
                    } else if (aid == kChannels && asize >= 1) {
                        std::uint64_t ch = 0;
                        for (std::size_t k = 0; k < asize && k < 4; ++k) {
                            ch = (ch << 8) | data[q + k];
                        }
                        if (ch <= 0xFFFFu) track.channels = static_cast<std::uint16_t>(ch);
                    }
                    q += asize;
                }
                break;
            }
            case kLanguage:
                track.language.assign(reinterpret_cast<const char*>(data.data()), data.size());
                break;

            default:
                break;
        }

        p += size;
    }
}

inline void parse_tracks(std::span<const std::uint8_t> d, VideoTrack& track) {
    std::size_t p = 0;

    while (p < d.size()) {
        std::uint64_t id, size, id_len;

        if (!read_vint(d, p, id, id_len, true)) {
            break;
        }

        if (!read_size(d, p, size)) {
            break;
        }

        if (p + size > d.size() || size == 0) {
            break;
        }

        if (id == kTrackEntry && !track.found) {
            parse_track_entry(d.subspan(p, static_cast<std::size_t>(size)), track);
        }

        p += size;
    }
}

/*
 * Parse a SimpleBlock / Block payload into one frame (no lacing).
 */
/*
 * Some muxers write a CodecPrivate length size
 * that disagrees with the actual block framing. Sanity-check the
 * configured length size against the first frame and fall back
 * to 4 bytes when it is implausible.
 */
inline bool parse_block(
    std::span<const std::uint8_t> d,
    std::uint64_t track_number,
    std::span<const std::uint8_t>& frame
) {
    std::size_t p = 0;

    std::uint64_t tn, tlen;

    if (!read_vint(d, p, tn, tlen, false)) {
        return false;
    }

    if (tn != track_number) {
        return false;
    }

    if (p + 3 > d.size()) {
        return false;
    }

    p += 2; /* signed 16-bit timestamp */

    const std::uint8_t flags = d[p++];

    const std::uint8_t lacing = (flags >> 1) & 0x03u;

    if (lacing != 0) {
        return false; /* limited: no lacing */
    }

    frame = d.subspan(p);

    return true;
}

}  // namespace detail

}  // namespace mkv
}  // namespace demux
}  // namespace bs
