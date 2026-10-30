// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/*
 * H.264 CABAC arithmetic decoder.
 *
 * The arithmetic coder is the H.264 Annex 9 algorithm and context
 * initialisation uses the spec tables in avc_cabac_tables.hpp.
 *
 * Two things keep the bin loop short:
 *
 *  - a 64-bit RBSP reservoir (`cache_`) that is refilled a word at a time.
 *    Bits are taken from the window with a variable shift, so the RBSP is
 *    bounds-checked and addressed once per refill instead of once per bit.
 *    A non-byte-aligned constructor offset simply becomes the initial
 *    `rel_` offset inside the first window.
 *
 *  - one derived 32-bit table entry per (context state, range quantiser)
 *    holding the LPS range and both successor states.  The LPS/MPS select
 *    stays a branch because CABAC bins are strongly MPS-biased and the
 *    branchy form measured faster than conditional moves; the state-0
 *    valMPS flip is folded into the table instead of a second branch.
 *
 * Renormalisation uses a direct one-shift path when range >= 128 and a
 * count-leading-zeros step otherwise, consuming the whole shift at once.
 * `bit_position()` still reports the absolute position of the next unread
 * RBSP bit: prefetched bits never advance it.
 */

#include "avc_cabac_tables.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

#if defined(__GNUC__) || defined(__clang__)
#define BS_AVC_CABAC_ALWAYS_INLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define BS_AVC_CABAC_ALWAYS_INLINE __forceinline
#else
#define BS_AVC_CABAC_ALWAYS_INLINE inline
#endif

namespace bs {
namespace avc {

class CabacError : public std::runtime_error {
   public:
    explicit CabacError(const char* message) : std::runtime_error(message) {}
};

struct CabacContext {
    std::uint8_t state = 0;  // pStateIdx, 0..63
    std::uint8_t mps = 0;    // valMPS
};

namespace detail_cabac {

struct BinEntry {
    std::uint8_t lps;       // rangeLPS for this state and range quantiser
    std::uint8_t next_lps;  // next pStateIdx after an LPS bin
    std::uint8_t next_mps;  // next pStateIdx after an MPS bin
    std::uint8_t mps_flip;  // 1 only for state 0 (LPS toggles valMPS)
};

inline constexpr auto kBinTable = [] {
    std::array<std::array<BinEntry, 4>, 64> table{};
    for (std::size_t state = 0; state < 64; ++state) {
        for (std::size_t q = 0; q < 4; ++q) {
            BinEntry& entry = table[state][q];
            entry.lps = kCabacRangeLps[state][q];
            entry.next_lps = kCabacStateTrans[state][0];
            entry.next_mps = kCabacStateTrans[state][1];
            entry.mps_flip = static_cast<std::uint8_t>(state == 0);
        }
    }
    return table;
}();

}  // namespace detail_cabac

class CabacDecoder {
   public:
    CabacDecoder(std::span<const std::uint8_t> rbsp, std::size_t bit_offset)
        : rbsp_(rbsp), bit_pos_(bit_offset) {
        if (bit_offset > rbsp.size() * 8) {
            throw CabacError("CABAC: start offset outside slice data");
        }
        range_ = 510;
        offset_ = read_bits(9);
    }

    /*
     * Initialise all contexts for one slice.  `slice_qp` is SliceQPY
     * (26 + pic_init_qp_minus26 + slice_qp_delta), clipped to 0..51.
     */
    void init_contexts(int slice_qp, int cabac_init_idc, bool i_slice) noexcept {
        const int model = i_slice ? 0 : (cabac_init_idc + 1);
        /*
         * SliceQPY can reach 51 + 6 * 6 = 87 in 14-bit streams; the
         * init formula is valid across that range and the result is
         * clipped to the CABAC state range below.
         */
        const int qp = slice_qp < 0 ? 0 : (slice_qp > 87 ? 87 : slice_qp);
        for (int i = 0; i < kCabacContextCount; ++i) {
            const int m = kCabacContextInit[i][model][0];
            const int n = kCabacContextInit[i][model][1];
            int pre = ((m * qp) >> 4) + n;
            if (pre < 1) {
                pre = 1;
            } else if (pre > 126) {
                pre = 126;
            }
            if (pre <= 63) {
                contexts_[i].state = static_cast<std::uint8_t>(63 - pre);
                contexts_[i].mps = 0;
            } else {
                contexts_[i].state = static_cast<std::uint8_t>(pre - 64);
                contexts_[i].mps = 1;
            }
        }
    }

