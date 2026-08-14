#pragma once

#include "stream.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace bs {
namespace demux {
namespace es {

/*
 * -----------------------------------------------------------
 * Shared elementary-stream reconstruction helpers
 * -----------------------------------------------------------
 */

inline void annex_b_nal(ElementaryStream& out, std::span<const std::uint8_t> nal) {
    out.bytes.push_back(0x00);
    out.bytes.push_back(0x00);
    out.bytes.push_back(0x01);
    out.bytes.insert(out.bytes.end(), nal.begin(), nal.end());
}

/*
 * Prepend the parameter-set NALs from avcC / hvcC / vvcC.
 */
inline void prepend_param_sets(
    ElementaryStream& out, std::span<const std::uint8_t> cfg, bool is_hvc_or_vvc
) {
    if (cfg.empty()) {
        return;
    }

    if (is_hvc_or_vvc) {
        std::size_t p = 19;

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

                annex_b_nal(out, cfg.subspan(p, len));

                p += len;
            }
        }

        return;
    }

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

        annex_b_nal(out, cfg.subspan(p, len));

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

        annex_b_nal(out, cfg.subspan(p, len));

        p += len;
    }
}

/*
 * Convert length-prefixed NAL units to an Annex-B stream.
 */
inline void emit_length_prefixed(
    ElementaryStream& out, std::span<const std::uint8_t> data, std::uint32_t length_size
) {
    std::size_t pos = 0;

    while (pos + length_size <= data.size()) {
        std::uint32_t nal_size = 0;

        for (std::uint32_t i = 0; i < length_size; ++i) {
            nal_size = (nal_size << 8) | data[pos + i];
        }

        pos += length_size;

        if (pos + nal_size > data.size()) {
            break;
        }

        annex_b_nal(out, data.subspan(pos, nal_size));

        pos += nal_size;
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

}  // namespace es
}  // namespace demux
}  // namespace bs
