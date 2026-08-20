// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>

namespace bs {

/*
 * -----------------------------------------------------------
 * IVF container framer (VP8 / VP9)
 * -----------------------------------------------------------
 *
 * The IVF format wraps raw video frames:
 *
 *   32-byte file header   ("DKIF", fourcc "VP80"/"VP90", ...)
 *   then per frame:
 *     12-byte frame header (frame_size u32 LE + timestamp u64)
 *     frame_size bytes of frame data
 *
 * The yielded frame spans reference the input buffer.
 */
class IvfFramer {
   public:
    explicit IvfFramer(std::span<const std::uint8_t> data) : data_(data) {
        /*
         * Skip the 32-byte file header.
         */
        pos_ = 32;

        locate_next();
    }

    [[nodiscard]]
    bool valid() const noexcept {
        return !finished_;
    }

    [[nodiscard]]
    std::span<const std::uint8_t> frame() const noexcept {
        if (finished_) {
            return {};
        }

        return data_.subspan(frame_begin_, frame_end_ - frame_begin_);
    }

    void next() {
        locate_next();
    }

   private:
    void locate_next() {
        if (pos_ + 12 > data_.size()) {
            finished_ = true;
            return;
        }

        const std::uint32_t size = static_cast<std::uint32_t>(data_[pos_]) |
                                   (static_cast<std::uint32_t>(data_[pos_ + 1]) << 8) |
                                   (static_cast<std::uint32_t>(data_[pos_ + 2]) << 16) |
                                   (static_cast<std::uint32_t>(data_[pos_ + 3]) << 24);

        frame_begin_ = pos_ + 12;

        frame_end_ = frame_begin_ + size;

        if (frame_end_ > data_.size()) {
            finished_ = true;
            return;
        }

        pos_ = frame_end_;
        finished_ = false;
    }

    std::span<const std::uint8_t> data_{};

    std::size_t pos_ = 0;
    std::size_t frame_begin_ = 0;
    std::size_t frame_end_ = 0;

    bool finished_ = true;
};

}  // namespace bs
