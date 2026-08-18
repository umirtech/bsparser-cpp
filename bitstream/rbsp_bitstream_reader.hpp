#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <limits>
#include <vector>

namespace bs {

/*
 * RBSP bitstream reader (shared by H.264 / H.265 / H.266).
 *
 * Input:
 *
 *     NAL EBSP payload
 *
 * Logical output:
 *
 *     RBSP bytes
 *
 * without copying/rematerializing the buffer.
 *
 * Emulation-prevention bytes (0x03 following 00 00 under
 * the applicable conditions) are skipped logically.
 */
class RbspBitstreamReader {
   public:
    explicit RbspBitstreamReader(std::span<const std::byte> ebsp) : ebsp_(ebsp) {
        build_logical_map();
    }

    /*
     * -------------------------------------------------------
     * Position
     * -------------------------------------------------------
     */

    [[nodiscard]]
    std::size_t bit_position() const noexcept {
        return rbsp_bit_position_;
    }

    [[nodiscard]]
    std::size_t bits_remaining() const noexcept {
        /*
         * We cannot simply calculate:
         *
         *     ebsp.size() * 8 - position
         *
         * because skipped emulation-prevention bytes do not
         * belong to the logical RBSP.
         *
         * Calculate the logical RBSP size lazily.
         */
        return rbsp_total_bits() - rbsp_bit_position_;
    }

    [[nodiscard]]
    bool empty() const noexcept {
        return bits_remaining() == 0;
    }

    [[nodiscard]]
    bool byte_aligned() const noexcept {
        return (rbsp_bit_position_ & 7u) == 0;
    }

    /*
     * -------------------------------------------------------
     * Single bit
     * -------------------------------------------------------
     */

    [[nodiscard]]
    bool read_bit() {
        require_bits(1);

        const std::size_t logical_byte = rbsp_bit_position_ >> 3;

        const unsigned bit_index = 7u - static_cast<unsigned>(rbsp_bit_position_ & 7u);

        const std::size_t ebsp_byte = logical_to_ebsp_byte(logical_byte);

        const auto value = std::to_integer<std::uint8_t>(ebsp_[ebsp_byte]);

        ++rbsp_bit_position_;

        return ((value >> bit_index) & 1u) != 0;
    }

    /*
     * -------------------------------------------------------
     * Fixed width
     * -------------------------------------------------------
     */

    template <unsigned N>
    [[nodiscard]]
    std::uint64_t read_bits() {
        static_assert(N > 0 && N <= 64);

        std::uint64_t result = 0;

        for (unsigned i = 0; i < N; ++i) {
            result = (result << 1) | (read_bit() ? 1u : 0u);
        }

        return result;
    }

    [[nodiscard]]
    std::uint64_t read_bits(unsigned count) {
        if (count > 64) {
            throw std::invalid_argument("RbspBitstreamReader: width > 64");
        }

        std::uint64_t result = 0;

        for (unsigned i = 0; i < count; ++i) {
            result = (result << 1) | (read_bit() ? 1u : 0u);
        }

        return result;
    }

    [[nodiscard]]
    std::uint8_t read_u8(unsigned bits = 8) {
        return static_cast<std::uint8_t>(read_bits(bits));
    }

    [[nodiscard]]
    std::uint16_t read_u16(unsigned bits = 16) {
        return static_cast<std::uint16_t>(read_bits(bits));
    }

    [[nodiscard]]
    std::uint32_t read_u32(unsigned bits = 32) {
        return static_cast<std::uint32_t>(read_bits(bits));
    }

    /*
     * -------------------------------------------------------
     * Unsigned Exp-Golomb
     * -------------------------------------------------------
     *
     * H.265 ue(v):
     *
     *     leadingZeroBits
     *     codeNum
     *
     * codeNum =
     *
     *     (1 << leadingZeroBits) - 1
     *     + infoBits
     */
    [[nodiscard]]
    std::uint32_t read_ue() {
        unsigned leading_zero_bits = 0;

        while (true) {
            if (bits_remaining() == 0) {
                throw std::out_of_range("RBSP: truncated ue(v)");
            }

            if (read_bit()) {
                break;
            }

            ++leading_zero_bits;

            /*
             * Prevent pathological streams from causing
             * undefined shifts.
             */
            if (leading_zero_bits >= 32) {
                throw std::overflow_error("RBSP: ue(v) exceeds uint32_t");
            }
        }

        if (leading_zero_bits == 0) {
            return 0;
        }

        const auto info = read_bits(leading_zero_bits);

        const std::uint64_t code_num = ((std::uint64_t{1} << leading_zero_bits) - 1) + info;

        if (code_num > std::numeric_limits<std::uint32_t>::max()) {
            throw std::overflow_error("RBSP: ue(v) exceeds uint32_t");
        }

        return static_cast<std::uint32_t>(code_num);
    }

