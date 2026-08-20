// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace bs {

/*
 * Generic MSB-first bitstream reader.
 *
 * The reader never owns the input.
 *
 * All reads consume bits from:
 *
 *     bit 7 -> bit 0
 *
 * of each byte.
 *
 * Example:
 *
 *     byte = 10110010
 *            ^
 *            first bit read
 */
class BitstreamReader {
   public:
    using byte_type = std::byte;

    explicit BitstreamReader(std::span<const byte_type> data) noexcept : data_(data) {}

    /*
     * -------------------------------------------------------
     * Position
     * -------------------------------------------------------
     */

    [[nodiscard]]
    constexpr std::size_t bit_position() const noexcept {
        return bit_position_;
    }

    [[nodiscard]]
    constexpr std::size_t byte_position() const noexcept {
        return bit_position_ / 8;
    }

    [[nodiscard]]
    constexpr std::size_t bits_remaining() const noexcept {
        const std::size_t total_bits = data_.size() * 8;

        if (bit_position_ >= total_bits) {
            return 0;
        }

        return total_bits - bit_position_;
    }

    [[nodiscard]]
    constexpr bool empty() const noexcept {
        return bits_remaining() == 0;
    }

    [[nodiscard]]
    constexpr bool byte_aligned() const noexcept {
        return (bit_position_ & 7u) == 0;
    }

    [[nodiscard]]
    constexpr std::span<const byte_type> data() const noexcept {
        return data_;
    }

    /*
     * -------------------------------------------------------
     * Position manipulation
     * -------------------------------------------------------
     */

    void skip_bits(std::size_t count) {
        require_bits(count);

        bit_position_ += count;
    }

    void align_to_byte() {
        const auto remainder = bit_position_ & 7u;

        if (remainder != 0) {
            skip_bits(8 - remainder);
        }
    }

    /*
     * Save/restore position.
     *
     * Useful when implementing conditional syntax.
     */
    [[nodiscard]]
    constexpr std::size_t save_position() const noexcept {
        return bit_position_;
    }

    void restore_position(std::size_t position) {
        if (position > data_.size() * 8) {
            throw std::out_of_range("BitstreamReader: invalid position");
        }

        bit_position_ = position;
    }

    /*
     * -------------------------------------------------------
     * Single bit
     * -------------------------------------------------------
     */

    [[nodiscard]]
    bool read_bit() {
        require_bits(1);

        const std::size_t byte_index = bit_position_ >> 3;

        const unsigned bit_index = 7u - static_cast<unsigned>(bit_position_ & 7u);

        const auto value = std::to_integer<std::uint8_t>(data_[byte_index]);

        ++bit_position_;

        return ((value >> bit_index) & 1u) != 0;
    }

    /*
     * Peek without consuming.
     */
    [[nodiscard]]
    bool peek_bit() const {
        require_bits_constexpr_safe(1);

        const std::size_t byte_index = bit_position_ >> 3;

        const unsigned bit_index = 7u - static_cast<unsigned>(bit_position_ & 7u);

        const auto value = std::to_integer<std::uint8_t>(data_[byte_index]);

        return ((value >> bit_index) & 1u) != 0;
    }

    /*
     * -------------------------------------------------------
     * Fixed-width unsigned values
     * -------------------------------------------------------
     */

    template <unsigned N>
    [[nodiscard]]
    std::uint64_t read_bits() {
        static_assert(N > 0 && N <= 64, "read_bits<N>: N must be 1..64");

        require_bits(N);

        std::uint64_t result = 0;

        unsigned remaining = N;

        while (remaining != 0) {
            const std::size_t byte_index = bit_position_ >> 3;

            const unsigned bit_offset = static_cast<unsigned>(bit_position_ & 7u);

            const unsigned available = 8u - bit_offset;

            const unsigned take = remaining < available ? remaining : available;

            const auto byte = std::to_integer<std::uint8_t>(data_[byte_index]);

            const unsigned shift = available - take;

            const std::uint8_t mask = static_cast<std::uint8_t>((std::uint16_t{1} << take) - 1);

            const std::uint8_t part = static_cast<std::uint8_t>((byte >> shift) & mask);

            result = (result << take) | part;

            bit_position_ += take;
            remaining -= take;
        }

        return result;
    }

