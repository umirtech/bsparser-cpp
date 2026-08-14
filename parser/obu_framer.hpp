#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace bs {
namespace av1 {

/*
 * -----------------------------------------------------------
 * AV1 OBU framing
 * -----------------------------------------------------------
 *
 * AV1 streams come in two forms:
 *
 *   Annex-B:   each OBU preceded by a 0x00 0x00 0x01 start
 *              code. OBU boundaries are found by scanning.
 *   Low-overhead: OBUs are concatenated, each carrying an
 *              obu_has_size_field=1 and a ULEB128 size. Used by
 *              the IVF / MP4 / raw `obu` containers.
 *
 * This iterator auto-detects the form from the leading bytes
 * and yields each OBU's byte span (header + payload),
 * referencing the input buffer.
 */
class ObuFramer {
public:
    explicit ObuFramer(
        std::span<const std::uint8_t> data)
        : data_(data)
    {
        low_overhead_ =
            !(data.size() >= 3 &&
              data[0] == 0x00 &&
              data[1] == 0x00 &&
              data[2] == 0x01);

        locate_next();
    }


    [[nodiscard]]
    bool valid() const noexcept
    {
        return !finished_;
    }


    [[nodiscard]]
    std::span<const std::uint8_t> obu() const noexcept
    {
        if (finished_) {
            return {};
        }

        return data_.subspan(obu_begin_, obu_end_ - obu_begin_);
    }


    void next()
    {
        locate_next();
    }


private:
    [[nodiscard]]
    static std::size_t find_start_code(
        std::span<const std::uint8_t> data,
        std::size_t from) noexcept
    {
        std::size_t i = from;

        while (i + 2 < data.size()) {

            if (data[i] == 0x00 &&
                data[i + 1] == 0x00 &&
                data[i + 2] == 0x01) {

                return i;
            }

            ++i;
        }

        return data.size();
    }


    /*
     * Read the ULEB128 size at `pos`; advance pos.
     */
    static std::uint64_t read_size(
        std::span<const std::uint8_t> data,
        std::size_t& pos)
    {
        std::uint64_t size = 0;

        for (unsigned i = 0; i < 8; ++i) {

            if (pos >= data.size()) {
                throw std::out_of_range(
                    "AV1: truncated OBU size");
            }

            const std::uint8_t b = data[pos++];

            size |= static_cast<std::uint64_t>(b & 0x7Fu) << (i * 7u);

            if ((b & 0x80u) == 0) {
                return size;
            }
        }

        throw std::invalid_argument(
            "AV1: OBU size too large");
    }


    void locate_next()
    {
        if (low_overhead_) {
            locate_next_low_overhead();
        } else {
            locate_next_annex_b();
        }
    }


    void locate_next_annex_b()
    {
        const std::size_t start =
            find_start_code(data_, current_);

        if (start == data_.size()) {
            finished_ = true;
            return;
        }

        const std::size_t begin = start + 3;

        const std::size_t next =
            find_start_code(data_, begin);

        if (begin >= next) {
            current_ = next;
            locate_next_annex_b();
            return;
        }

        obu_begin_ = begin;
        obu_end_ = next;
        current_ = next;
        finished_ = false;
    }


    void locate_next_low_overhead()
    {
        if (current_ >= data_.size()) {
            finished_ = true;
            return;
        }

        const std::size_t begin = current_;

        if (begin + 1 > data_.size()) {
            finished_ = true;
            return;
        }

        const std::uint8_t hdr = data_[begin];

        const bool extension_flag = (hdr >> 2) & 1u;
        const bool has_size_field = (hdr >> 1) & 1u;

        std::size_t pos = begin + 1;

        if (extension_flag) {
            pos += 1;
        }

        if (!has_size_field) {

            /* Cannot size the OBU; treat the rest as one unit. */
            obu_begin_ = begin;
            obu_end_ = data_.size();
            current_ = data_.size();
            finished_ = false;
            return;
        }

        const std::uint64_t size =
            read_size(data_, pos);

        obu_begin_ = begin;
        obu_end_ = pos + static_cast<std::size_t>(size);

        if (obu_end_ > data_.size()) {
            finished_ = true;
            return;
        }

        current_ = obu_end_;
        finished_ = false;
    }


    std::span<const std::uint8_t> data_{};

    std::size_t current_ = 0;
    std::size_t obu_begin_ = 0;
    std::size_t obu_end_ = 0;

    bool low_overhead_ = false;
    bool finished_ = true;
};

} // namespace av1
} // namespace bs
