#pragma once

#include "ivf_framer.hpp"
#include "obu_framer.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

namespace bs {

/*
 * -----------------------------------------------------------
 * NAL framing
 * -----------------------------------------------------------
 *
 * This layer understands the container/framing around HEVC
 * NAL units, but does NOT parse the NAL header itself.
 *
 * Supported formats:
 *
 *   Annex-B:
 *
 *       00 00 01
 *       00 00 00 01
 *
 *   Length-prefixed:
 *
 *       [length][NAL bytes]
 *
 * The returned NAL spans always reference the original input
 * buffer. No payload is copied.
 */

/*
 * -----------------------------------------------------------
 * Framing error
 * -----------------------------------------------------------
 */

class NalFramingError : public std::runtime_error {
   public:
    explicit NalFramingError(const char* message) : std::runtime_error(message) {}
};

/*
 * -----------------------------------------------------------
 * NAL byte span
 * -----------------------------------------------------------
 *
 * This is the complete NAL unit:
 *
 *     2-byte HEVC NAL header
 *     EBSP payload
 *
 * It does NOT include:
 *
 *     Annex-B start code
 *     length prefix
 */

using FramedNalSpan = std::span<const std::uint8_t>;

/*
 * -----------------------------------------------------------
 * Annex-B helpers
 * -----------------------------------------------------------
 */

[[nodiscard]]
constexpr bool is_annex_b_start_code_3(
    std::span<const std::uint8_t> data, std::size_t pos
) noexcept {
    return pos + 3 <= data.size() && data[pos] == 0x00 && data[pos + 1] == 0x00 &&
           data[pos + 2] == 0x01;
}

[[nodiscard]]
constexpr bool is_annex_b_start_code_4(
    std::span<const std::uint8_t> data, std::size_t pos
) noexcept {
    return pos + 4 <= data.size() && data[pos] == 0x00 && data[pos + 1] == 0x00 &&
           data[pos + 2] == 0x00 && data[pos + 3] == 0x01;
}

/*
 * Return the size of the start code at pos.
 *
 * Returns zero when there is no start code.
 *
 * Prefer the four-byte form when both could match.
 */
[[nodiscard]]
constexpr std::size_t annex_b_start_code_size(
    std::span<const std::uint8_t> data, std::size_t pos
) noexcept {
    if (is_annex_b_start_code_4(data, pos)) {
        return 4;
    }

    if (is_annex_b_start_code_3(data, pos)) {
        return 3;
    }

    return 0;
}

/*
 * -----------------------------------------------------------
 * SWAR start-code search
 * -----------------------------------------------------------
 *
 * Detects the three-byte form `00 00 01` (a four-byte
 * `00 00 00 01` start code contains it) using 8-byte lanes.
 *
 * Each byte lane is represented by its high bit (0x80). Shifts
 * are by whole lanes (8 / 16 bits), so markers never bleed
 * across byte boundaries -- the classic mistake is a 1-bit
 * shift, which merges a trailing 1-bit of one byte with the
 * leading zero-bits of the next.
 */

namespace {

/*
 * Borrow-free per-byte equality to zero.
 *
 * Returns 0x80 in each byte lane whose eight bits are all
 * zero. The left shifts are masked to discard the bits that
 * would bleed in from a neighbouring byte, so the result is
 * exact per byte (unlike the classic `v - 0x01...` haszero
 * idiom, whose borrow propagation marks the wrong lane).
 */
[[nodiscard]]
inline std::uint64_t sw_allzero(std::uint64_t v) noexcept {
    v |= (v << 1) & 0xFEFEFEFEFEFEFEFEULL;
    v |= (v << 2) & 0xFCFCFCFCFCFCFCFCULL;
    v |= (v << 4) & 0xF0F0F0F0F0F0F0F0ULL;

    return ~v & 0x8080808080808080ULL;
}

}  // namespace

/*
 * Return the index of the first start code at or after `from`,
 * or `data.size()` when none remains.
 */
[[nodiscard]]
inline std::size_t annex_b_find_start_code(
    std::span<const std::uint8_t> data, std::size_t from
) noexcept {
    const std::size_t n = data.size();

    if (from + 3 > n) {
        return n;
    }

    const std::uint8_t* p = data.data();
    std::size_t i = from;

    while (i + 8 <= n) {
        std::uint64_t x;
        std::memcpy(&x, p + i, 8);

        const std::uint64_t z = sw_allzero(x);

        const std::uint64_t o = sw_allzero(x ^ 0x0101010101010101ULL);

        /*
         * 00 00 01 ending at lane j: lane j is 0x01 and
         * lanes j-1, j-2 are 0x00. Lane shifts keep the
         * markers byte-aligned.
         */
        const std::uint64_t match = o & (z << 8) & (z << 16);

        if (match != 0) {
            const int bit = __builtin_ctzll(match);

            const int j = bit / 8;

            return i + static_cast<std::size_t>(j - 2);
        }

        i += 6;
    }

    for (; i + 3 <= n; ++i) {
        if (annex_b_start_code_size(data, i) != 0) {
            return i;
        }
    }

    return n;
}

/*
 * -----------------------------------------------------------
 * Annex-B NAL iterator
 * -----------------------------------------------------------
 *
 * This is a lightweight iterator over NAL spans.
 *
 * It deliberately does not parse the NAL header.
 */

class AnnexBNalIterator {
   private:
    std::span<const std::uint8_t> data_{};

