// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bs {

/*
 * Materialise the RBSP of one EBSP payload into `rbsp`, reusing the vector's
 * existing capacity.  The vector is cleared but never shrunk, so a parser can
 * keep one buffer for every slice of a stream and avoid a per-slice
 * allocation.
 *
 * The slice-data parsers need a contiguous byte range for CABAC/CAVLC and for
 * exact byte offsets.  This helper is only used on the opt-in motion-vector
 * path; the header-only parsers keep their zero-copy readers.
 */
inline void extract_rbsp_into(
        std::span<const std::uint8_t> ebsp,
        std::vector<std::uint8_t>& rbsp) {
    rbsp.clear();
    if (rbsp.capacity() < ebsp.size()) {
        rbsp.reserve(ebsp.size());
    }

    unsigned zero_count = 0;
    for (std::size_t i = 0; i < ebsp.size(); ++i) {
        const std::uint8_t byte = ebsp[i];

        if (zero_count >= 2 && byte == 0x03) {
            const std::uint8_t next = (i + 1 < ebsp.size()) ? ebsp[i + 1] : 0x00;
            if (next <= 0x03) {
                zero_count = 0;
                continue;
            }
        }

        rbsp.push_back(byte);
        zero_count = (byte == 0x00) ? zero_count + 1 : 0;
    }
}

/* Convenience wrapper for callers that want an owning buffer. */
[[nodiscard]]
inline std::vector<std::uint8_t> extract_rbsp(
        std::span<const std::uint8_t> ebsp) {
    std::vector<std::uint8_t> rbsp;
    extract_rbsp_into(ebsp, rbsp);
    return rbsp;
}

} // namespace bs
