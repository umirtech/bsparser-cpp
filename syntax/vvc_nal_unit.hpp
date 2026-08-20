// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "vvc_common.hpp"

#include <cstdint>
#include <span>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC NAL unit
 * -----------------------------------------------------------
 *
 * A NAL unit is the two-byte header plus the EBSP payload, all
 * viewed zero-copy from the input buffer.
 */
class NalUnit {
   public:
    NalUnit(
        std::span<const std::uint8_t> bytes,
        std::uint8_t nal_unit_type,
        std::uint8_t layer_id,
        std::uint8_t temporal_id
    )
        : bytes_(bytes),
          nal_unit_type_(nal_unit_type),
          layer_id_(layer_id),
          temporal_id_(temporal_id) {}

    [[nodiscard]]
    std::span<const std::uint8_t> header_bytes() const noexcept {
        return bytes_.subspan(0, 2);
    }

    /*
     * Everything after the two-byte NAL header.
     */
    [[nodiscard]]
    std::span<const std::uint8_t> payload_bytes() const noexcept {
        return bytes_.subspan(2);
    }

    [[nodiscard]]
    std::span<const std::uint8_t> whole_bytes() const noexcept {
        return bytes_;
    }

    [[nodiscard]]
    std::uint8_t nal_type() const noexcept {
        return nal_unit_type_;
    }

    [[nodiscard]]
    NalUnitType type() const noexcept {
        return static_cast<NalUnitType>(nal_unit_type_);
    }

    [[nodiscard]]
    std::uint8_t layer_id() const noexcept {
        return layer_id_;
    }

    [[nodiscard]]
    std::uint8_t temporal_id() const noexcept {
        return temporal_id_;
    }

    [[nodiscard]]
    bool is_vcl() const noexcept {
        return is_vcl_nal_unit(nal_unit_type_);
    }

   private:
    std::span<const std::uint8_t> bytes_{};

    std::uint8_t nal_unit_type_ = 0;
    std::uint8_t layer_id_ = 0;
    std::uint8_t temporal_id_ = 0;
};

}  // namespace vvc
}  // namespace bs