    std::size_t current_ = 0;

    std::size_t nal_begin_ = 0;

    std::size_t nal_end_ = 0;

    bool finished_ = true;

    /*
     * Find the next start code.
     */
    [[nodiscard]]
    std::size_t find_start_code(std::size_t from) const noexcept {
        return annex_b_find_start_code(data_, from);
    }

    /*
     * Locate the next NAL after current_.
     */
    void locate_next() {
        for (;;) {
            const auto start = find_start_code(current_);

            if (start == data_.size()) {
                finished_ = true;
                nal_begin_ = data_.size();
                nal_end_ = data_.size();
                return;
            }

            const auto prefix_size = annex_b_start_code_size(data_, start);

            const auto begin = start + prefix_size;

            const auto next = find_start_code(begin);

            /*
             * Annex-B permits trailing_zero_8bits between the end
             * of a NAL and the next start code.
             *
             * Remove those trailing zero bytes from the NAL span.
             */
            std::size_t end = next;

            while (end > begin && data_[end - 1] == 0x00) {
                --end;
            }

            /*
             * Empty NAL units are not useful to the syntax layer.
             * Skip them (iteratively, to avoid unbounded recursion).
             */
            if (begin >= end) {
                current_ = next;
                continue;
            }

            nal_begin_ = begin;
            nal_end_ = end;
            current_ = next;
            finished_ = false;
            return;
        }
    }

   public:
    AnnexBNalIterator() = default;

    explicit AnnexBNalIterator(std::span<const std::uint8_t> data)
        : data_(data), current_(0), nal_begin_(0), nal_end_(0), finished_(false) {
        locate_next();
    }

    /*
     * Is another NAL available?
     */
    [[nodiscard]]
    bool valid() const noexcept {
        return !finished_;
    }

    /*
     * Current NAL.
     */
    [[nodiscard]]
    FramedNalSpan nal() const noexcept {
        if (finished_) {
            return {};
        }

        return data_.subspan(nal_begin_, nal_end_ - nal_begin_);
    }

    /*
     * Advance to the next NAL.
     */
    void next() {
        if (finished_) {
            return;
        }

        locate_next();
    }

    /*
     * Underlying source.
     */
    [[nodiscard]]
    std::span<const std::uint8_t> source() const noexcept {
        return data_;
    }
};

/*
 * -----------------------------------------------------------
 * Collect Annex-B NAL spans
 * -----------------------------------------------------------
 *
 * This does not copy NAL payloads.
 *
 * The vector contains only span descriptors.
 */

[[nodiscard]]
inline std::vector<FramedNalSpan> split_annex_b(std::span<const std::uint8_t> data) {
    std::vector<FramedNalSpan> result;

    AnnexBNalIterator iterator{data};

    while (iterator.valid()) {
        result.push_back(iterator.nal());
        iterator.next();
    }

    return result;
}

/*
 * -----------------------------------------------------------
 * Length-prefixed NAL framing
 * -----------------------------------------------------------
 *
 * Common HEVC containers use:
 *
 *     NAL length
 *     NAL bytes
 *
 * The length field is normally 1, 2, or 4 bytes.
 *
 * The caller must provide the actual length-field width.
 */

/*
 * Read a big-endian length.
 */
[[nodiscard]]
constexpr std::uint32_t read_big_endian_length(
    std::span<const std::uint8_t> data, std::size_t pos, unsigned length_size
) {
    if (length_size == 0 || length_size > 4) {
        throw NalFramingError("length-prefixed NAL: invalid length size");
    }

    if (pos + length_size > data.size()) {
        throw NalFramingError("length-prefixed NAL: truncated length");
    }

    std::uint32_t value = 0;

    for (unsigned i = 0; i < length_size; ++i) {
        value = (value << 8) | data[pos + i];
    }

    return value;
}

/*
 * -----------------------------------------------------------
 * Length-prefixed iterator
 * -----------------------------------------------------------
 */

class LengthPrefixedNalIterator {
   private:
    std::span<const std::uint8_t> data_{};

