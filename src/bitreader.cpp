#include "bitreader.h"

namespace bsparser {

    BitReader::BitReader(const Bytes& data) : data_(data) {}

    uint32_t BitReader::u(unsigned bits)
    {
        if (bits > 32 || bits > bits_left()) throw std::out_of_range("truncated bitstream");
        uint32_t result = 0;
        for (unsigned i = 0; i < bits; ++i)
            result = (result << 1) | ((data_[bitpos_++] >> (7 - ((bitpos_ - 1) & 7))) & 1);
        return result;
    }

    int32_t BitReader::s(unsigned bits)
    {
        if (bits == 0)
            return 0;

        const uint32_t v = u(bits);

        if (bits == 32)
            return static_cast<int32_t>(v);

        const uint32_t sign = 1u << (bits - 1);

        if (v & sign)
            return static_cast<int32_t>(
                static_cast<int64_t>(v) -
                (int64_t{1} << bits)
            );

        return static_cast<int32_t>(v);
    }

    uint32_t BitReader::ue()
    {
        unsigned zeros = 0;

        while (u(1) == 0)
        {
            if (++zeros > 31)
                throw std::runtime_error(
                    "Exp-Golomb value too large");
        }

        if (zeros == 0)
            return 0;

        return ((uint32_t{1} << zeros) - 1u) + u(zeros);
    }

    int32_t BitReader::se()
    {
        auto c = ue();
        return c & 1 ? int32_t((c + 1) / 2) : -int32_t(c / 2);
    }

    uint64_t BitReader::leb128()
    {
        uint64_t result = 0;
        for (unsigned i = 0; i < 8; ++i)
        {
            auto byte = u(8);
            result |= uint64_t(byte & 127) << (7 * i);
            if (!(byte & 128)) return result;
        }
        throw std::runtime_error("LEB128 exceeds 8 bytes");
    }

    size_t BitReader::bit_position() const noexcept
    {
        return bitpos_;
    }

    size_t BitReader::bits_left() const noexcept
    {
        return data_.size() * 8 - bitpos_;
    }

    void BitReader::skip(size_t n)
    {
        if (n > bits_left()) throw std::out_of_range("truncated bitstream");
        bitpos_ += n;
    }

    uint32_t BitReader::peek_bit(size_t offset) const noexcept
    {
        const size_t pos = bitpos_ + offset;

        if (pos >= data_.size() * 8)
            return 0;

        const size_t byte_pos = pos >> 3;
        const unsigned bit_in_byte = 7u - static_cast<unsigned>(pos & 7u);

        return (data_[byte_pos] >> bit_in_byte) & 1u;
    }

    bool BitReader::more_rbsp_data() const noexcept
    {
        const size_t left = bits_left();

        if (left == 0)
            return false;

        /*
        * rbsp_trailing_bits() is:
        *
        *     rbsp_stop_one_bit
        *     rbsp_alignment_zero_bit*
        *
        * Therefore the complete remaining bit sequence:
        *
        *     1 000000...
        *
        * means there is no more RBSP syntax data.
        */

        // First bit is not the rbsp_stop_one_bit.
        if (peek_bit(0) == 0)
            return true;

        // First bit is 1. Check whether all remaining bits are zero.
        for (size_t i = 1; i < left; ++i)
        {
            if (peek_bit(i) != 0)
                return true;
        }

        // Remaining bits are exactly 1000... -> trailing bits.
        return false;
    }


    void BitReader::rbsp_trailing_bits()
    {
        if (bits_left() == 0)
            throw std::out_of_range("missing rbsp_stop_one_bit");

        // rbsp_stop_one_bit
        if (u(1) != 1)
            throw std::runtime_error("invalid rbsp_stop_one_bit");

        // rbsp_alignment_zero_bit*
        while (bits_left() > 0)
        {
            if (u(1) != 0)
                throw std::runtime_error(
                    "invalid rbsp_alignment_zero_bit");
        }
    }

}

