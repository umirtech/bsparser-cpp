// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "avc_common.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace bs {
namespace avc {

/*
 * H.264 / AVC NAL unit
 *
 * A NAL unit consists conceptually of:
 *
 *     nal_unit_header()   (1 byte)
 *     rbsp_byte[]         (EBSP payload)
 *
 * This structure is intentionally non-owning: the payload is
 * a std::span over the caller's buffer.
 *
 * Lifetime requirement:
 *
 *     The memory referenced by payload must remain alive while
 *     this NalUnit is being used.
 */

/*
 * -----------------------------------------------------------
 * NAL unit header
 * -----------------------------------------------------------
 *
 * One byte:
 *
 *     forbidden_zero_bit 1
 *     nal_ref_idc        2
 *     nal_unit_type      5
 */

struct NalUnitHeader {
    bool forbidden_zero_bit = false;
    std::uint8_t nal_ref_idc = 0;
    NalUnitType nal_unit_type = NalUnitType::Sei;

    [[nodiscard]]
    constexpr bool valid() const noexcept {
        return !forbidden_zero_bit;
    }
};

/*
 * -----------------------------------------------------------
 * Complete NAL unit view
 * -----------------------------------------------------------
 */

struct NalUnit {
    NalUnitHeader header{};

    /*
     * EBSP payload (emulation-prevention bytes still present).
     */
    std::span<const std::uint8_t> payload{};

    [[nodiscard]]
    constexpr NalUnitType type() const noexcept {
        return header.nal_unit_type;
    }

    [[nodiscard]]
    constexpr std::uint8_t nal_type() const noexcept {
        return static_cast<std::uint8_t>(header.nal_unit_type);
    }

    [[nodiscard]]
    constexpr bool is_vcl() const noexcept {
        return is_vcl_nal_unit(header.nal_unit_type);
    }

    [[nodiscard]]
    constexpr bool is_idr() const noexcept {
        return is_idr_nal_unit(header.nal_unit_type);
    }

    [[nodiscard]]
    constexpr std::span<const std::uint8_t> payload_bytes() const noexcept {
        return payload;
    }

    [[nodiscard]]
    constexpr std::size_t payload_size() const noexcept {
        return payload.size();
    }

    [[nodiscard]]
    constexpr bool has_payload() const noexcept {
        return !payload.empty();
    }

    [[nodiscard]]
    constexpr bool valid() const noexcept {
        return header.valid();
    }
};

/*
 * -----------------------------------------------------------
 * Construction helper
 * -----------------------------------------------------------
 */

[[nodiscard]]
constexpr NalUnit make_nal_unit(
    const NalUnitHeader& header, std::span<const std::uint8_t> payload
) noexcept {
    return NalUnit{header, payload};
}

}  // namespace avc
}  // namespace bs