// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/*
 * H.264 CAVLC entropy reader.
 *
 * The parser only needs syntax traversal: coefficient values are consumed to
 * advance to the next macroblock, not reconstructed.  The VLC tables and
 * their direct-index decoding forms live in avc_cavlc_tables.hpp.  Syntax
 * reads come from a cached 64-bit reservoir so a whole VLC is consumed with
 * one shift instead of one byte load per bit.
 */

#include "avc_cavlc_tables.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <stdexcept>

namespace bs {
namespace avc {

namespace detail {

[[nodiscard]]
inline std::uint64_t load_be64(const std::uint8_t* data) noexcept {
    std::uint64_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    if constexpr (std::endian::native == std::endian::little) {
#if defined(__GNUC__) || defined(__clang__)
        return __builtin_bswap64(value);
#elif defined(_MSC_VER)
        return _byteswap_uint64(value);
#else
        value = ((value & 0x00000000FFFFFFFFULL) << 32) |
                ((value & 0xFFFFFFFF00000000ULL) >> 32);
        value = ((value & 0x0000FFFF0000FFFFULL) << 16) |
                ((value & 0xFFFF0000FFFF0000ULL) >> 16);
        value = ((value & 0x00FF00FF00FF00FFULL) << 8) |
                ((value & 0xFF00FF00FF00FF00ULL) >> 8);
        return value;
#endif
    }
    return value;
}

}  // namespace detail

class CavlcError : public std::runtime_error {
   public:
    explicit CavlcError(const char* message) : std::runtime_error(message) {}
};

class CavlcReader {
   public:
    CavlcReader(std::span<const std::uint8_t> rbsp, std::size_t bit_offset)
        : rbsp_(rbsp), bit_pos_(bit_offset), total_bits_(rbsp.size() * 8) {
        if (bit_offset > total_bits_) {
            throw CavlcError("CAVLC: start offset outside slice data");
        }

        /*
         * more_rbsp_data() only needs the position of the rbsp trailing
         * stop bit; caching it turns a per-macroblock backward scan into a
         * single per-slice one.  The original scan walks from the end
         * downwards, so the cached position is the rightmost set bit (the
         * least-significant set bit of the last non-zero byte).
         */
        last_one_ = total_bits_;
        for (std::size_t i = rbsp_.size(); i > 0; --i) {
            const std::uint8_t byte = rbsp_[i - 1];
            if (byte != 0) {
                const std::size_t bit = static_cast<std::size_t>(
                        std::countr_zero(static_cast<unsigned>(byte)));
                last_one_ = (i - 1) * 8 + (7 - bit);
                break;
            }
        }
    }

    [[nodiscard]] std::size_t bit_position() const noexcept { return bit_pos_; }

    [[nodiscard]] bool byte_aligned() const noexcept { return (bit_pos_ & 7u) == 0; }

    void byte_align() noexcept {
        bit_pos_ = (bit_pos_ + 7u) & ~std::size_t{7u};
        cache_bits_ = 0;
    }

    [[nodiscard]] bool more_rbsp_data() const noexcept {
        if (bit_pos_ >= total_bits_) return false;
        /*
         * The original scan only looked at or after bit_pos_, so no set bit
         * there means "more data" (last_one_ stays total).  Comparing
         * against the cached last set bit reproduces that exactly, including
         * the last_one_ == bit_pos_ stop-bit case.
         */
        return last_one_ != bit_pos_;
    }

    [[noreturn]] static void invalid_bit_count() {
        throw CavlcError("CAVLC: invalid bit count");
    }

    /*
     * Fewer than `count` bits remain: keep the bit-by-bit path so the
     * truncation point (and the bits consumed before throwing) matches the
     * original reader.
     */
    [[nodiscard]] std::uint32_t read_bits_slow(int count) {
        std::uint32_t value = 0;
        for (int i = 0; i < count; ++i) {
            value = (value << 1) | (read_bit() ? 1u : 0u);
        }
        return value;
    }

