// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "av1_common.hpp"

#include <cstdint>
#include <span>

namespace bs {
namespace av1 {

/*
 * -----------------------------------------------------------
 * AV1 open bitstream unit (OBU)
 * -----------------------------------------------------------
 */
class Obu {
   public:
    Obu(std::span<const std::uint8_t> bytes,
        std::uint8_t obu_type,
        bool extension_flag,
        bool has_size_field,
        std::uint64_t size,
        std::uint8_t temporal_id,
        std::uint8_t spatial_id)
        : bytes_(bytes),
          obu_type_(obu_type),
          extension_flag_(extension_flag),
          has_size_field_(has_size_field),
          size_(size),
          temporal_id_(temporal_id),
          spatial_id_(spatial_id) {}

    /*
     * Full OBU span (header + payload).
     */
    [[nodiscard]]
    std::span<const std::uint8_t> whole_bytes() const noexcept {
        return bytes_;
    }

    /*
     * OBU payload (everything after the header + size).
     */
    [[nodiscard]]
    std::span<const std::uint8_t> payload_bytes() const noexcept {
        return payload_;
    }

    void set_payload(std::span<const std::uint8_t> payload) noexcept {
        payload_ = payload;
    }

    [[nodiscard]]
    std::uint8_t type() const noexcept {
        return obu_type_;
    }

    [[nodiscard]]
    bool has_extension() const noexcept {
        return extension_flag_;
    }

    [[nodiscard]]
    bool has_size_field() const noexcept {
        return has_size_field_;
    }

    [[nodiscard]]
    std::uint64_t size() const noexcept {
        return size_;
    }

    [[nodiscard]]
    std::uint8_t temporal_id() const noexcept {
        return temporal_id_;
    }

    [[nodiscard]]
    std::uint8_t spatial_id() const noexcept {
        return spatial_id_;
    }

   private:
    std::span<const std::uint8_t> bytes_{};
    std::span<const std::uint8_t> payload_{};

    std::uint8_t obu_type_ = 0;
    bool extension_flag_ = false;
    bool has_size_field_ = false;
    std::uint64_t size_ = 0;
    std::uint8_t temporal_id_ = 0;
    std::uint8_t spatial_id_ = 0;
};

}  // namespace av1
}  // namespace bs
