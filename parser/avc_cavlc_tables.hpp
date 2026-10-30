// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause
//
// H.264 CAVLC VLC tables.
//
// Values are the tables published in ITU-T H.264 (Tables 9-5 .. 9-10 and
// 7-11/7-13/7-14).  They are transcribed from the public FFmpeg h264_cavlc.c
// tables, which are a spec transcription; only the numeric data is kept here.

#pragma once

#include <array>
#include <cstdint>

namespace bs {
namespace avc {

inline constexpr std::uint8_t kCoeffTokenLen[4][68] = {
    1, 0, 0, 0, 6, 2, 0, 0, 8, 6, 3, 0, 9, 8, 7, 5,
    10, 9, 8, 6, 11, 10, 9, 7, 13, 11, 10, 8, 13, 13, 11, 9,
    13, 13, 13, 10, 14, 14, 13, 11, 14, 14, 14, 13, 15, 15, 14, 14,
    15, 15, 15, 14, 16, 15, 15, 15, 16, 16, 16, 15, 16, 16, 16, 16,
    16, 16, 16, 16, 2, 0, 0, 0, 6, 2, 0, 0, 6, 5, 3, 0,
    7, 6, 6, 4, 8, 6, 6, 4, 8, 7, 7, 5, 9, 8, 8, 6,
    11, 9, 9, 6, 11, 11, 11, 7, 12, 11, 11, 9, 12, 12, 12, 11,
    12, 12, 12, 11, 13, 13, 13, 12, 13, 13, 13, 13, 13, 14, 13, 13,
    14, 14, 14, 13, 14, 14, 14, 14, 4, 0, 0, 0, 6, 4, 0, 0,
    6, 5, 4, 0, 6, 5, 5, 4, 7, 5, 5, 4, 7, 5, 5, 4,
    7, 6, 6, 4, 7, 6, 6, 4, 8, 7, 7, 5, 8, 8, 7, 6,
    9, 8, 8, 7, 9, 9, 8, 8, 9, 9, 9, 8, 10, 9, 9, 9,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 6, 0, 0, 0,
    6, 6, 0, 0, 6, 6, 6, 0, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
};

inline constexpr std::uint8_t kCoeffTokenBits[4][68] = {
    1, 0, 0, 0, 5, 1, 0, 0, 7, 4, 1, 0, 7, 6, 5, 3,
    7, 6, 5, 3, 7, 6, 5, 4, 15, 6, 5, 4, 11, 14, 5, 4,
    8, 10, 13, 4, 15, 14, 9, 4, 11, 10, 13, 12, 15, 14, 9, 12,
    11, 10, 13, 8, 15, 1, 9, 12, 11, 14, 13, 8, 7, 10, 9, 12,
    4, 6, 5, 8, 3, 0, 0, 0, 11, 2, 0, 0, 7, 7, 3, 0,
    7, 10, 9, 5, 7, 6, 5, 4, 4, 6, 5, 6, 7, 6, 5, 8,
    15, 6, 5, 4, 11, 14, 13, 4, 15, 10, 9, 4, 11, 14, 13, 12,
    8, 10, 9, 8, 15, 14, 13, 12, 11, 10, 9, 12, 7, 11, 6, 8,
    9, 8, 10, 1, 7, 6, 5, 4, 15, 0, 0, 0, 15, 14, 0, 0,
    11, 15, 13, 0, 8, 12, 14, 12, 15, 10, 11, 11, 11, 8, 9, 10,
    9, 14, 13, 9, 8, 10, 9, 8, 15, 14, 13, 13, 11, 14, 10, 12,
    15, 10, 13, 12, 11, 14, 9, 12, 8, 10, 13, 8, 13, 7, 9, 12,
    9, 12, 11, 10, 5, 8, 7, 6, 1, 4, 3, 2, 3, 0, 0, 0,
    0, 1, 0, 0, 4, 5, 6, 0, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
};

inline constexpr std::uint8_t kChromaDcCoeffTokenLen[20] = {
    2, 0, 0, 0, 6, 1, 0, 0, 6, 6, 3, 0, 6, 7, 7, 6,
    6, 8, 8, 7,
};

inline constexpr std::uint8_t kChromaDcCoeffTokenBits[20] = {
    1, 0, 0, 0, 7, 1, 0, 0, 4, 6, 1, 0, 3, 3, 2, 5,
    2, 3, 2, 0,
};

inline constexpr std::uint8_t kChroma422DcCoeffTokenLen[36] = {
    1, 0, 0, 0, 7, 2, 0, 0, 7, 7, 3, 0, 9, 7, 7, 5,
    9, 9, 7, 6, 10, 10, 9, 7, 11, 11, 10, 7, 12, 12, 11, 10,
    13, 12, 12, 11,
};

inline constexpr std::uint8_t kChroma422DcCoeffTokenBits[36] = {
    1, 0, 0, 0, 15, 1, 0, 0, 14, 13, 1, 0, 7, 12, 11, 1,
    6, 5, 10, 1, 7, 6, 4, 9, 7, 6, 5, 8, 7, 6, 5, 4,
    7, 5, 4, 4,
};

inline constexpr std::uint8_t kTotalZerosLen[16][16] = {
    1, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 9,
    3, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 6, 6, 6, 6, 0,
    4, 3, 3, 3, 4, 4, 3, 3, 4, 5, 5, 6, 5, 6, 0, 0,
    5, 3, 4, 4, 3, 3, 3, 4, 3, 4, 5, 5, 5, 0, 0, 0,
    4, 4, 4, 3, 3, 3, 3, 3, 4, 5, 4, 5, 0, 0, 0, 0,
    6, 5, 3, 3, 3, 3, 3, 3, 4, 3, 6, 0, 0, 0, 0, 0,
    6, 5, 3, 3, 3, 2, 3, 4, 3, 6, 0, 0, 0, 0, 0, 0,
    6, 4, 5, 3, 2, 2, 3, 3, 6, 0, 0, 0, 0, 0, 0, 0,
    6, 6, 4, 2, 2, 3, 2, 5, 0, 0, 0, 0, 0, 0, 0, 0,
    5, 5, 3, 2, 2, 2, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    4, 4, 3, 3, 1, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    4, 4, 2, 1, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    3, 3, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

inline constexpr std::uint8_t kTotalZerosBits[16][16] = {
    1, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 1,
    7, 6, 5, 4, 3, 5, 4, 3, 2, 3, 2, 3, 2, 1, 0, 0,
    5, 7, 6, 5, 4, 3, 4, 3, 2, 3, 2, 1, 1, 0, 0, 0,
    3, 7, 5, 4, 6, 5, 4, 3, 3, 2, 2, 1, 0, 0, 0, 0,
    5, 4, 3, 7, 6, 5, 4, 3, 2, 1, 1, 0, 0, 0, 0, 0,
    1, 1, 7, 6, 5, 4, 3, 2, 1, 1, 0, 0, 0, 0, 0, 0,
    1, 1, 5, 4, 3, 3, 2, 1, 1, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 3, 3, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 0, 1, 3, 2, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 0, 1, 3, 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 1, 2, 1, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

inline constexpr std::uint8_t kChromaDcTotalZerosLen[3][4] = {
    1, 2, 3, 3, 1, 2, 2, 0, 1, 1, 0, 0,
};

inline constexpr std::uint8_t kChromaDcTotalZerosBits[3][4] = {
    1, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0,
};

inline constexpr std::uint8_t kChroma422DcTotalZerosLen[7][8] = {
    1, 3, 3, 4, 4, 4, 5, 5, 3, 2, 3, 3, 3, 3, 3, 0,
    3, 3, 2, 2, 3, 3, 0, 0, 3, 2, 2, 2, 3, 0, 0, 0,
    2, 2, 2, 2, 0, 0, 0, 0, 2, 2, 1, 0, 0, 0, 0, 0,
    1, 1, 0, 0, 0, 0, 0, 0,
};

inline constexpr std::uint8_t kChroma422DcTotalZerosBits[7][8] = {
    1, 2, 3, 2, 3, 1, 1, 0, 0, 1, 1, 4, 5, 6, 7, 0,
    0, 1, 1, 2, 6, 7, 0, 0, 6, 0, 1, 2, 7, 0, 0, 0,
    0, 1, 2, 3, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0,
    0, 1, 0, 0, 0, 0, 0, 0,
};

inline constexpr std::uint8_t kRunLen[7][16] = {
    1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    2, 2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    2, 2, 2, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    2, 2, 3, 3, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    2, 3, 3, 3, 3, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    3, 3, 3, 3, 3, 3, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0,
};

inline constexpr std::uint8_t kRunBits[7][16] = {
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    3, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    3, 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    3, 2, 3, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    3, 0, 1, 3, 2, 5, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    7, 6, 5, 4, 3, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0,
};

/*
 * Direct-index decoding tables generated from the VLC tables above.
 *
 * `primary` has one entry per `PrimaryBits`-bit prefix.  A non-zero entry
 * packs the code length in the high byte and the symbol in the low byte.
 * Zero marks a prefix that only belongs to a code longer than
 * `PrimaryBits`; those codes are resolved by scanning the source table
 * (a prefix code has at most one such candidate per prefix).
 *
 * 9 bits cover the coeff_token families; 8 bits are enough for
 * total_zeros and run_before, whose codes are shorter and whose tables
 * are looked up far more often.  The smaller tables keep the whole
 * lookup set close to L1d; the long-code fallback only fires for about
 * 1% of the codes in a typical stream.
 */
template <int PrimaryBits>
struct CavlcVlcTable {
    std::array<std::uint16_t, (std::size_t{1} << PrimaryBits)> primary{};
    const std::uint8_t* lengths = nullptr;
    const std::uint8_t* bits = nullptr;
    std::uint16_t count = 0;
};

template <int PrimaryBits>
[[nodiscard]] constexpr CavlcVlcTable<PrimaryBits> make_cavlc_vlc_table(
        const std::uint8_t* lengths,
        const std::uint8_t* bits,
        int count) {
    CavlcVlcTable<PrimaryBits> table{};
    table.lengths = lengths;
    table.bits = bits;
    table.count = static_cast<std::uint16_t>(count);

    for (int symbol = 0; symbol < count; ++symbol) {
        const int length = lengths[symbol];
        if (length <= 0 || length > PrimaryBits) continue;

        const std::uint32_t base =
                static_cast<std::uint32_t>(bits[symbol])
                << (PrimaryBits - length);
        const std::uint32_t span = std::uint32_t{1} << (PrimaryBits - length);
        const std::uint16_t entry =
                static_cast<std::uint16_t>((length << 8) | symbol);
        for (std::uint32_t i = 0; i < span; ++i) {
            if (table.primary[base + i] == 0) table.primary[base + i] = entry;
        }
    }
    return table;
}

inline constexpr CavlcVlcTable<9> kCoeffTokenVlc[4] = {
    make_cavlc_vlc_table<9>(kCoeffTokenLen[0], kCoeffTokenBits[0], 68),
    make_cavlc_vlc_table<9>(kCoeffTokenLen[1], kCoeffTokenBits[1], 68),
    make_cavlc_vlc_table<9>(kCoeffTokenLen[2], kCoeffTokenBits[2], 68),
    make_cavlc_vlc_table<9>(kCoeffTokenLen[3], kCoeffTokenBits[3], 68),
};

inline constexpr CavlcVlcTable<9> kChromaDcCoeffTokenVlc =
        make_cavlc_vlc_table<9>(kChromaDcCoeffTokenLen, kChromaDcCoeffTokenBits, 20);

inline constexpr CavlcVlcTable<9> kChroma422DcCoeffTokenVlc =
        make_cavlc_vlc_table<9>(kChroma422DcCoeffTokenLen,
                                 kChroma422DcCoeffTokenBits, 36);

/*
 * total_zeros rows are indexed by TotalCoeff - 1 and carry 17 - TotalCoeff
 * codes, so each row gets its own count-exact primary table.
 */
inline constexpr CavlcVlcTable<8> kTotalZerosVlc[15] = {
    make_cavlc_vlc_table<8>(kTotalZerosLen[0], kTotalZerosBits[0], 16),
    make_cavlc_vlc_table<8>(kTotalZerosLen[1], kTotalZerosBits[1], 15),
    make_cavlc_vlc_table<8>(kTotalZerosLen[2], kTotalZerosBits[2], 14),
    make_cavlc_vlc_table<8>(kTotalZerosLen[3], kTotalZerosBits[3], 13),
    make_cavlc_vlc_table<8>(kTotalZerosLen[4], kTotalZerosBits[4], 12),
    make_cavlc_vlc_table<8>(kTotalZerosLen[5], kTotalZerosBits[5], 11),
    make_cavlc_vlc_table<8>(kTotalZerosLen[6], kTotalZerosBits[6], 10),
    make_cavlc_vlc_table<8>(kTotalZerosLen[7], kTotalZerosBits[7], 9),
    make_cavlc_vlc_table<8>(kTotalZerosLen[8], kTotalZerosBits[8], 8),
    make_cavlc_vlc_table<8>(kTotalZerosLen[9], kTotalZerosBits[9], 7),
    make_cavlc_vlc_table<8>(kTotalZerosLen[10], kTotalZerosBits[10], 6),
    make_cavlc_vlc_table<8>(kTotalZerosLen[11], kTotalZerosBits[11], 5),
    make_cavlc_vlc_table<8>(kTotalZerosLen[12], kTotalZerosBits[12], 4),
    make_cavlc_vlc_table<8>(kTotalZerosLen[13], kTotalZerosBits[13], 3),
    make_cavlc_vlc_table<8>(kTotalZerosLen[14], kTotalZerosBits[14], 2),
};

inline constexpr CavlcVlcTable<8> kChromaDcTotalZerosVlc[3] = {
    make_cavlc_vlc_table<8>(kChromaDcTotalZerosLen[0], kChromaDcTotalZerosBits[0], 4),
    make_cavlc_vlc_table<8>(kChromaDcTotalZerosLen[1], kChromaDcTotalZerosBits[1], 3),
    make_cavlc_vlc_table<8>(kChromaDcTotalZerosLen[2], kChromaDcTotalZerosBits[2], 2),
};

inline constexpr CavlcVlcTable<8> kChroma422DcTotalZerosVlc[7] = {
    make_cavlc_vlc_table<8>(kChroma422DcTotalZerosLen[0],
                            kChroma422DcTotalZerosBits[0], 8),
    make_cavlc_vlc_table<8>(kChroma422DcTotalZerosLen[1],
                            kChroma422DcTotalZerosBits[1], 7),
    make_cavlc_vlc_table<8>(kChroma422DcTotalZerosLen[2],
                            kChroma422DcTotalZerosBits[2], 6),
    make_cavlc_vlc_table<8>(kChroma422DcTotalZerosLen[3],
                            kChroma422DcTotalZerosBits[3], 5),
    make_cavlc_vlc_table<8>(kChroma422DcTotalZerosLen[4],
                            kChroma422DcTotalZerosBits[4], 4),
    make_cavlc_vlc_table<8>(kChroma422DcTotalZerosLen[5],
                            kChroma422DcTotalZerosBits[5], 3),
    make_cavlc_vlc_table<8>(kChroma422DcTotalZerosLen[6],
                            kChroma422DcTotalZerosBits[6], 2),
};

inline constexpr CavlcVlcTable<8> kRunVlc[7] = {
    make_cavlc_vlc_table<8>(kRunLen[0], kRunBits[0], 2),
    make_cavlc_vlc_table<8>(kRunLen[1], kRunBits[1], 3),
    make_cavlc_vlc_table<8>(kRunLen[2], kRunBits[2], 4),
    make_cavlc_vlc_table<8>(kRunLen[3], kRunBits[3], 5),
    make_cavlc_vlc_table<8>(kRunLen[4], kRunBits[4], 6),
    make_cavlc_vlc_table<8>(kRunLen[5], kRunBits[5], 7),
    make_cavlc_vlc_table<8>(kRunLen[6], kRunBits[6], 15),
};


inline constexpr std::uint8_t kIntra4x4CbpTable[48] = {
    47, 31, 15, 0, 23, 27, 29, 30, 7, 11, 13, 14, 39, 43, 45, 46,
    16, 3, 5, 10, 12, 19, 21, 26, 28, 35, 37, 42, 44, 1, 2, 4,
    8, 17, 18, 20, 24, 6, 9, 22, 25, 32, 33, 34, 36, 40, 38, 41,
};

inline constexpr std::uint8_t kInterCbpTable[48] = {
    0, 16, 1, 2, 4, 8, 32, 3, 5, 10, 12, 15, 47, 7, 11, 13,
    14, 6, 9, 31, 35, 37, 42, 44, 33, 34, 36, 40, 39, 43, 45, 46,
    17, 18, 20, 24, 19, 21, 26, 28, 23, 27, 29, 30, 22, 25, 38, 41,
};

/*
 * 4:4:4 coded_block_pattern uses the 16-entry mapping: one CBP bit per
 * 8x8 group is shared by all three planes (H.264 Tables 7-13 and 9-4).
 */
inline constexpr std::uint8_t kIntra4x4CbpTable444[16] = {
    15, 0, 7, 11, 13, 14, 3, 5, 10, 12, 1, 2, 4, 8, 6, 9,
};

inline constexpr std::uint8_t kInterCbpTable444[16] = {
    0, 1, 2, 4, 8, 3, 5, 10, 12, 15, 7, 11, 13, 14, 6, 9,
};

}  // namespace avc
}  // namespace bs
