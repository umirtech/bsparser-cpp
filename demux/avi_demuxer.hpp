// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "stream.hpp"

#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace bs {
namespace demux {
namespace avi {

/*
 * -----------------------------------------------------------
 * AVI (RIFF) demuxer (limited)
 * -----------------------------------------------------------
 *
 * Extracts the first video stream from an AVI file:
 *
 *   H.264 / HEVC  -> Annex-B (each frame treated as
 *                    length-prefixed NAL units, length 4)
 *   VP8 / VP9     -> IVF-wrapped frames
 *   AV1           -> OBU stream
 */

namespace detail {

inline std::uint32_t read_u32(std::span<const std::uint8_t> d, std::size_t p) {
    if (p + 4 > d.size()) {
        throw std::out_of_range("AVI: truncated chunk field");
    }
    /* RIFF/AVI sizes are little-endian. */
    return static_cast<std::uint32_t>(d[p]) | (static_cast<std::uint32_t>(d[p + 1]) << 8) |
           (static_cast<std::uint32_t>(d[p + 2]) << 16) |
           (static_cast<std::uint32_t>(d[p + 3]) << 24);
}

inline bool is_fourcc(std::span<const std::uint8_t> d, std::size_t p, const char* s) {
    return p + 4 <= d.size() && std::memcmp(d.data() + p, s, 4) == 0;
}

struct Chunk {
    std::size_t start = 0;
    std::size_t data = 0;
    std::size_t end = 0;
    std::size_t size = 0;
    char fourcc[5] = {0, 0, 0, 0, 0};
    bool is_list = false;
};

/*
 * AVI chunks are word-aligned: an odd-sized chunk is padded with one
 * byte.  `size` is the real data length; `end` is the aligned boundary
 * (data + size + pad) so callers can simply advance with `end`.
 */
inline Chunk chunk_at(std::span<const std::uint8_t> d, std::size_t p) {
    Chunk c;
    c.start = p;

    if (p + 8 > d.size()) {
        return c;
    }

    std::memcpy(c.fourcc, d.data() + p, 4);

    const std::uint32_t size = read_u32(d, p + 4);

    c.size = size;
    c.data = p + 8;
    c.end = c.data + size + (size & 1u);
    if (c.end > d.size()) {
        c.end = d.size();
    }

    c.is_list = std::memcmp(c.fourcc, "LIST", 4) == 0;

    return c;
}

inline void emit_nal(ElementaryStream& out, std::span<const std::uint8_t> nal) {
    out.bytes.push_back(0x00);
    out.bytes.push_back(0x00);
    out.bytes.push_back(0x01);
    out.bytes.insert(out.bytes.end(), nal.begin(), nal.end());
}

/*
 * Append one video chunk. H.26x chunks may be Annex-B (start
 * codes) or length-prefixed NAL units.
 */
inline void append_h26x_chunk(ElementaryStream& out, std::span<const std::uint8_t> frame) {
    const bool annex_b = frame.size() >= 4 && frame[0] == 0x00 && frame[1] == 0x00 &&
                         (frame[2] == 0x01 || (frame[2] == 0x00 && frame[3] == 0x01));

    if (annex_b) {
        out.bytes.insert(out.bytes.end(), frame.begin(), frame.end());
        return;
    }

    std::size_t fp = 0;

    while (fp + 4 <= frame.size()) {
        const std::uint32_t nal_size = read_u32(frame, fp);

        fp += 4;

        if (fp + nal_size > frame.size()) {
            break;
        }

        emit_nal(out, frame.subspan(fp, nal_size));

        fp += nal_size;
    }
}

inline void append_ivf_header(
    ElementaryStream& out, const char* fourcc, std::uint32_t width, std::uint32_t height
) {
    out.bytes.insert(out.bytes.end(), {'D', 'K', 'I', 'F'});
    out.bytes.insert(out.bytes.end(), {0x00, 0x00});
    out.bytes.insert(out.bytes.end(), {0x20, 0x00});
    out.bytes.insert(out.bytes.end(), fourcc, fourcc + 4);
    out.bytes.push_back(static_cast<std::uint8_t>(width & 0xFFu));
    out.bytes.push_back(static_cast<std::uint8_t>((width >> 8) & 0xFFu));
    out.bytes.push_back(static_cast<std::uint8_t>(height & 0xFFu));
    out.bytes.push_back(static_cast<std::uint8_t>((height >> 8) & 0xFFu));
    out.bytes.insert(out.bytes.end(), {0x00, 0x00, 0x00, 0x1E});
    out.bytes.insert(out.bytes.end(), {0x00, 0x00, 0x00, 0x01});
    out.bytes.insert(out.bytes.end(), {0x00, 0x00, 0x00, 0x00});
    out.bytes.insert(out.bytes.end(), {0x00, 0x00, 0x00, 0x00});
}

inline void append_ivf_frame(ElementaryStream& out, std::span<const std::uint8_t> frame) {
    const std::uint32_t size = static_cast<std::uint32_t>(frame.size());

    out.bytes.push_back(static_cast<std::uint8_t>(size & 0xFFu));
    out.bytes.push_back(static_cast<std::uint8_t>((size >> 8) & 0xFFu));
    out.bytes.push_back(static_cast<std::uint8_t>((size >> 16) & 0xFFu));
    out.bytes.push_back(static_cast<std::uint8_t>((size >> 24) & 0xFFu));

    for (int i = 0; i < 8; ++i) {
        out.bytes.push_back(0);
    }

    out.bytes.insert(out.bytes.end(), frame.begin(), frame.end());
}

}  // namespace detail

[[nodiscard]]
inline ElementaryStream demux_avi(std::span<const std::uint8_t> data) {
    ElementaryStream out;

    if (data.size() < 12 || !detail::is_fourcc(data, 0, "RIFF") ||
        !detail::is_fourcc(data, 8, "AVI ")) {
        return out;
    }

    std::string codec_fourcc;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    /*
     * Walk the top-level chunks; find hdrl and movi.
     */
    std::size_t pos = 12;
    std::size_t movi_pos = data.size();
    bool have_video_stream = false;

    while (pos + 8 <= data.size()) {
        detail::Chunk c = detail::chunk_at(data, pos);

        if (c.end <= c.start) {
            break;
        }

        if (c.is_list) {
            if (c.data + 4 <= c.end && detail::is_fourcc(data, c.data, "hdrl")) {
                /* Iterate the hdrl LIST for strl entries. */
                std::size_t p = c.data + 4;

                while (p + 8 <= c.end) {
                    detail::Chunk item = detail::chunk_at(data, p);

                    if (item.end <= p) {
                        break;
                    }

                    if (item.is_list && item.data + 4 <= item.end &&
                        detail::is_fourcc(data, item.data, "strl")) {
                        std::size_t q = item.data + 4;

                        bool in_video_strl = false;

                        while (q + 8 <= item.end) {
                            detail::Chunk sub = detail::chunk_at(data, q);

                            if (sub.end <= q) {
                                break;
                            }

                            if (detail::is_fourcc(data, q, "strh") && sub.data + 12 <= sub.end) {
                                if (detail::is_fourcc(data, sub.data, "vids")) {
                                    have_video_stream = true;
                                    in_video_strl = true;
                                    codec_fourcc = std::string(
                                        reinterpret_cast<const char*>(data.data() + sub.data + 4), 4
                                    );
                                }
                            }

                            /*
                             * Only take width/height from the video
                             * strl: an audio strf is a WAVEFORMATEX
                             * and its bytes are not dimensions.
                             */
                            if (in_video_strl && detail::is_fourcc(data, q, "strf") &&
                                sub.data + 20 <= sub.end) {
                                width = detail::read_u32(data, sub.data + 4);
                                height = detail::read_u32(data, sub.data + 8);
                            }

                            q = sub.end;
                        }
                    }

                    p = item.end;
                }
            }

            if (c.data + 4 <= c.end && detail::is_fourcc(data, c.data, "movi")) {
                movi_pos = c.data + 4;
            }
        } else {
            if (detail::is_fourcc(data, pos, "movi")) {
                movi_pos = c.data;
            }
        }

        pos = c.end;
    }

    if (!have_video_stream || codec_fourcc.empty() || movi_pos == data.size()) {
        return out;
    }

    const bool is_h26x = codec_fourcc == "H264" || codec_fourcc == "X264" ||
                         codec_fourcc == "h264" || codec_fourcc == "HEVC" ||
                         codec_fourcc == "hvc1" || codec_fourcc == "VVC" || codec_fourcc == "vvc1";

    const bool is_vp = codec_fourcc == "VP80" || codec_fourcc == "VP90";

    const bool is_av1 = codec_fourcc == "AV01";

    if (!is_h26x && !is_vp && !is_av1) {
        return out;
    }

    out.codec =
        is_h26x
            ? (codec_fourcc == "HEVC" || codec_fourcc == "hvc1"
                   ? Codec::Hevc
                   : (codec_fourcc == "VVC" || codec_fourcc == "vvc1" ? Codec::Vvc : Codec::Avc))
            : (is_vp ? (codec_fourcc == "VP90" ? Codec::Vp9 : Codec::Vp8) : Codec::Av1);

    out.width = width;
    out.height = height;
    out.codec_name = codec_fourcc;

    if (is_vp) {
        out.framing = NalFramingMode::Ivf;
        detail::append_ivf_header(out, codec_fourcc.c_str(), width, height);
    } else if (is_av1) {
        out.framing = NalFramingMode::Obu;
    } else {
        out.framing = NalFramingMode::AnnexB;
    }

    /*
     * Walk the movi LIST, collecting video chunks.
     */
    std::size_t p = movi_pos;

    while (p + 8 <= data.size()) {
        detail::Chunk c = detail::chunk_at(data, p);

        if (c.end <= p) {
            break;
        }

        if (c.is_list) {
            if (c.data + 4 <= c.end && detail::is_fourcc(data, c.data, "rec ")) {
                std::size_t q = c.data + 4;

                while (q + 8 <= c.end) {
                    detail::Chunk sub = detail::chunk_at(data, q);

                    if (sub.end <= q) {
                        break;
                    }

                    const bool is_video = detail::is_fourcc(data, q, "00dc") ||
                                          detail::is_fourcc(data, q, "00db") ||
                                          detail::is_fourcc(data, q, "00dw");

                    if (is_video && sub.data < sub.data + sub.size) {
                        const auto frame = data.subspan(sub.data, sub.size);

                        if (is_h26x) {
                            detail::append_h26x_chunk(out, frame);

                        } else if (is_vp) {
                            detail::append_ivf_frame(out, frame);

                        } else {
                            out.bytes.insert(out.bytes.end(), frame.begin(), frame.end());
                        }
                    }

                    q = sub.end;
                }
            }
        } else {
            const bool is_video = detail::is_fourcc(data, p, "00dc") ||
                                  detail::is_fourcc(data, p, "00db") ||
                                  detail::is_fourcc(data, p, "00dw");

            if (is_video && c.data < c.data + c.size) {
                const auto frame = data.subspan(c.data, c.size);

                if (is_h26x) {
                    detail::append_h26x_chunk(out, frame);

                } else if (is_vp) {
                    detail::append_ivf_frame(out, frame);

                } else {
                    out.bytes.insert(out.bytes.end(), frame.begin(), frame.end());
                }
            }
        }

        p = c.end;
    }

    if (out.bytes.empty()) {
        return out;
    }

    out.ok = true;
    return out;
}

}  // namespace avi
}  // namespace demux
}  // namespace bs