    [[nodiscard]] CabacContext& ctx(int index) noexcept {
        return contexts_[index < 0 ? 0 : (index >= kCabacContextCount ? kCabacContextCount - 1 : index)];
    }

    [[nodiscard]] const CabacContext& ctx(int index) const noexcept {
        return contexts_[index < 0 ? 0 : (index >= kCabacContextCount ? kCabacContextCount - 1 : index)];
    }

    [[nodiscard]] BS_AVC_CABAC_ALWAYS_INLINE bool decode(CabacContext& context) {
        const auto state = static_cast<std::size_t>(context.state & 63u);
        const std::uint32_t q = (range_ >> 6) & 3u;
        const detail_cabac::BinEntry& entry = detail_cabac::kBinTable[state][q];
        const std::uint32_t lps = entry.lps;

        range_ -= lps;

        bool bin = false;
        if (offset_ >= range_) {
            offset_ -= range_;
            bin = context.mps == 0;
            context.mps = static_cast<std::uint8_t>(context.mps ^ entry.mps_flip);
            context.state = entry.next_lps;
            range_ = lps;
        } else {
            bin = context.mps != 0;
            context.state = entry.next_mps;
        }

        /*
         * Annex 9.3.3.2 renormalisation.  `range_` is never zero, so the
         * shift is 9 - bit_width(range_): zero when already in
         * [256, 510), otherwise 1..7.  A single shift is by far the most
         * common case, so it bypasses the count-leading-zeros path; the
         * rest consume the whole window in one step instead of looping.
         */
        std::uint64_t cache = cache_;
        unsigned rel = rel_;
        unsigned valid = cache_valid_;
        std::size_t pos = bit_pos_;
        if (range_ < 256u) {
            if (range_ >= 128u) {
                /*
                 * One shift is enough and covers most renormalisations, so
                 * keep it free of the leading-zero calculation.
                 */
                range_ <<= 1;
                if (rel + 1 > valid) [[unlikely]] {
                    refill(pos);
                    cache = cache_;
                    rel = rel_;
                    valid = cache_valid_;
                    if (rel + 1 > valid) {
                        const unsigned remaining = valid - rel;
                        bit_pos_ = pos + remaining;
                        rel_ = valid;
                        throw CabacError("CABAC: truncated bitstream");
                    }
                }
                offset_ = (offset_ << 1) | extract(cache, rel, 1);
                rel += 1;
                pos += 1;
            } else {
                const auto shift = static_cast<unsigned>(std::countl_zero(range_)) - 23u;
                range_ <<= shift;
                if (rel + shift > valid) [[unlikely]] {
                    refill(pos);
                    cache = cache_;
                    rel = rel_;
                    valid = cache_valid_;
                    if (rel + shift > valid) {
                        const unsigned remaining = valid - rel;
                        bit_pos_ = pos + remaining;
                        rel_ = valid;
                        throw CabacError("CABAC: truncated bitstream");
                    }
                }
                offset_ = (offset_ << shift) | extract(cache, rel, shift);
                rel += shift;
                pos += shift;
            }
        }
        cache_ = cache;
        rel_ = rel;
        cache_valid_ = valid;
        bit_pos_ = pos;
        return bin;
    }

