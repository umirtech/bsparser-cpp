// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

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
 * Elementary-stream helpers: parameter-set extraction and
 * length-prefixed framing detection.  Used by the streaming
 * index builders; payloads themselves are never copied here.
 * -----------------------------------------------------------
 */

inline void annex_b_nal(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> nal) {
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x01);
    out.insert(out.end(), nal.begin(), nal.end());
}

/*
 * Attempt to parse hvcC/vvcC NAL arrays starting at
 * `num_arrays_offset`. On success the parameter sets are emitted
 * and true is returned; on failure the output is rolled back.
 */
inline bool try_prepend_hvc_arrays(
    std::vector<std::uint8_t>& out, std::span<const std::uint8_t> cfg, std::size_t num_arrays_offset
) {
    if (num_arrays_offset >= cfg.size()) {
        return false;
    }

    std::size_t p = num_arrays_offset;

    const std::uint8_t num_arrays = cfg[p++];

    if (num_arrays == 0 || num_arrays > 32) {
        return false;
    }

    const std::size_t saved = out.size();

    for (std::uint8_t a = 0; a < num_arrays; ++a) {
        if (p + 3 > cfg.size()) {
            out.resize(saved);
            return false;
        }

        ++p; /* array_completeness + NAL unit type */

        const std::uint16_t num_nalus = static_cast<std::uint16_t>((cfg[p] << 8) | cfg[p + 1]);

        p += 2;

        if (num_nalus > 64) {
            out.resize(saved);
            return false;
        }

        for (std::uint16_t i = 0; i < num_nalus; ++i) {
            if (p + 2 > cfg.size()) {
                out.resize(saved);
                return false;
            }

            const std::uint16_t len = static_cast<std::uint16_t>((cfg[p] << 8) | cfg[p + 1]);

            p += 2;

            if (p + len > cfg.size()) {
                out.resize(saved);
                return false;
            }

            annex_b_nal(out, cfg.subspan(p, len));

            p += len;
        }
    }

    return true;
}

/*
 * Prepend the parameter-set NALs from avcC / hvcC / vvcC.
 */
inline void prepend_param_sets(
    std::vector<std::uint8_t>& out, std::span<const std::uint8_t> cfg, bool is_hvc_or_vvc
) {
    if (cfg.empty()) {
        return;
    }

    if (is_hvc_or_vvc) {
        /*
         * The numOfArrays field position varies between muxers
         * (and hvcC vs vvcC), so probe a small window.
         */
        for (std::size_t off = 19; off <= 23; ++off) {
            if (try_prepend_hvc_arrays(out, cfg, off)) {
                return;
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
 * True when `data` parses as a length-prefixed NAL stream that consumes
 * the entire buffer (every length is valid and the last NAL ends exactly
 * at the end).  Used to disambiguate a frame that starts with an Annex-B
 * start code from a length-prefixed frame whose first length happens to
 * be 1 (00 00 00 01).
 */
[[nodiscard]]
inline bool fully_length_prefixed(std::span<const std::uint8_t> data, std::uint32_t length_size) {
    if (data.size() < length_size) {
        return false;
    }

    std::size_t pos = 0;
    unsigned nals = 0;

    while (pos + length_size <= data.size()) {
        std::uint32_t nal_size = 0;

        for (std::uint32_t i = 0; i < length_size; ++i) {
            nal_size = (nal_size << 8) | data[pos + i];
        }

        pos += length_size;

        if (nal_size == 0 || nal_size > data.size() - pos) {
            return false;
        }

        pos += nal_size;
        ++nals;
    }

    return nals > 0 && pos == data.size();
}

}  // namespace es
}  // namespace demux
}  // namespace bs
