// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "stream.hpp"

#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace bs {
namespace demux {
namespace flv {

/*
 * -----------------------------------------------------------
 * FLV demuxer (limited)
 * -----------------------------------------------------------
 *
 * Extracts video tags from an FLV file:
 *
 *   AVC (codec_id 7)  / HEVC (12)  -> Annex-B (parameter sets
 *                                     prepended from the seq header)
 *   AV1 (codec_id 13)               -> OBU stream
 *   VP8 (10) / VP9 (11)             -> IVF-wrapped frames
 */

namespace detail {

inline std::uint32_t read_u24(std::span<const std::uint8_t> d, std::size_t p) {
    if (p + 3 > d.size()) {
        throw std::out_of_range("FLV: truncated tag field");
    }
    return (static_cast<std::uint32_t>(d[p]) << 16) | (static_cast<std::uint32_t>(d[p + 1]) << 8) |
           static_cast<std::uint32_t>(d[p + 2]);
}

inline std::uint32_t read_u32(std::span<const std::uint8_t> d, std::size_t p) {
    if (p + 4 > d.size()) {
        throw std::out_of_range("FLV: truncated tag field");
    }
    return (static_cast<std::uint32_t>(d[p]) << 24) | (static_cast<std::uint32_t>(d[p + 1]) << 16) |
           (static_cast<std::uint32_t>(d[p + 2]) << 8) | static_cast<std::uint32_t>(d[p + 3]);
}

inline void emit_annex_b_nal(ElementaryStream& out, std::span<const std::uint8_t> nal) {
    out.bytes.push_back(0x00);
    out.bytes.push_back(0x00);
    out.bytes.push_back(0x01);
    out.bytes.insert(out.bytes.end(), nal.begin(), nal.end());
}

/*
 * Parse avcC (or hvcC/vvcC) and prepend its parameter-set NALs.
 */
inline void prepend_param_sets(
    ElementaryStream& out, std::span<const std::uint8_t> cfg, bool is_hvc_or_vvc
) {
    if (cfg.empty()) {
        return;
    }

    if (is_hvc_or_vvc) {
        std::size_t p = 19; /* past the hvcC/vvcC leading fields */

        if (cfg.size() <= p) {
            return;
        }

        const std::uint8_t num_arrays = cfg[p++];

        for (std::uint8_t a = 0; a < num_arrays && p + 3 <= cfg.size(); ++a) {
            ++p;

            const std::uint16_t num_nalus = static_cast<std::uint16_t>((cfg[p] << 8) | cfg[p + 1]);

            p += 2;

            for (std::uint16_t i = 0; i < num_nalus && p + 2 <= cfg.size(); ++i) {
                const std::uint16_t len = static_cast<std::uint16_t>((cfg[p] << 8) | cfg[p + 1]);

                p += 2;

                if (p + len > cfg.size()) {
                    return;
                }

                emit_annex_b_nal(out, cfg.subspan(p, len));

                p += len;
            }
        }

        return;
    }

    /* avcC */
    if (cfg.size() < 7) {
        return;
    }

    const std::uint8_t num_sps = cfg[5] & 0x1Fu;

    std::size_t p = 6;

    for (std::uint8_t i = 0; i < num_sps && p + 2 <= cfg.size(); ++i) {
        const std::uint16_t len = static_cast<std::uint16_t>((cfg[p] << 8) | cfg[p + 1]);

        p += 2;

        if (p + len > cfg.size()) {
            return;
        }

        emit_annex_b_nal(out, cfg.subspan(p, len));

        p += len;
    }

    if (p >= cfg.size()) {
        return;
    }

    const std::uint8_t num_pps = cfg[p++];

    for (std::uint8_t i = 0; i < num_pps && p + 2 <= cfg.size(); ++i) {
        const std::uint16_t len = static_cast<std::uint16_t>((cfg[p] << 8) | cfg[p + 1]);

        p += 2;

        if (p + len > cfg.size()) {
            return;
        }

        emit_annex_b_nal(out, cfg.subspan(p, len));

        p += len;
    }
}

inline void append_ivf_header(
    ElementaryStream& out, const char* fourcc, std::uint16_t width, std::uint16_t height
) {
    out.bytes.insert(out.bytes.end(), {'D', 'K', 'I', 'F'});
    out.bytes.insert(out.bytes.end(), {0x00, 0x00});
    out.bytes.insert(out.bytes.end(), {0x20, 0x00});
    out.bytes.insert(out.bytes.end(), fourcc, fourcc + 4);
    out.bytes.push_back(static_cast<std::uint8_t>(width & 0xFFu));
    out.bytes.push_back(static_cast<std::uint8_t>(width >> 8));
    out.bytes.push_back(static_cast<std::uint8_t>(height & 0xFFu));
    out.bytes.push_back(static_cast<std::uint8_t>(height >> 8));
    out.bytes.insert(out.bytes.end(), {0x00, 0x00, 0x00, 0x1E});
    out.bytes.insert(out.bytes.end(), {0x00, 0x00, 0x00, 0x01});
    out.bytes.insert(out.bytes.end(), {0x00, 0x00, 0x00, 0x00});
    out.bytes.insert(out.bytes.end(), {0x00, 0x00, 0x00, 0x00});
}

inline void append_ivf_frame(
    ElementaryStream& out, std::span<const std::uint8_t> frame, std::uint64_t ts
) {
    const std::uint32_t size = static_cast<std::uint32_t>(frame.size());

    out.bytes.push_back(static_cast<std::uint8_t>(size & 0xFFu));
    out.bytes.push_back(static_cast<std::uint8_t>((size >> 8) & 0xFFu));
    out.bytes.push_back(static_cast<std::uint8_t>((size >> 16) & 0xFFu));
    out.bytes.push_back(static_cast<std::uint8_t>((size >> 24) & 0xFFu));

    for (int i = 0; i < 8; ++i) {
        out.bytes.push_back(static_cast<std::uint8_t>((ts >> (i * 8)) & 0xFFu));
    }

    out.bytes.insert(out.bytes.end(), frame.begin(), frame.end());
}

}  // namespace detail

[[nodiscard]]
inline ElementaryStream demux_flv(std::span<const std::uint8_t> data) {
    ElementaryStream out;

    if (data.size() < 9 || data[0] != 'F' || data[1] != 'L' || data[2] != 'V') {
        return out;
    }

    const std::uint32_t data_offset = detail::read_u32(data, 5);

    std::size_t pos = data_offset;

    /*
     * Some muxers write a leading PreviousTagSize0
     * (4 zero bytes) before the first tag, so data_offset points
     * four bytes early. Detect and skip it.
     */
    if (pos + 4 < data.size()) {
        const auto is_tag_type = [](std::uint8_t b) {
            return b == 0x08u || b == 0x09u || b == 0x12u;
        };
        if (!is_tag_type(data[pos]) && is_tag_type(data[pos + 4])) {
            pos += 4;
        }
    }

    enum class Kind { None, Avc, Hevc, Vvc, Av1, Vp8, Vp9 };

    Kind kind = Kind::None;

    bool have_annex = false;
    std::uint16_t width = 0;
    std::uint16_t height = 0;

    std::uint64_t ts = 0;

    while (pos + 11 <= data.size()) {
        const std::uint8_t tag_type = data[pos];

        const std::uint32_t tag_size = detail::read_u24(data, pos + 1);

        if (pos + 11 + tag_size > data.size()) {
            break;
        }

        const auto tag = data.subspan(pos + 11, tag_size);

        if (tag_type == 9 && tag_size >= 1) {
            const std::uint8_t codec_id = tag[0] & 0x0Fu;

            if (codec_id == 7 || codec_id == 12 || codec_id == 13) {
                /* AVC / HEVC / VVC / AV1: has a packet-type byte */
                if (tag_size < 2) {
                    pos += 11 + tag_size + 4;
                    continue;
                }

                const std::uint8_t packet_type = tag[1];

                const bool is_avc = codec_id == 7;

                /*
                 * AVC/HEVC carry a 3-byte composition time after
                 * the packet-type byte; AV1 does not.
                 */
                const std::size_t data_off = is_avc || codec_id == 12 ? 5u : 2u;

                if (data_off > tag.size()) {
                    pos += 11 + tag_size + 4;
                    continue;
                }

                const auto body = tag.subspan(data_off);

                if (packet_type == 0) {
                    /* sequence header: avcC / hvcC */
                    kind = is_avc ? Kind::Avc : (codec_id == 12 ? Kind::Hevc : Kind::Vvc);
                    detail::prepend_param_sets(out, body, !is_avc);
                    have_annex = true;

                } else if (packet_type == 1) {
                    /* length-prefixed NALUs */
                    kind = is_avc ? Kind::Avc : (codec_id == 12 ? Kind::Hevc : Kind::Vvc);

                    std::size_t p = 0;
                    while (p + 4 <= body.size()) {
                        const std::uint32_t nal_size = detail::read_u32(body, p);

                        p += 4;

                        if (p + nal_size > body.size()) {
                            break;
                        }

                        detail::emit_annex_b_nal(out, body.subspan(p, nal_size));

                        p += nal_size;
                    }
                    have_annex = true;
                }

            } else if (codec_id == 10 || codec_id == 11) {
                /* VP8 / VP9: raw frame */
                kind = codec_id == 10 ? Kind::Vp8 : Kind::Vp9;

                if (!out.ok) {
                    out.codec = kind == Kind::Vp8 ? Codec::Vp8 : Codec::Vp9;
                    out.framing = NalFramingMode::Ivf;
                    detail::append_ivf_header(
                        out, kind == Kind::Vp8 ? "VP80" : "VP90", width, height
                    );
                    out.ok = true;
                }

                detail::append_ivf_frame(out, tag.subspan(1), ts);
                ts += 1;
            }
        }

        pos += 11 + tag_size + 4; /* + PreviousTagSize */
    }

    if (!have_annex && kind == Kind::None) {
        return out;
    }

    if (kind == Kind::Avc) {
        out.codec = Codec::Avc;
        out.framing = NalFramingMode::AnnexB;
        out.ok = true;
    } else if (kind == Kind::Hevc) {
        out.codec = Codec::Hevc;
        out.framing = NalFramingMode::AnnexB;
        out.ok = true;
    } else if (kind == Kind::Vvc) {
        out.codec = Codec::Vvc;
        out.framing = NalFramingMode::AnnexB;
        out.ok = true;
    } else if (kind == Kind::Av1) {
        out.codec = Codec::Av1;
        out.framing = NalFramingMode::Obu;
        out.ok = true;
    }

    return out;
}

}  // namespace flv
}  // namespace demux
}  // namespace bs
