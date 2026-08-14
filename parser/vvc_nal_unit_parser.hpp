#pragma once

#include "vvc_nal_unit.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC NAL unit header parser (H.266 §7.3.1.1)
 * -----------------------------------------------------------
 *
 *     forbidden_zero_bit (1) · nuh_reserved_zero_bit (1) ·
 *     nuh_layer_id (6) · nal_unit_type (5) ·
 *     nuh_temporal_id_plus1 (3)
 */
[[nodiscard]]
inline NalUnit parse_nal_unit(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 2) {
        throw std::out_of_range("VVC: truncated NAL unit header");
    }

    const std::uint16_t header = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[0]) << 8) | static_cast<std::uint16_t>(bytes[1])
    );

    const std::uint8_t forbidden_zero_bit = static_cast<std::uint8_t>((header >> 15) & 1u);

    const std::uint8_t nuh_reserved_zero_bit = static_cast<std::uint8_t>((header >> 14) & 1u);

    const std::uint8_t layer_id = static_cast<std::uint8_t>((header >> 8) & 0x3Fu);

    const std::uint8_t nal_unit_type = static_cast<std::uint8_t>((header >> 3) & 0x1Fu);

    const std::uint8_t temporal_id_plus1 = static_cast<std::uint8_t>(header & 0x7u);

    if (forbidden_zero_bit != 0) {
        throw std::runtime_error("VVC: forbidden_zero_bit != 0");
    }

    if (nuh_reserved_zero_bit != 0) {
        throw std::runtime_error("VVC: nuh_reserved_zero_bit != 0");
    }

    if (temporal_id_plus1 == 0) {
        throw std::runtime_error("VVC: nuh_temporal_id_plus1 == 0");
    }

    return NalUnit(bytes, nal_unit_type, layer_id, temporal_id_plus1 - 1);
}

}  // namespace vvc
}  // namespace bs