    [[nodiscard]] std::uint32_t read_bits(int count) {
        if (count < 0 || count > 32) invalid_bit_count();
        if (count == 0) return 0;

        if (cache_bits_ < static_cast<unsigned>(count)) refill();
        if (cache_bits_ >= static_cast<unsigned>(count)) {
            const std::uint32_t value =
                    static_cast<std::uint32_t>(cache_ >> (64 - count));
            consume(count);
            return value;
        }
        return read_bits_slow(count);
    }

    [[noreturn]] static void truncated_bitstream() {
        throw CavlcError("CAVLC: truncated bitstream");
    }

    [[nodiscard]] bool read_bit() {
        if (cache_bits_ == 0) {
            refill();
            if (cache_bits_ == 0) truncated_bitstream();
        }
        const bool value = (cache_ >> 63) != 0;
        cache_ <<= 1;
        --cache_bits_;
        ++bit_pos_;
        return value;
    }

    [[noreturn]] static void ue_overflow() {
        throw CavlcError("CAVLC: ue(v) overflow");
    }

    [[nodiscard]] std::uint32_t read_ue_slow() {
        int zeros = 0;
        while (!read_bit()) {
            if (++zeros > 31) ue_overflow();
        }
        if (zeros == 0) return 0;
        return ((std::uint32_t{1} << zeros) - 1u) + read_bits(zeros);
    }

    [[nodiscard]] std::uint32_t read_ue() {
        if (cache_bits_ < 32) refill();
        if (cache_bits_ >= 32) {
            const int zeros = std::countl_zero(cache_);
            if (zeros > 31) {
                /*
                 * The original loop consumed all 32 zero bits before
                 * reporting the overflow; keep the same observation point
                 * for callers that inspect the reader after an error.
                 */
                consume(32);
                ue_overflow();
            }
            consume(zeros + 1);
            if (zeros == 0) return 0;
            return ((std::uint32_t{1} << zeros) - 1u) + read_bits(zeros);
        }
        return read_ue_slow();
    }

    [[nodiscard]] std::int32_t read_se() {
        const std::uint32_t code = read_ue();
        if ((code & 1u) != 0) {
            return static_cast<std::int32_t>((code + 1u) >> 1);
        }
        if (code == 0) return 0;
        return -static_cast<std::int32_t>(code >> 1);
    }

    [[nodiscard]] std::uint32_t read_te(std::uint32_t range) {
        if (range <= 1) return 0;
        /*
         * te(v) is a single inverted bit for range 2 and ue(v) (clamped)
         * otherwise (H.264 9.1).
         */
        if (range == 2) return read_bit() ? 0u : 1u;
        const std::uint32_t value = read_ue();
        return value >= range ? range - 1 : value;
    }

    /*
     * Decode one coeff_token.  Returns {totalCoeff, trailingOnes}.
     */
    struct CoeffToken {
        std::uint8_t total_coeff = 0;
        std::uint8_t trailing_ones = 0;
    };

    [[nodiscard]] CoeffToken decode_coeff_token(
            std::uint32_t nC,
            bool chroma_dc_420,
            bool chroma_dc_422) {
        if (chroma_dc_420) {
            return decode_coeff_table(kChromaDcCoeffTokenVlc);
        }
        if (chroma_dc_422) {
            return decode_coeff_table(kChroma422DcCoeffTokenVlc);
        }

        std::size_t table = 0;
        if (nC < 2) {
            table = 0;
        } else if (nC < 4) {
            table = 1;
        } else if (nC < 8) {
            table = 2;
        } else {
            table = 3;
        }
        return decode_coeff_table(kCoeffTokenVlc[table]);
    }

    [[nodiscard]] std::uint32_t decode_total_zeros(
            std::uint32_t total_coeff,
            bool chroma_dc_420,
            bool chroma_dc_422) {
        if (chroma_dc_420) {
            if (total_coeff < 1 || total_coeff > 3) return 0;
            return decode_vlc(kChromaDcTotalZerosVlc[total_coeff - 1]);
        }
        if (chroma_dc_422) {
            if (total_coeff < 1 || total_coeff > 7) return 0;
            return decode_vlc(kChroma422DcTotalZerosVlc[total_coeff - 1]);
        }
        if (total_coeff < 1 || total_coeff > 15) return 0;
        return decode_vlc(kTotalZerosVlc[total_coeff - 1]);
    }

