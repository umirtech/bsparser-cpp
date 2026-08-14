#pragma once

#include "es_reconstruct.hpp"
#include "stream.hpp"

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
 * Parses the EBML element stream, finds the first video track
 * (Tracks > TrackEntry[TrackType=1]) and extracts its
 * SimpleBlocks/Blocks from every Cluster:
 *
 *   AVC / HEVC / VVC  -> Annex-B (length-prefixed NALs,
 *                        parameter sets from CodecPrivate)
 *   AV1               -> OBU stream
 *   VP8 / VP9         -> IVF-wrapped frames
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
    std::size_t& length,
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
    std::size_t len = 0;
    return read_vint(d, pos, value, len, false);
}

inline bool read_id(std::span<const std::uint8_t> d, std::size_t& pos, std::uint64_t& value) {
    std::size_t len = 0;
    return read_vint(d, pos, value, len, true);
}

struct VideoTrack {
    bool found = false;
    std::uint64_t number = 0;
    std::string codec_id;
    std::vector<std::uint8_t> codec_private;
    std::uint64_t width = 0;
    std::uint64_t height = 0;
    std::uint32_t length_size = 4;
};

inline void parse_track_entry(std::span<const std::uint8_t> d, VideoTrack& track) {
    std::size_t p = 0;

    while (p < d.size()) {
        std::uint64_t id, size;
        std::size_t id_len;

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
                    track.found = data[0] == 1u; /* video */
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
 * Some muxers (e.g. ffmpeg) write a CodecPrivate length size
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

[[nodiscard]]
inline ElementaryStream demux_mkv(std::span<const std::uint8_t> data) {
    ElementaryStream out;

    if (data.size() < 4 || data[0] != 0x1A || data[1] != 0x45 || data[2] != 0xDF ||
        data[3] != 0xA3) {
        return out;
    }

    /*
     * Locate the Segment element (skip the EBML header).
     */
    std::size_t pos = 0;
    std::uint64_t seg_size = 0;

    while (pos + 4 <= data.size()) {
        std::uint64_t id, size, id_len;

        if (!detail::read_vint(data, pos, id, id_len, true)) {
            return out;
        }

        if (!detail::read_size(data, pos, size)) {
            return out;
        }

        if (id == detail::kSegment) {
            seg_size = size;
            break;
        }

        if (size > data.size() - pos || size == 0) {
            return out;
        }

        pos += size;
    }

    if (pos + 4 > data.size()) {
        return out;
    }

    const std::size_t seg_begin = pos;
    const std::size_t seg_end = (seg_size != 0 && seg_size < data.size() - seg_begin)
                                    ? seg_begin + static_cast<std::size_t>(seg_size)
                                    : data.size();

    detail::VideoTrack track;

    /*
     * Stream each video frame into the output as it is found
     * instead of buffering the whole stream, so large files do
     * not require a second full copy of the video data.
     */
    bool output_ready = false;
    std::uint32_t frame_ts = 0;

    auto emit_frame = [&](std::span<const std::uint8_t> frame) {
        if (!output_ready) {
            out.codec_name = track.codec_id;
            out.width = static_cast<std::uint32_t>(track.width);
            out.height = static_cast<std::uint32_t>(track.height);

            const bool avc = track.codec_id == "V_MPEG4/ISO/AVC";
            const bool hevc = track.codec_id == "V_MPEGH/ISO/HEVC";
            const bool vvc = track.codec_id == "V_VVC";
            const bool av1 = track.codec_id == "V_AV1";
            const bool vp = track.codec_id == "V_VP9" || track.codec_id == "V_VP8";

            if (avc || hevc || vvc) {
                out.codec = avc ? Codec::Avc : (hevc ? Codec::Hevc : Codec::Vvc);
                out.framing = NalFramingMode::AnnexB;

                const std::size_t lso = avc ? 4u : (hevc ? 18u : 14u);

                if (track.codec_private.size() > lso) {
                    track.length_size =
                        static_cast<std::uint32_t>(track.codec_private[lso] & 3u) + 1u;
                }

                /* sanity-check the length size against this frame */
                if (frame.size() > track.length_size) {
                    std::uint32_t fl = 0;
                    for (std::uint32_t i = 0; i < track.length_size; ++i) {
                        fl = (fl << 8) | frame[i];
                    }
                    if (fl == 0 || fl > frame.size()) {
                        track.length_size = 4;
                    }
                } else {
                    track.length_size = 4;
                }

                es::prepend_param_sets(out, track.codec_private, !avc);

            } else if (vp) {
                out.codec = track.codec_id == "V_VP9" ? Codec::Vp9 : Codec::Vp8;
                out.framing = NalFramingMode::Ivf;
                es::append_ivf_header(
                    out, track.codec_id == "V_VP9" ? "VP90" : "VP80", out.width, out.height
                );

            } else if (av1) {
                out.codec = Codec::Av1;
                out.framing = NalFramingMode::Obu;
            }

            output_ready = true;
        }

        if (track.codec_id == "V_MPEG4/ISO/AVC" || track.codec_id == "V_MPEGH/ISO/HEVC" ||
            track.codec_id == "V_VVC") {
            /*
             * Blocks are length-prefixed with ls bytes, but some muxers
             * write raw Annex-B frames (start-coded NALs).  Detect that
             * and copy them through directly.  A frame that starts with a
             * start code but also parses cleanly as length-prefixed (e.g.
             * a 1-byte first NAL) is kept on the length-prefixed path.
             */
            const bool starts_annex_b =
                frame.size() >= 4 && frame[0] == 0x00 && frame[1] == 0x00 &&
                (frame[2] == 0x01 || (frame[2] == 0x00 && frame[3] == 0x01));

            const bool annex_b =
                starts_annex_b && !es::fully_length_prefixed(frame, track.length_size);

            if (annex_b) {
                out.bytes.insert(out.bytes.end(), frame.begin(), frame.end());
            } else {
                es::emit_length_prefixed(out, frame, track.length_size);
            }
        } else if (track.codec_id == "V_VP9" || track.codec_id == "V_VP8") {
            es::append_ivf_frame(out, frame, frame_ts++);
        } else {
            out.bytes.insert(out.bytes.end(), frame.begin(), frame.end());
        }
    };

    std::size_t p = seg_begin;

    while (p + 4 <= seg_end) {
        std::uint64_t id, size, id_len;

        if (!detail::read_vint(data, p, id, id_len, true)) {
            break;
        }

        if (!detail::read_size(data, p, size)) {
            break;
        }

        if (size > seg_end - p || size == 0) {
            break;
        }

        const auto element = data.subspan(p, static_cast<std::size_t>(size));

        if (id == detail::kTracks && !track.found) {
            detail::parse_tracks(element, track);
        }

        if (id == detail::kCluster && track.found) {
            std::size_t q = 0;

            while (q + 4 <= element.size()) {
                std::uint64_t cid, csize, clen;

                if (!detail::read_vint(element, q, cid, clen, true)) {
                    break;
                }

                if (!detail::read_size(element, q, csize)) {
                    break;
                }

                if (q + csize > element.size() || csize == 0) {
                    break;
                }

                std::span<const std::uint8_t> block_data;

                if (cid == detail::kSimpleBlock) {
                    block_data = element.subspan(q, static_cast<std::size_t>(csize));
                } else if (cid == detail::kBlockGroup) {                    std::size_t b = 0;
                    std::uint64_t bid, bsize, blen;

                    if (detail::read_vint(
                            element.subspan(q, static_cast<std::size_t>(csize)), b, bid, blen, true
                        ) &&
                        detail::read_size(
                            element.subspan(q, static_cast<std::size_t>(csize)), b, bsize
                        ) &&
                        bid == detail::kBlock && b + bsize <= q + csize) {
                        block_data = element.subspan(q + b, static_cast<std::size_t>(bsize));
                    }
                }

                if (!block_data.empty()) {
                    std::span<const std::uint8_t> frame;

                    if (detail::parse_block(block_data, track.number, frame)) {
                        emit_frame(frame);
                    }
                }

                q += csize;
            }
        }

        p += size;
    }

    if (!output_ready) {
        return out;
    }

    out.ok = true;
    return out;
}

}  // namespace mkv
}  // namespace demux
}  // namespace bs
