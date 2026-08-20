// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * Test-only MSB-first bit writer for building synthetic
 * bitstreams inline.
 */
#pragma once

#include <cstdint>
#include <vector>

namespace bstest {

struct BitWriter {
    std::vector<std::uint8_t> bytes{};
    unsigned bit_pos = 0;

    void put(std::uint32_t value, unsigned nbits) {
        for (int i = static_cast<int>(nbits) - 1; i >= 0; --i) {
            if (bit_pos == 0) {
                bytes.push_back(0);
            }
            bytes.back() |= static_cast<std::uint8_t>(((value >> i) & 1u) << (7u - bit_pos));
            bit_pos = (bit_pos + 1u) % 8u;
        }
    }

    void pad_to_byte() {
        while (bit_pos != 0) {
            put(0, 1);
        }
    }

    void ue(std::uint32_t code_num) {
        std::uint32_t lz = 0;
        std::uint32_t v = code_num + 1u;
        while (v >>= 1) {
            ++lz;
        }
        for (std::uint32_t i = 0; i < lz; ++i) {
            put(0, 1);
        }
        put(1, 1);
        if (lz != 0) {
            put(code_num + 1u - (1u << lz), lz);
        }
    }

    std::vector<std::uint8_t> take() {
        pad_to_byte();
        return bytes;
    }
};

/*
 * Annex-B frame: prepend a start code and return as bytes.
 */
inline std::vector<std::uint8_t> annex_b(const std::vector<std::uint8_t>& nals) {
    std::vector<std::uint8_t> out;
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x01);
    out.insert(out.end(), nals.begin(), nals.end());
    return out;
}

}  // namespace bstest