    unsigned length_size_ = 4;

    std::size_t current_ = 0;

    std::size_t nal_begin_ = 0;

    std::size_t nal_end_ = 0;

    bool finished_ = true;

    void locate_next() {
        for (;;) {
            if (current_ >= data_.size()) {
                finished_ = true;
                nal_begin_ = data_.size();
                nal_end_ = data_.size();
                return;
            }

            if (current_ + length_size_ > data_.size()) {
                throw NalFramingError("length-prefixed NAL: truncated length");
            }

            const auto length = read_big_endian_length(data_, current_, length_size_);

            const auto payload_begin = current_ + length_size_;

            const auto payload_end = payload_begin + static_cast<std::size_t>(length);

            if (payload_end > data_.size()) {
                throw NalFramingError("length-prefixed NAL: NAL exceeds input");
            }

            /*
             * Zero-length NAL units are ignored (iteratively, to
             * avoid unbounded recursion).
             */
            if (length == 0) {
                current_ = payload_end;
                continue;
            }

            nal_begin_ = payload_begin;
            nal_end_ = payload_end;

            current_ = payload_end;

            finished_ = false;
            return;
        }
    }

   public:
    LengthPrefixedNalIterator() = default;

    LengthPrefixedNalIterator(std::span<const std::uint8_t> data, unsigned length_size)
        : data_(data),
          length_size_(length_size),
          current_(0),
          nal_begin_(0),
          nal_end_(0),
          finished_(false) {
        if (length_size_ == 0 || length_size_ > 4) {
            throw NalFramingError("length-prefixed NAL: invalid length size");
        }

        locate_next();
    }

    [[nodiscard]]
    bool valid() const noexcept {
        return !finished_;
    }

    [[nodiscard]]
    FramedNalSpan nal() const noexcept {
        if (finished_) {
            return {};
        }

        return data_.subspan(nal_begin_, nal_end_ - nal_begin_);
    }

    void next() {
        if (finished_) {
            return;
        }

        locate_next();
    }

    [[nodiscard]]
    unsigned length_size() const noexcept {
        return length_size_;
    }

    [[nodiscard]]
    std::span<const std::uint8_t> source() const noexcept {
        return data_;
    }
};

/*
 * -----------------------------------------------------------
 * Collect length-prefixed NAL spans
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline std::vector<FramedNalSpan> split_length_prefixed(
    std::span<const std::uint8_t> data, unsigned length_size
) {
    std::vector<FramedNalSpan> result;

    LengthPrefixedNalIterator iterator{data, length_size};

    while (iterator.valid()) {
        result.push_back(iterator.nal());
        iterator.next();
    }

    return result;
}

/*
 * -----------------------------------------------------------
 * Unified NAL framing mode
 * -----------------------------------------------------------
 */

enum class NalFramingMode : std::uint8_t { AnnexB, LengthPrefixed, Obu, Ivf };

/*
 * -----------------------------------------------------------
 * Unified splitter
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline std::vector<FramedNalSpan> split_nal_units(
    std::span<const std::uint8_t> data, NalFramingMode mode, unsigned length_size = 4
) {
    switch (mode) {
        case NalFramingMode::AnnexB:
            return split_annex_b(data);

        case NalFramingMode::LengthPrefixed:
            return split_length_prefixed(data, length_size);

        case NalFramingMode::Obu: {
            std::vector<FramedNalSpan> out;
            av1::ObuFramer framer{data};
            while (framer.valid()) {
                out.push_back(framer.obu());
                framer.next();
            }
            return out;
        }

        case NalFramingMode::Ivf: {
            std::vector<FramedNalSpan> out;
            IvfFramer framer{data};
            while (framer.valid()) {
                out.push_back(framer.frame());
                framer.next();
            }
            return out;
        }
    }

    throw NalFramingError("NAL framing: unsupported framing mode");
}

}  // namespace bs