    [[nodiscard]] std::uint32_t decode_run_before(std::uint32_t zeros_left) {
        if (zeros_left == 0) return 0;
        const int table = zeros_left > 6 ? 6 : static_cast<int>(zeros_left) - 1;
        return decode_vlc(kRunVlc[table]);
    }

    /*
     * Consume one residual block and return TotalCoeff.  `nC` is only used
     * for the coeff_token table.
     */
    [[nodiscard]] std::uint32_t decode_residual_block(
            std::uint32_t nC,
            int max_coeff,
            bool chroma_dc_420,
            bool chroma_dc_422) {
        const CoeffToken token =
                decode_coeff_token(nC, chroma_dc_420, chroma_dc_422);
        if (token.total_coeff == 0) return 0;

        if (token.total_coeff > static_cast<std::uint32_t>(max_coeff)) {
            throw CavlcError("CAVLC: total_coeff above max_coeff");
        }

        std::uint32_t zeros_left = 0;

        /*
         * Trailing-one signs.
         */
        for (std::uint32_t i = 0; i < token.trailing_ones; ++i) {
            (void)read_bit();
        }

        int suffix_length = 0;
        if (token.total_coeff > 10 && token.trailing_ones < 3) {
            suffix_length = 1;
        }

        const std::uint32_t levels = token.total_coeff - token.trailing_ones;
        for (std::uint32_t i = 0; i < levels; ++i) {
            /*
             * The first coded level carries an implicit +2 in levelCode when
             * TrailingOnes is less than 3 (H.264 9.3.2.2, equation 9-9).
             */
            const int level = decode_level(suffix_length,
                                           i == 0 && token.trailing_ones < 3);
            const int abs_level = level < 0 ? -level : level;
            if (suffix_length == 0) suffix_length = 1;
            if (abs_level > (3 << (suffix_length - 1)) && suffix_length < 6) {
                ++suffix_length;
            }
        }

        /*
         * total_zeros follows the levels in the residual_block_cavlc()
         * syntax (H.264 7.3.5.3.2); reading it before the levels desyncs
         * every block with a non-zero level.
         */
        if (token.total_coeff < static_cast<std::uint32_t>(max_coeff)) {
            zeros_left = decode_total_zeros(
                    token.total_coeff, chroma_dc_420, chroma_dc_422);
        }

        for (std::uint32_t i = 0; i + 1 < token.total_coeff && zeros_left > 0; ++i) {
            const std::uint32_t run = decode_run_before(zeros_left);
            if (run > zeros_left) {
                throw CavlcError("CAVLC: run_before exceeds zeros_left");
            }
            zeros_left -= run;
        }

        return token.total_coeff;
    }

    [[nodiscard]] static std::uint32_t cbp_from_code_num(
            std::uint32_t code_num,
            bool intra4x4) {
        if (code_num >= 48) return 0;
        return intra4x4 ? kIntra4x4CbpTable[code_num] : kInterCbpTable[code_num];
    }

    /*
     * 4:4:4 coded_block_pattern: the code number directly addresses the
     * 16-entry mapping and each bit gates one 8x8 group in all three
     * planes.  Returns 0xFFFFFFFF for an out-of-range code number.
     */
    [[nodiscard]] static std::uint32_t cbp_from_code_num_444(
            std::uint32_t code_num,
            bool intra4x4) {
        if (code_num >= 16) return 0xFFFFFFFFu;
        return intra4x4 ? kIntra4x4CbpTable444[code_num] : kInterCbpTable444[code_num];
    }

   private:
    [[nodiscard]] std::size_t remaining_bits() const noexcept {
        return bit_pos_ < total_bits_ ? total_bits_ - bit_pos_ : 0;
    }

