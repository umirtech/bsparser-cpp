// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <limits>

namespace bs {

/*
 * -----------------------------------------------------------
 * RBSP reader error
 * -----------------------------------------------------------
 */

class RbspReaderError : public std::runtime_error {
   public:
    explicit RbspReaderError(const char* message) : std::runtime_error(message) {}
};

/*
 * -----------------------------------------------------------
 * RBSP bitstream reader
 * -----------------------------------------------------------
 *
 * The input span contains EBSP bytes.
 *
 * H.264 / H.265 / H.266 emulation-prevention sequences:
 *
 *     00 00 03 xx
 *
 * are skipped logically while reading.
 *
 * No RBSP buffer is allocated.
 *
 * Example:
 *
 *     std::span<const uint8_t> ebsp = ...;
 *
 *     RbspReader reader{ebsp};
 *
 *     auto value = reader.read_bits(6);
 *
 * -----------------------------------------------------------
 */

class RbspReader {
   public:
    using byte_type = std::uint8_t;
    using span_type = std::span<const byte_type>;

   private:
    span_type data_{};

    /*
     * Physical EBSP byte position.
     */
    std::size_t byte_pos_ = 0;

    /*
     * Bit position inside the current logical RBSP byte.
     *
     * 0 = MSB
     * 7 = LSB
     */
    unsigned bit_pos_ = 0;

    /*
     * Number of consecutive zero bytes immediately preceding
     * the current physical position in the EBSP.
     *
     * Used to detect:
     *
     *     00 00 03
     */
    unsigned zero_count_ = 0;

   public:
    RbspReader() = default;

    explicit RbspReader(span_type data) noexcept : data_(data) {}

    /*
     * -------------------------------------------------------
     * Position
     * -------------------------------------------------------
     */

    [[nodiscard]]
    std::size_t byte_position() const noexcept {
        return byte_pos_;
    }

    [[nodiscard]]
    unsigned bit_position() const noexcept {
        return bit_pos_;
    }

    /*
     * Number of logical RBSP bits consumed is not simply:
     *
     *     byte_pos_ * 8 + bit_pos_
     *
     * because byte_pos_ refers to the physical EBSP.
     *
     * Keep the exact logical count separately if required by
     * callers.
     */
    [[nodiscard]]
    std::size_t physical_bit_position() const noexcept {
        return byte_pos_ * 8 + bit_pos_;
    }

    /*
     * -------------------------------------------------------
     * Input
     * -------------------------------------------------------
     */

    [[nodiscard]]
    span_type data() const noexcept {
        return data_;
    }

    [[nodiscard]]
    std::size_t size_bytes() const noexcept {
        return data_.size();
    }

    /*
     * -------------------------------------------------------
     * Bit availability
     * -------------------------------------------------------
     */

   private:
    /*
     * Determine whether the physical byte at byte_pos_
     * is an emulation-prevention byte.
     *
     * This is only meaningful when we are positioned at a byte
     * boundary.
     */
    [[nodiscard]]
    bool is_emulation_prevention_byte() const noexcept {
        if (bit_pos_ != 0) {
            return false;
        }

        if (byte_pos_ >= data_.size()) {
            return false;
        }

        if (byte_pos_ < 2) {
            return false;
        }

        return data_[byte_pos_] == 0x03 && data_[byte_pos_ - 1] == 0x00 &&
               data_[byte_pos_ - 2] == 0x00;
    }

    /*
     * Skip one emulation-prevention byte.
     */
    void skip_emulation_prevention_byte() noexcept {
        if (is_emulation_prevention_byte()) {
            ++byte_pos_;

            /*
             * The two preceding zero bytes still belong to the
             * logical RBSP stream.
             *
             * Keep zero_count_ at 2 so that the next byte can
             * update the state correctly.
             */
            zero_count_ = 2;
        }
    }

    /*
     * Move to the next logical byte.
     */
    void normalize_position() noexcept {
        if (bit_pos_ != 0) {
            return;
        }

        skip_emulation_prevention_byte();
    }

    /*
     * Check whether at least one logical bit remains.
     */
    [[nodiscard]]
    bool has_more_bits_internal() const noexcept {
        std::size_t pos = byte_pos_;

        if (bit_pos_ == 0 && pos + 2 < data_.size() && data_[pos] == 0x03 &&
            data_[pos - 1] == 0x00 && data_[pos - 2] == 0x00) {
            ++pos;
        }

        return pos < data_.size();
    }

    /*
     * Read the next logical byte.
     *
     * Must only be called at a byte boundary.
     */
    [[nodiscard]]
    std::uint8_t read_logical_byte() {
        if (bit_pos_ != 0) {
            throw RbspReaderError("RBSP reader: internal byte alignment error");
        }

        normalize_position();

        if (byte_pos_ >= data_.size()) {
            throw RbspReaderError("RBSP reader: end of RBSP");
        }

        const std::uint8_t value = data_[byte_pos_++];

        if (value == 0x00) {
            if (zero_count_ < 2) {
                ++zero_count_;
            }
        } else {
            zero_count_ = 0;
        }

        return value;
    }

   public:
    /*
     * -------------------------------------------------------
     * has_more_data
     * -------------------------------------------------------
     */

    [[nodiscard]]
    bool has_more_bits() const noexcept {
        if (bit_pos_ != 0) {
            return byte_pos_ < data_.size();
        }

        std::size_t pos = byte_pos_;

        while (pos + 2 < data_.size() && data_[pos] == 0x03 && pos >= 2 && data_[pos - 1] == 0x00 &&
               data_[pos - 2] == 0x00) {
            ++pos;
        }

        return pos < data_.size();
    }