    /*
     * Runtime-width read.
     *
     * Useful for syntax such as:
     *
     *     u(v)
     */
    [[nodiscard]]
    std::uint64_t read_bits(unsigned count) {
        if (count == 0) {
            return 0;
        }

        if (count > 64) {
            throw std::invalid_argument("BitstreamReader: width > 64");
        }

        std::uint64_t result = 0;

        while (count >= 32) {
            result = (result << 32) | read_bits<32>();

            count -= 32;
        }

        if (count != 0) {
            const auto part = read_bits_runtime_small(count);

            result = (result << count) | part;
        }

        return result;
    }

    [[nodiscard]]
    std::uint8_t read_u8(unsigned bits = 8) {
        if (bits > 8) {
            throw std::invalid_argument("read_u8: width > 8");
        }

        return static_cast<std::uint8_t>(read_bits(bits));
    }

    [[nodiscard]]
    std::uint16_t read_u16(unsigned bits = 16) {
        if (bits > 16) {
            throw std::invalid_argument("read_u16: width > 16");
        }

        return static_cast<std::uint16_t>(read_bits(bits));
    }

    [[nodiscard]]
    std::uint32_t read_u32(unsigned bits = 32) {
        if (bits > 32) {
            throw std::invalid_argument("read_u32: width > 32");
        }

        return static_cast<std::uint32_t>(read_bits(bits));
    }

    [[nodiscard]]
    std::uint64_t read_u64(unsigned bits = 64) {
        if (bits > 64) {
            throw std::invalid_argument("read_u64: width > 64");
        }

        return read_bits(bits);
    }

    /*
     * -------------------------------------------------------
     * Byte reads
     * -------------------------------------------------------
     */

    [[nodiscard]]
    std::uint8_t read_byte() {
        return read_u8(8);
    }

    /*
     * Read a big-endian byte-aligned integer.
     */
    [[nodiscard]]
    std::uint16_t read_be16() {
        if (!byte_aligned()) {
            throw std::logic_error("read_be16 requires byte alignment");
        }

        return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(read_byte()) << 8) | read_byte()
        );
    }

    [[nodiscard]]
    std::uint32_t read_be32() {
        if (!byte_aligned()) {
            throw std::logic_error("read_be32 requires byte alignment");
        }

        return (static_cast<std::uint32_t>(read_byte()) << 24) |
               (static_cast<std::uint32_t>(read_byte()) << 16) |
               (static_cast<std::uint32_t>(read_byte()) << 8) |
               static_cast<std::uint32_t>(read_byte());
    }

    /*
     * -------------------------------------------------------
     * Peek fixed-width
     * -------------------------------------------------------
     */

    template <unsigned N>
    [[nodiscard]]
    std::uint64_t peek_bits() const {
        static_assert(N > 0 && N <= 64, "peek_bits<N>: N must be 1..64");

        if constexpr (N <= 8) {
            require_bits_constexpr_safe(N);

            std::size_t position = bit_position_;

            std::uint64_t result = 0;

            for (unsigned i = 0; i < N; ++i) {
                const std::size_t byte_index = position >> 3;

                const unsigned bit_index = 7u - static_cast<unsigned>(position & 7u);

                const auto byte = std::to_integer<std::uint8_t>(data_[byte_index]);

                result = (result << 1) | ((byte >> bit_index) & 1u);

                ++position;
            }

            return result;
        } else {
            /*
             * Use a temporary reader for larger peeks.
             */
            BitstreamReader copy(*this);

            return copy.template read_bits<N>();
        }
    }

    /*
     * -------------------------------------------------------
     * Raw byte span
     * -------------------------------------------------------
     *
     * Only available when byte aligned.
     */
    [[nodiscard]]
    std::span<const byte_type> remaining_bytes() {
        if (!byte_aligned()) {
            throw std::logic_error("remaining_bytes requires byte alignment");
        }

        return data_.subspan(byte_position());
    }

   protected:
    void require_bits(std::size_t count) const {
        if (count > bits_remaining()) {
            throw std::out_of_range("BitstreamReader: insufficient bits");
        }
    }

    void require_bits_constexpr_safe(std::size_t count) const {
        if (count > bits_remaining()) {
            throw std::out_of_range("BitstreamReader: insufficient bits");
        }
    }

   private:
    [[nodiscard]]
    std::uint64_t read_bits_runtime_small(unsigned count) {
        std::uint64_t result = 0;

        for (unsigned i = 0; i < count; ++i) {
            result = (result << 1) | (read_bit() ? 1u : 0u);
        }

        return result;
    }

    std::span<const byte_type> data_{};

    std::size_t bit_position_ = 0;
};

}  // namespace bs