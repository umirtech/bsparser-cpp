#pragma once

#include "avc_nal_unit.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

namespace bs {
namespace avc {

class NalUnitParseError : public std::runtime_error {
public:
    explicit NalUnitParseError(const char* message)
        : std::runtime_error(message)
    {
    }

    explicit NalUnitParseError(const std::string& message)
        : std::runtime_error(message)
    {
    }
};


using NalUnitSpan =
    std::span<const std::uint8_t>;


/*
 * -----------------------------------------------------------
 * Parse one complete AVC NAL unit
 * -----------------------------------------------------------
 *
 * The AVC NAL header is a single byte:
 *
 *     byte 0   forbidden_zero_bit | nal_ref_idc | nal_unit_type
 *     byte 1+  EBSP payload
 */

[[nodiscard]]
inline NalUnit
parse_nal_unit(
    NalUnitSpan bytes)
{
    if (bytes.size() < 1) {
        throw NalUnitParseError(
            "AVC NAL unit: truncated header");
    }

    const std::uint8_t raw =
        bytes[0];

    const NalUnitHeader header{
        static_cast<bool>((raw >> 7) & 1u),
        static_cast<std::uint8_t>((raw >> 5) & 3u),
        static_cast<NalUnitType>(raw & 0x1Fu)
    };

    if (header.forbidden_zero_bit) {
        throw NalUnitParseError(
            "AVC NAL unit: forbidden_zero_bit is non-zero");
    }

    return make_nal_unit(
        header,
        bytes.subspan(1));
}


/*
 * -----------------------------------------------------------
 * Non-throwing variant
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline bool
try_parse_nal_unit(
    NalUnitSpan bytes,
    NalUnit& output)
{
    try {
        output = parse_nal_unit(bytes);
        return true;
    }
    catch (const NalUnitParseError&) {
        return false;
    }
}

} // namespace avc
} // namespace bs