    /*
     * -------------------------------------------------------
     * Read one bit
     * -------------------------------------------------------
     */

    [[nodiscard]]
    bool read_bit() {
        normalize_position();

        if (byte_pos_ >= data_.size()) {
            throw RbspReaderError("RBSP reader: unexpected end of RBSP");
        }

        const std::uint8_t byte = data_[byte_pos_];

        const unsigned shift = 7U - bit_pos_;

        const bool value = ((byte >> shift) & 1U) != 0;

        ++bit_pos_;

        if (bit_pos_ == 8) {
            bit_pos_ = 0;

            /*
             * Update zero tracking after consuming the byte.
             */
            if (byte == 0x00) {
                if (zero_count_ < 2) {
                    ++zero_count_;
                }
            } else {
                zero_count_ = 0;
            }

            ++byte_pos_;
        }

        return value;
    }

    /*
     * -------------------------------------------------------
     * Read N bits
     * -------------------------------------------------------
     *
     * Supports:
     *
     *     read_bits(1)
     *     read_bits(6)
     *     read_bits(16)
     *     read_bits(32)
     */

    [[nodiscard]]
    std::uint32_t read_bits(unsigned count) {
        if (count > 32) {
            throw RbspReaderError("RBSP reader: read_bits count > 32");
        }

        if (count == 0) {
            return 0;
        }

        std::uint32_t value = 0;

        for (unsigned i = 0; i < count; ++i) {
            value <<= 1;

            if (read_bit()) {
                value |= 1U;
            }
        }

        return value;
    }

    /*
     * -------------------------------------------------------
     * Unsigned Exp-Golomb
     * -------------------------------------------------------
     *
     * ue(v)
     */

    [[nodiscard]]
    std::uint32_t read_ue() {
        unsigned leading_zero_bits = 0;

        while (true) {
            if (!has_more_bits()) {
                throw RbspReaderError("RBSP reader: truncated ue(v)");
            }

            const bool bit = read_bit();

            if (bit) {
                break;
            }

            ++leading_zero_bits;

            if (leading_zero_bits >= 32) {
                throw RbspReaderError("RBSP reader: ue(v) overflow");
            }
        }

        if (leading_zero_bits == 0) {
            return 0;
        }

        const std::uint32_t suffix = read_bits(leading_zero_bits);

        return ((std::uint32_t{1} << leading_zero_bits) - 1U) + suffix;
    }

    /*
     * -------------------------------------------------------
     * Signed Exp-Golomb
     * -------------------------------------------------------
     *
     * se(v)
     */

    [[nodiscard]]
    std::int32_t read_se() {
        const std::uint32_t code_num = read_ue();

        if ((code_num & 1U) != 0) {
            /*
             * positive:
             *
             *     (code_num + 1) / 2
             */
            const std::uint32_t value = (code_num + 1U) >> 1;

            if (value > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
                throw RbspReaderError("RBSP reader: se(v) overflow");
            }

            return static_cast<std::int32_t>(value);
        }

        /*
         * negative:
         *
         *     -(code_num / 2)
         */
        const std::uint32_t value = code_num >> 1;

        if (value > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) + 1U) {
            throw RbspReaderError("RBSP reader: se(v) overflow");
        }

        if (value == 0) {
            return 0;
        }

        return -static_cast<std::int32_t>(value);
    }

    /*
     * -------------------------------------------------------
     * Byte alignment
     * -------------------------------------------------------
     */

    void byte_align() noexcept {
        if (bit_pos_ != 0) {
            bit_pos_ = 0;
            ++byte_pos_;
        }
    }

    /*
     * -------------------------------------------------------
     * Byte Position
     * -------------------------------------------------------
     */

    std::size_t position() noexcept {
        return byte_pos_;
    }

    /*
     * -------------------------------------------------------
     * rbsp_alignment_zero_bit()
     * -------------------------------------------------------
     *
     * The syntax requires:
     *
     *     bit_equal_to_one
     *     while (!byte_aligned())
     *         alignment_zero_bit
     *
     * This helper is for the common byte-alignment operation.
     */

    void rbsp_alignment() {
        const bool stop_bit = read_bit();

        if (!stop_bit) {
            throw RbspReaderError("RBSP reader: missing rbsp_stop_one_bit");
        }

        while (bit_pos_ != 0) {
            if (read_bit()) {
                throw RbspReaderError("RBSP reader: non-zero alignment bit");
            }
        }
    }

    /*
     * -------------------------------------------------------
     * rbsp_trailing_bits()
     * -------------------------------------------------------
     *
     * Equivalent to:
     *
     *     rbsp_stop_one_bit
     *
     *     while (!byte_aligned())
     *         alignment_zero_bit
     */

    void read_rbsp_trailing_bits() {
        rbsp_alignment();
    }

    /*
     * -------------------------------------------------------
     * Byte aligned state
     * -------------------------------------------------------
     */

    [[nodiscard]]
    bool byte_aligned() const noexcept {
        return bit_pos_ == 0;
    }

    /*
     * -------------------------------------------------------
     * Remaining physical bytes
     * -------------------------------------------------------
     *
     * This is a physical count, not a logical RBSP count.
     */

    [[nodiscard]]
    std::size_t remaining_physical_bytes() const noexcept {
        if (byte_pos_ >= data_.size()) {
            return 0;
        }

        return data_.size() - byte_pos_;
    }
};

/*
 * -----------------------------------------------------------
 * Factory
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline RbspReader make_rbsp_reader(std::span<const std::uint8_t> ebsp) noexcept {
    return RbspReader{ebsp};
}

}  // namespace bs