#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace bs {
namespace av1 {

/*
 * -----------------------------------------------------------
 * AV1 boolean arithmetic decoder (AV1 §9.2)
 * -----------------------------------------------------------
 *
 * AV1 headers are entropy-coded with a Daala-style boolean
 * decoder. Uncompressed fields are read as boolean with
 * probability 128. This decoder implements the spec process.
 */
class BooleanDecoder {
   public:
    explicit BooleanDecoder(std::span<const std::uint8_t> data) : data_(data) {
        value_ = 0;
        for (int i = 0; i < 16; ++i) {
            value_ = (value_ << 1) | next_bit();
        }
    }

    /*
     * Read a single boolean with the given probability
     * (0..255).
     */
    bool read_bool(std::uint8_t probability) {
        const unsigned split = 1u + (((range_ - 1u) * probability) >> 8);

        const unsigned big_split = split << 8;

        bool value;

        if (value_ >= big_split) {
            value = true;
            value_ -= big_split;
            range_ -= split;
        } else {
            value = false;
            range_ = split;
        }

        while (range_ < 128u) {
            range_ <<= 1;
            value_ = (value_ << 1) | next_bit();
        }

        return value;
    }

    /*
     * Read `count` bits as boolean reads with probability 128
     * (i.e. uncompressed / f(count)).
     */
    std::uint32_t read_literal(unsigned count) {
        if (count > 32) {
            throw std::invalid_argument("BooleanDecoder: count > 32");
        }

        std::uint32_t value = 0;

        for (unsigned i = 0; i < count; ++i) {
            value = (value << 1) | (read_bool(128u) ? 1u : 0u);
        }

        return value;
    }

    [[nodiscard]]
    bool more_data() const noexcept {
        return byte_pos_ < data_.size() || bit_pos_ != 0;
    }

   private:
    /*
     * One raw bit from the compressed byte stream, MSB first.
     * The decoder is zero-padded at the end of the buffer
     * (the spec's read_bit returns 0 past the end).
     */
    std::uint8_t next_bit() {
        if (byte_pos_ >= data_.size()) {
            return 0;
        }

        const std::uint8_t bit =
            static_cast<std::uint8_t>((data_[byte_pos_] >> (7u - bit_pos_)) & 1u);

        ++bit_pos_;

        if (bit_pos_ == 8u) {
            bit_pos_ = 0;
            ++byte_pos_;
        }

        return bit;
    }

    std::span<const std::uint8_t> data_{};

    std::size_t byte_pos_ = 0;
    unsigned bit_pos_ = 0;

    std::uint32_t value_ = 0;
    unsigned range_ = 254u;
};

}  // namespace av1
}  // namespace bs
