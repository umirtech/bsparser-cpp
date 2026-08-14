#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace bs {

/*
 * -----------------------------------------------------------
 * Plain bit reader
 * -----------------------------------------------------------
 *
 * A minimal MSB-first bit reader over a byte span, with NO
 * emulation-prevention handling. Used by codecs that do not use
 * the H.26x RBSP scheme (VP8, VP9, AV1), where bytes are read
 * verbatim.
 */
class PlainBitReader {
   public:
    explicit PlainBitReader(std::span<const std::uint8_t> data) noexcept : data_(data) {}

    [[nodiscard]]
    bool read_bit() {
        if (bit_pos_ == 0 && byte_pos_ >= data_.size()) {
            throw std::out_of_range("PlainBitReader: read past end");
        }

        const std::uint8_t byte = data_[byte_pos_];

        const unsigned shift = 7u - bit_pos_;

        const bool value = (byte >> shift) & 1u;

        ++bit_pos_;

        if (bit_pos_ == 8) {
            bit_pos_ = 0;
            ++byte_pos_;
        }

        return value;
    }

    [[nodiscard]]
    std::uint32_t read_bits(unsigned count) {
        if (count > 32) {
            throw std::invalid_argument("PlainBitReader: count > 32");
        }

        std::uint32_t value = 0;

        for (unsigned i = 0; i < count; ++i) {
            value = (value << 1) | (read_bit() ? 1u : 0u);
        }

        return value;
    }

    /*
     * Skip to the next byte boundary (consuming to the end of
     * the current byte).
     */
    void byte_align() {
        if (bit_pos_ != 0) {
            const unsigned skip = 8u - bit_pos_;
            for (unsigned i = 0; i < skip; ++i) {
                (void)read_bit();
            }
        }
    }

    [[nodiscard]]
    bool byte_aligned() const noexcept {
        return bit_pos_ == 0;
    }

    [[nodiscard]]
    std::size_t byte_position() const noexcept {
        return byte_pos_;
    }

   private:
    std::span<const std::uint8_t> data_{};

    std::size_t byte_pos_ = 0;

    unsigned bit_pos_ = 0;
};

}  // namespace bs