    /*
     * -------------------------------------------------------
     * Signed Exp-Golomb
     * -------------------------------------------------------
     *
     * H.265 se(v):
     *
     *     codeNum = ue(v)
     *
     *     if(codeNum & 1)
     *         value = (codeNum + 1) / 2
     *     else
     *         value = -(codeNum / 2)
     */
    [[nodiscard]]
    std::int32_t read_se() {
        const std::uint32_t code_num = read_ue();

        if ((code_num & 1u) != 0) {
            const std::uint32_t value = (code_num + 1u) >> 1;

            if (value > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
                throw std::overflow_error("RBSP: se(v) overflow");
            }

            return static_cast<std::int32_t>(value);

        } else {
            const std::uint32_t value = code_num >> 1;

            if (value > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
                throw std::overflow_error("RBSP: se(v) overflow");
            }

            return -static_cast<std::int32_t>(value);
        }
    }

    /*
     * -------------------------------------------------------
     * Position
     * -------------------------------------------------------
     */

    void skip_bits(std::size_t count) {
        require_bits(count);

        rbsp_bit_position_ += count;
    }

    void align_to_byte() {
        const auto remainder = rbsp_bit_position_ & 7u;

        if (remainder != 0) {
            skip_bits(8 - remainder);
        }
    }

    /*
     * -------------------------------------------------------
     * H.265 rbsp_trailing_bits()
     * -------------------------------------------------------
     *
     * Syntax:
     *
     *     rbsp_stop_one_bit
     *     while(!byte_aligned())
     *         rbsp_alignment_zero_bit
     *
     * We validate the actual values.
     */
    void read_rbsp_trailing_bits() {
        if (bits_remaining() == 0) {
            throw std::out_of_range("RBSP: missing rbsp_stop_one_bit");
        }

        /*
         * Must be 1.
         */
        if (!read_bit()) {
            throw std::runtime_error("RBSP: rbsp_stop_one_bit != 1");
        }

        /*
         * Remaining bits until byte alignment must be 0.
         */
        while (!byte_aligned()) {
            if (bits_remaining() == 0) {
                throw std::out_of_range("RBSP: truncated alignment bits");
            }

            if (read_bit()) {
                throw std::runtime_error("RBSP: rbsp_alignment_zero_bit != 0");
            }
        }
    }

    /*
     * -------------------------------------------------------
     * Byte Position
     * -------------------------------------------------------
     */

    std::size_t position() noexcept {
        return rbsp_bit_position_;
    }

    /*
     * -------------------------------------------------------
     * more_rbsp_data()
     * -------------------------------------------------------
     *
     * We need to determine whether the remaining bits are
     * actual syntax or only:
     *
     *     rbsp_stop_one_bit
     *     rbsp_alignment_zero_bit[]
     *
     * H.265 explicitly defines this as a syntax-level
     * determination rather than simply "remaining bytes".
     */
    [[nodiscard]]
    bool more_rbsp_data() const {
        const std::size_t remaining = bits_remaining();

        if (remaining == 0) {
            return false;
        }

        /*
         * Search from the final logical bit backwards for
         * the last '1' bit.
         *
         * If that bit is exactly the first bit of the
         * trailing-bits structure, there is no more RBSP
         * syntax data.
         */
        const std::size_t last_one = find_last_one_bit();

        if (last_one == invalid_position) {
            /*
             * No stop bit at all.
             */
            return false;
        }

        /*
         * If everything after the last '1' is zero, the last
         * one is a candidate rbsp_stop_one_bit.
         *
         * We need to know whether there are bits before it.
         */
        const std::size_t current = rbsp_bit_position_;

        return last_one > current;
    }

    /*
     * -------------------------------------------------------
     * Access to original EBSP
     * -------------------------------------------------------
     */

    [[nodiscard]]
    std::span<const std::byte> ebsp() const noexcept {
        return ebsp_;
    }

    /*
     * -------------------------------------------------------
     * Logical RBSP byte access
     * -------------------------------------------------------
     */

    [[nodiscard]]
    std::uint8_t rbsp_byte(std::size_t logical_index) const {
        const auto ebsp_index = logical_to_ebsp_byte(logical_index);

        return std::to_integer<std::uint8_t>(ebsp_[ebsp_index]);
    }