    /*
     * Refill the reservoir so its most-significant bit is the next stream
     * bit.  Bits past the end of the RBSP are zero so `peek_bits()` keeps
     * the original zero-padded lookahead; `skip_bits()` still rejects any
     * code that runs past the end.
     */
    void refill() noexcept {
        if (bit_pos_ >= total_bits_) {
            cache_ = 0;
            cache_bits_ = 0;
            return;
        }

        const std::size_t byte = bit_pos_ >> 3;
        const unsigned shift = static_cast<unsigned>(bit_pos_ & 7u);
        const std::size_t remaining = total_bits_ - bit_pos_;
        std::uint64_t word = 0;

        if (byte + 9 <= rbsp_.size()) {
            word = detail::load_be64(rbsp_.data() + byte) << shift;
            if (shift != 0) {
                word |= static_cast<std::uint64_t>(rbsp_[byte + 8]) >>
                        (8 - shift);
            }
        } else {
            std::uint64_t high = 0;
            for (unsigned i = 0; i < 8; ++i) {
                const std::uint8_t value =
                        byte + i < rbsp_.size() ? rbsp_[byte + i] : 0u;
                high = (high << 8) | value;
            }
            word = high << shift;
            if (shift != 0) {
                const std::uint64_t next =
                        byte + 8 < rbsp_.size() ? rbsp_[byte + 8] : 0u;
                word |= next >> (8 - shift);
            }
        }

        if (remaining < 64) {
            word &= ~std::uint64_t{0} << (64 - remaining);
            cache_bits_ = static_cast<unsigned>(remaining);
        } else {
            cache_bits_ = 64;
        }
        cache_ = word;
    }

    void consume(unsigned count) noexcept {
        cache_ <<= count;
        cache_bits_ -= count;
        bit_pos_ += count;
    }

    [[nodiscard]] std::uint32_t peek_bits(int count) {
        if (count <= 0) return 0;
        if (cache_bits_ < static_cast<unsigned>(count)) refill();
        return static_cast<std::uint32_t>(cache_ >> (64 - count));
    }

    void skip_bits(int count) {
        if (count < 0) {
            throw CavlcError("CAVLC: truncated skip");
        }
        const std::size_t skip = static_cast<std::size_t>(count);
        if (skip > remaining_bits()) {
            throw CavlcError("CAVLC: truncated skip");
        }
        if (skip <= cache_bits_) {
            if (skip < 64) {
                cache_ <<= skip;
            } else {
                cache_ = 0;
            }
            cache_bits_ -= static_cast<unsigned>(skip);
        } else {
            cache_bits_ = 0;
        }
        bit_pos_ += skip;
    }

    /*
     * Direct-index VLC decode.  The hot path only runs when at least
     * `PrimaryBits` bits are cached, so a primary hit is guaranteed to have
     * all of its bits available and can consume them without a truncation
     * check.  End-of-RBSP and long-code cases drop into the cold helpers.
     */
    template <int PrimaryBits>
    [[nodiscard]] std::uint32_t decode_vlc(
            const CavlcVlcTable<PrimaryBits>& table) {
        if (cache_bits_ < PrimaryBits) {
            refill();
            if (cache_bits_ < PrimaryBits) return decode_vlc_tail(table);
        }

        const std::uint32_t index =
                static_cast<std::uint32_t>(cache_ >> (64 - PrimaryBits));
        const std::uint16_t entry = table.primary[index];
        if (entry == 0) return decode_vlc_long(table);

        consume(entry >> 8);
        return entry & 0xffu;
    }

    /*
     * Fallback for codes longer than the primary index.  A prefix code has
     * at most one such code per prefix, so a bounded scan of the source
     * table is enough.
     */
    template <int PrimaryBits>
    [[nodiscard]] std::uint32_t decode_vlc_long(
            const CavlcVlcTable<PrimaryBits>& table) {
        for (int i = 0; i < static_cast<int>(table.count); ++i) {
            const int length = table.lengths[i];
            if (length <= PrimaryBits) continue;
            if (peek_bits(length) == table.bits[i]) {
                if (static_cast<unsigned>(length) <= cache_bits_) {
                    consume(static_cast<unsigned>(length));
                } else {
                    skip_bits(length);
                }
                return static_cast<std::uint32_t>(i);
            }
        }
        invalid_vlc(table.count);
    }

