#pragma once

#include "av1_obu.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace bs {
namespace av1 {

/*
 * -----------------------------------------------------------
 * AV1 OBU header parser (AV1 §5.3)
 * -----------------------------------------------------------
 */
[[nodiscard]]
inline std::uint64_t
read_uleb128(
    std::span<const std::uint8_t> bytes,
    std::size_t& pos)
{
    std::uint64_t value = 0;

    for (unsigned i = 0; i < 8; ++i) {

        if (pos >= bytes.size()) {
            throw std::out_of_range(
                "AV1: truncated ULEB128 size");
        }

        const std::uint8_t b = bytes[pos++];

        value |= static_cast<std::uint64_t>(b & 0x7Fu) << (i * 7u);

        if ((b & 0x80u) == 0) {
            return value;
        }
    }

    throw std::invalid_argument(
        "AV1: ULEB128 size too large");
}


/*
 * Parse the OBU header + size + payload from an OBU byte span.
 */
[[nodiscard]]
inline Obu
parse_obu(
    std::span<const std::uint8_t> bytes)
{
    if (bytes.empty()) {
        throw std::out_of_range(
            "AV1: empty OBU");
    }

    const std::uint8_t header = bytes[0];

    const std::uint8_t type =
        static_cast<std::uint8_t>((header >> 3) & 0x0Fu);

    const bool extension_flag =
        (header >> 2) & 1u;

    const bool has_size_field =
        (header >> 1) & 1u;

    std::size_t pos = 1;

    std::uint8_t temporal_id = 0;
    std::uint8_t spatial_id = 0;

    if (extension_flag) {

        if (bytes.size() < 2) {
            throw std::out_of_range(
                "AV1: truncated OBU extension header");
        }

        const std::uint8_t ext = bytes[1];

        temporal_id =
            static_cast<std::uint8_t>((ext >> 5) & 0x07u);

        spatial_id =
            static_cast<std::uint8_t>((ext >> 3) & 0x03u);

        pos = 2;
    }

    std::uint64_t size = 0;

    if (has_size_field) {
        size = read_uleb128(bytes, pos);
    }

    Obu obu(
        bytes,
        type,
        extension_flag,
        has_size_field,
        size,
        temporal_id,
        spatial_id);

    if (has_size_field) {

        if (pos + size > bytes.size()) {
            throw std::out_of_range(
                "AV1: OBU payload exceeds OBU span");
        }

        obu.set_payload(bytes.subspan(pos, static_cast<std::size_t>(size)));

    } else {

        obu.set_payload(bytes.subspan(pos));
    }

    return obu;
}

} // namespace av1
} // namespace bs