    [[nodiscard]] bool decode_bypass() {
        offset_ = (offset_ << 1) | take_bits(1);
        if (offset_ >= range_) {
            offset_ -= range_;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool decode_terminate() {
        range_ -= 2;
        if (offset_ >= range_) {
            return true;
        }
        if (range_ < 256u) {
            range_ <<= 1;
            offset_ = (offset_ << 1) | take_bits(1);
        }
        return false;
    }

    [[nodiscard]] std::uint32_t decode_bypass_bits(int count) {
        std::uint32_t value = 0;
        for (int i = 0; i < count; ++i) {
            value = (value << 1) | (decode_bypass() ? 1u : 0u);
        }
        return value;
    }

    /*
     * UEG0 / UEG3-style unsigned Exp-Golomb with cabac contexts.  `contexts`
     * points at `context_count` contexts; the first that many bins use them,
     * then the suffix is decoded as bypass bins.
     */
    [[nodiscard]] std::uint32_t decode_ue(CabacContext* contexts, int context_count) {
        int leading_zeros = 0;
        while (true) {
            CabacContext& context = contexts[leading_zeros < context_count ? leading_zeros
                                                                           : context_count - 1];
            if (decode(context)) {
                break;
            }
            ++leading_zeros;
            if (leading_zeros > 31) {
                throw CabacError("CABAC: ue(v) overflow");
            }
        }

        if (leading_zeros == 0) {
            return 0;
        }

        const std::uint32_t suffix = decode_bypass_bits(leading_zeros);
        return ((std::uint32_t{1} << leading_zeros) - 1u) + suffix;
    }

    [[nodiscard]] std::uint32_t decode_ue(CabacContext& context) {
        return decode_ue(&context, 1);
    }

    [[nodiscard]] std::size_t bit_position() const noexcept { return bit_pos_; }

    /*
     * I_PCM support (H.264 7.3.5 and 9.3.1.2).  The arithmetic engine is
     * bypassed while the raw PCM samples are skipped, then re-initialised
     * from the first nine bits after the samples.  The context model is not
     * reset: only the arithmetic engine restarts.
     */
    void byte_align() noexcept {
        bit_pos_ = (bit_pos_ + 7u) & ~std::size_t{7u};
        drop_cache();
    }

    void skip_bits(std::uint64_t count) {
        const std::uint64_t total = static_cast<std::uint64_t>(rbsp_.size()) * 8u;
        if (bit_pos_ > total || count > total - static_cast<std::uint64_t>(bit_pos_)) {
            throw CabacError("CABAC: truncated bit skip");
        }
        bit_pos_ += static_cast<std::size_t>(count);
        drop_cache();
    }

    void restart() {
        range_ = 510;
        offset_ = read_bits(9);
    }

   private:
    static constexpr unsigned kCacheBits = 64;

    /*
     * Extract `count` bits at `rel` from the left-aligned window.  Callers
     * guarantee rel + count <= 64 and count > 0.
     */
    [[nodiscard]] static std::uint32_t extract(std::uint64_t cache,
                                               unsigned rel,
                                               unsigned count) noexcept {
        return static_cast<std::uint32_t>((cache << rel) >> (kCacheBits - count));
    }

    void drop_cache() noexcept {
        cache_ = 0;
        rel_ = 0;
        cache_valid_ = 0;
    }

    /*
     * Load a 64-bit window starting at the byte containing `pos`.  Bits
     * from `pos` sit at the top of `cache_` offset by `rel_`; `cache_valid_`
     * is the number of real RBSP bits in the window, so partial words at
     * the end of the RBSP are never treated as zero-filled data.
     */
    void refill(std::size_t pos) noexcept {
        const std::size_t base = pos & ~std::size_t{7u};
        const std::size_t byte = base >> 3;
        if (byte >= rbsp_.size()) {
            drop_cache();
            return;
        }
        const unsigned nbytes = (rbsp_.size() - byte) >= 8
                                        ? 8u
                                        : static_cast<unsigned>(rbsp_.size() - byte);
        std::uint64_t word = 0;
        for (unsigned i = 0; i < nbytes; ++i) {
            word |= static_cast<std::uint64_t>(rbsp_[byte + i]) << (56u - 8u * i);
        }
        cache_ = word;
        rel_ = static_cast<unsigned>(pos - base);
        const std::size_t available = rbsp_.size() * 8u - base;
        cache_valid_ = available < kCacheBits ? static_cast<unsigned>(available) : kCacheBits;
    }

    /*
     * Consume `count` bits, MSB first.  The hot path is one shift pair; a
     * short window calls refill(), which is the only place the RBSP is
     * addressed.  When the RBSP really ends, the consumed prefix is
     * accounted for before throwing, matching the old per-bit reader.
     */
    [[nodiscard]] BS_AVC_CABAC_ALWAYS_INLINE std::uint32_t take_bits(unsigned count) {
        if (count == 0) {
            return 0;
        }
        if (rel_ + count > cache_valid_) [[unlikely]] {
            refill(bit_pos_);
            if (rel_ + count > cache_valid_) {
                const unsigned remaining = cache_valid_ - rel_;
                bit_pos_ += remaining;
                rel_ = cache_valid_;
                throw CabacError("CABAC: truncated bitstream");
            }
        }
        const std::uint32_t value = extract(cache_, rel_, count);
        rel_ += count;
        bit_pos_ += count;
        return value;
    }

    [[nodiscard]] std::uint32_t read_bits(int count) {
        return take_bits(static_cast<unsigned>(count));
    }

    std::span<const std::uint8_t> rbsp_;
    std::size_t bit_pos_ = 0;
    std::uint64_t cache_ = 0;
    unsigned rel_ = 0;
    unsigned cache_valid_ = 0;
    std::uint32_t range_ = 510;
    std::uint32_t offset_ = 0;
    CabacContext contexts_[kCabacContextCount]{};
};

}  // namespace avc
}  // namespace bs

#undef BS_AVC_CABAC_ALWAYS_INLINE