   private:
    static constexpr std::size_t invalid_position = std::numeric_limits<std::size_t>::max();

    /*
     * -------------------------------------------------------
     * Emulation-prevention detection
     * -------------------------------------------------------
     *
     * H.265 EBSP:
     *
     *     00 00 03 xx
     *
     * contains an emulation_prevention_three_byte at 03.
     *
     * The 03 is skipped in the logical RBSP.
     *
     * We do NOT remove arbitrary 03 bytes.
     */
    [[nodiscard]]
    bool is_emulation_prevention_byte(std::size_t index) const noexcept {
        if (index < 2 || index >= ebsp_.size()) {
            return false;
        }

        const auto a = std::to_integer<std::uint8_t>(ebsp_[index - 2]);

        const auto b = std::to_integer<std::uint8_t>(ebsp_[index - 1]);

        const auto c = std::to_integer<std::uint8_t>(ebsp_[index]);

        /*
         * For HEVC RBSP extraction, 0x03 is inserted after
         * two consecutive zero bytes when the following byte
         * is in the protected range.
         *
         * The usual condition is:
         *
         *     00 00 03 00..03
         */
        return a == 0x00u && b == 0x00u && c == 0x03u && index + 1 < ebsp_.size() &&
               std::to_integer<std::uint8_t>(ebsp_[index + 1]) <= 0x03u;
    }

    /*
     * Build the logical-RBSP to EBSP byte table.
     *
     * This runs once at construction and makes every later
     * byte lookup O(1).
     *
     * Without it, logical_to_ebsp_byte() scanned the whole
     * EBSP on every single bit read, making parsing O(n^2).
     */
    void build_logical_map() {
        logical_to_ebsp_.clear();

        logical_to_ebsp_.reserve(ebsp_.size());

        for (std::size_t i = 0; i < ebsp_.size(); ++i) {
            if (is_emulation_prevention_byte(i)) {
                continue;
            }

            logical_to_ebsp_.push_back(i);
        }

        rbsp_byte_count_ = logical_to_ebsp_.size();

        rbsp_total_bits_ = rbsp_byte_count_ * 8;
    }

    /*
     * Map an RBSP byte index to the corresponding EBSP byte.
     */
    [[nodiscard]]
    std::size_t logical_to_ebsp_byte(std::size_t logical_index) const {
        if (logical_index >= logical_to_ebsp_.size()) {
            throw std::out_of_range("RBSP: logical byte index out of range");
        }

        return logical_to_ebsp_[logical_index];
    }

    /*
     * Count logical RBSP bytes.
     */
    [[nodiscard]]
    std::size_t rbsp_byte_count() const noexcept {
        return rbsp_byte_count_;
    }

    [[nodiscard]]
    std::size_t rbsp_total_bits() const noexcept {
        return rbsp_total_bits_;
    }

    /*
     * Find the final '1' bit in the logical RBSP.
     *
     * The result depends only on the fixed RBSP contents, so
     * it is computed once and cached.
     */
    [[nodiscard]]
    std::size_t find_last_one_bit() const {
        if (last_one_bit_cache_ != invalid_position) {
            return last_one_bit_cache_;
        }

        const std::size_t total_bits = rbsp_total_bits();

        for (std::size_t pos = total_bits; pos > 0; --pos) {
            const std::size_t bit = pos - 1;

            const std::size_t logical_byte = bit >> 3;

            const unsigned bit_index = 7u - static_cast<unsigned>(bit & 7u);

            const auto value = rbsp_byte(logical_byte);

            if (((value >> bit_index) & 1u) != 0) {
                last_one_bit_cache_ = bit;
                return bit;
            }
        }

        return invalid_position;
    }

    void require_bits(std::size_t count) const {
        if (count > bits_remaining()) {
            throw std::out_of_range("RbspBitstreamReader: insufficient bits");
        }
    }

    std::span<const std::byte> ebsp_{};

    /*
     * logical_to_ebsp_[i] is the EBSP byte index of logical
     * RBSP byte i.
     */
    std::vector<std::size_t> logical_to_ebsp_{};

    std::size_t rbsp_byte_count_ = 0;

    std::size_t rbsp_total_bits_ = 0;

    std::size_t rbsp_bit_position_ = 0;

    /*
     * Cached position of the final '1' bit (logical).
     */
    mutable std::size_t last_one_bit_cache_ = invalid_position;
};

}  // namespace bs