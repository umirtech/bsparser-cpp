#pragma once

#include "hevc_nal_unit_header.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace bs {

/*
 * H.265 NAL unit header parser.
 *
 * nal_unit_header() {
 *
 *     forbidden_zero_bit
 *     nal_unit_type               u(6)
 *     nuh_layer_id                u(6)
 *     nuh_temporal_id_plus1       u(3)
 *
 * }
 *
 * Total = 16 bits.
 *
 * The parser does not own the bitstream. It consumes the
 * supplied BitstreamReader and returns the decoded syntax
 * structure.
 */

/*
 * -----------------------------------------------------------
 * Parser error
 * -----------------------------------------------------------
 */

class NalUnitHeaderParseError : public std::runtime_error {
   public:
    explicit NalUnitHeaderParseError(const char* message) : std::runtime_error(message) {}

    explicit NalUnitHeaderParseError(const std::string& message) : std::runtime_error(message) {}
};

/*
 * -----------------------------------------------------------
 * Reader helpers
 * -----------------------------------------------------------
 *
 * Keep these helpers independent of the concrete reader type.
 *
 * Your reader is expected to provide:
 *
 *     read_bits(width)
 *
 * returning an unsigned integral/proxy value.
 *
 * It should also provide:
 *
 *     read_flag()
 *
 * if available.
 *
 * We intentionally use read_bits(1) here so this parser works
 * with the same reader interface used by the SPS/PPS parsers.
 */

/*
 * Read one syntax flag.
 */
template <typename Reader>
[[nodiscard]]
inline bool read_nal_flag(Reader& reader) {
    return static_cast<bool>(reader.read_bits(1));
}

/*
 * Read an unsigned syntax field.
 */
template <typename Reader>
[[nodiscard]]
inline std::uint32_t read_nal_bits(Reader& reader, unsigned width) {
    if (width == 0) {
        return 0;
    }

    if (width > 32) {
        throw NalUnitHeaderParseError("NAL header: invalid field width");
    }

    const auto value = reader.read_bits(width);

    return static_cast<std::uint32_t>(value);
}

/*
 * -----------------------------------------------------------
 * Parse NAL unit header
 * -----------------------------------------------------------
 */

template <typename Reader>
[[nodiscard]]
inline NalUnitHeader parse_nal_unit_header(Reader& reader) {
    NalUnitHeader header{};

    /*
     * forbidden_zero_bit
     */
    header.forbidden_zero_bit = read_nal_flag(reader);

    /*
     * nal_unit_type
     *
     * u(6)
     */
    const auto nal_type = read_nal_bits(reader, 6);

    if (nal_type > 63) {
        throw NalUnitHeaderParseError("NAL header: invalid nal_unit_type");
    }

    header.nal_unit_type = nal_unit_type_from_value(static_cast<std::uint8_t>(nal_type));

    /*
     * nuh_layer_id
     *
     * u(6)
     */
    header.nuh_layer_id = static_cast<std::uint8_t>(read_nal_bits(reader, 6));

    /*
     * nuh_temporal_id_plus1
     *
     * u(3)
     *
     * Zero is forbidden.
     */
    header.nuh_temporal_id_plus1 = static_cast<std::uint8_t>(read_nal_bits(reader, 3));

    /*
     * forbidden_zero_bit must be zero.
     */
    if (header.forbidden_zero_bit) {
        throw NalUnitHeaderParseError("NAL header: forbidden_zero_bit is non-zero");
    }

    /*
     * nuh_temporal_id_plus1 must be in 1..7.
     */
    if (!header.valid_temporal_id()) {
        throw NalUnitHeaderParseError("NAL header: nuh_temporal_id_plus1 is zero");
    }

    return header;
}

/*
 * -----------------------------------------------------------
 * Non-throwing parse variant
 * -----------------------------------------------------------
 *
 * Useful when the caller wants to perform error handling
 * without exceptions.
 */

template <typename Reader>
[[nodiscard]]
inline bool try_parse_nal_unit_header(Reader& reader, NalUnitHeader& header) {
    try {
        header = parse_nal_unit_header(reader);

        return true;
    } catch (const NalUnitHeaderParseError&) {
        return false;
    }
}

/*
 * -----------------------------------------------------------
 * Parse from the two raw header bytes
 * -----------------------------------------------------------
 *
 * Useful when the NAL parser has already extracted the first
 * two bytes without constructing a bitstream reader.
 *
 * big-endian representation:
 *
 *     bytes[0] << 8 | bytes[1]
 */

[[nodiscard]]
constexpr NalUnitHeader parse_nal_unit_header_bytes(
    std::uint8_t first_byte, std::uint8_t second_byte
) {
    const auto raw = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(first_byte) << 8) | static_cast<std::uint16_t>(second_byte)
    );

    return unpack_nal_unit_header(raw);
}

/*
 * -----------------------------------------------------------
 * Validation-only helper
 * -----------------------------------------------------------
 */

[[nodiscard]]
constexpr bool valid_nal_unit_header(const NalUnitHeader& header) noexcept {
    return validate_nal_unit_header(header);
}

/*
 * -----------------------------------------------------------
 * Convenience accessors
 * -----------------------------------------------------------
 */

[[nodiscard]]
constexpr std::uint8_t nal_unit_type(const NalUnitHeader& header) noexcept {
    return header.nal_type();
}

[[nodiscard]]
constexpr std::uint8_t nal_temporal_id(const NalUnitHeader& header) noexcept {
    return header.temporal_id();
}

[[nodiscard]]
constexpr std::uint8_t nal_layer_id(const NalUnitHeader& header) noexcept {
    return header.nuh_layer_id;
}

}  // namespace bs