    /*
     * Fewer than PrimaryBits remain.  The reservoir is already zero padded,
     * so the primary index is still meaningful; `skip_bits()` reports the
     * truncated code exactly like the original reader.
     */
    template <int PrimaryBits>
    [[nodiscard]] std::uint32_t decode_vlc_tail(
            const CavlcVlcTable<PrimaryBits>& table) {
        const std::uint32_t index =
                static_cast<std::uint32_t>(cache_ >> (64 - PrimaryBits));
        const std::uint16_t entry = table.primary[index];
        if (entry != 0) {
            skip_bits(entry >> 8);
            return entry & 0xffu;
        }
        return decode_vlc_long(table);
    }

    [[noreturn]] void invalid_vlc(int count) const {
        char message[96];
        std::snprintf(message, sizeof(message),
                      "CAVLC: invalid VLC code (count=%d bit=%zu)", count,
                      bit_pos_);
        throw CavlcError(message);
    }

    template <int PrimaryBits>
    [[nodiscard]] CoeffToken decode_coeff_table(
            const CavlcVlcTable<PrimaryBits>& table) {
        const std::uint32_t symbol = decode_vlc(table);
        CoeffToken token{};
        token.trailing_ones = static_cast<std::uint8_t>(symbol & 3u);
        token.total_coeff = static_cast<std::uint8_t>(symbol >> 2);
        return token;
    }

    [[nodiscard]] static int level_from_parts(
            int prefix,
            std::uint32_t suffix,
            int suffix_length,
            bool first_level) {
        int level_code = (prefix << suffix_length) + static_cast<int>(suffix);
        if (prefix == 15 && suffix_length == 0) {
            level_code += 15;
        }
        if (first_level) {
            level_code += 2;
        }

        if ((level_code & 1) == 0) {
            return (level_code >> 1) + 1;
        }
        return -((level_code + 1) >> 1);
    }

    [[noreturn]] void invalid_level_prefix() const {
        throw CavlcError("CAVLC: invalid level prefix");
    }

    [[nodiscard]] int decode_level(int suffix_length, bool first_level) {
        if (cache_bits_ < 16) {
            refill();
            if (cache_bits_ < 16) {
                return decode_level_slow(suffix_length, first_level);
            }
        }

        const int prefix = std::countl_zero(cache_);
        if (prefix > 15) {
            /*
             * Match the original loop's observation point: it consumed the
             * 16 zero bits that made up the invalid prefix before throwing.
             */
            consume(16);
            invalid_level_prefix();
        }

        int suffix_size = suffix_length;
        if (prefix == 14 && suffix_length == 0) {
            suffix_size = 4;
        } else if (prefix >= 15) {
            suffix_size = 12;
        }

        const unsigned total = static_cast<unsigned>(prefix) + 1u +
                               static_cast<unsigned>(suffix_size);
        std::uint32_t suffix = 0;
        if (suffix_size > 0) {
            if (total <= cache_bits_) {
                suffix = static_cast<std::uint32_t>(cache_ >> (64 - total)) &
                         ((std::uint32_t{1} << suffix_size) - 1u);
                consume(total);
            } else {
                consume(static_cast<unsigned>(prefix) + 1u);
                suffix = read_bits(suffix_size);
            }
        } else {
            consume(static_cast<unsigned>(prefix) + 1u);
        }
        return level_from_parts(prefix, suffix, suffix_length, first_level);
    }

    [[nodiscard]] int decode_level_slow(int suffix_length, bool first_level) {
        int prefix = 0;
        while (!read_bit()) {
            if (++prefix > 15) invalid_level_prefix();
        }

        int suffix_size = suffix_length;
        if (prefix == 14 && suffix_length == 0) {
            suffix_size = 4;
        } else if (prefix >= 15) {
            suffix_size = 12;
        }

        const std::uint32_t suffix =
                suffix_size > 0 ? read_bits(suffix_size) : 0u;
        return level_from_parts(prefix, suffix, suffix_length, first_level);
    }

    std::span<const std::uint8_t> rbsp_;
    std::size_t bit_pos_ = 0;
    std::size_t total_bits_ = 0;
    std::size_t last_one_ = 0;
    std::uint64_t cache_ = 0;
    unsigned cache_bits_ = 0;
};

}  // namespace avc
}  // namespace bs
