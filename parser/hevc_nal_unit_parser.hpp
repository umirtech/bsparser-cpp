// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "hevc_nal_unit.hpp"
#include "rbsp_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

namespace bs {

/*
 * -----------------------------------------------------------
 * NAL unit parser error
 * -----------------------------------------------------------
 */

class NalUnitParseError : public std::runtime_error {
   public:
    explicit NalUnitParseError(const char* message) : std::runtime_error(message) {}

    explicit NalUnitParseError(const std::string& message) : std::runtime_error(message) {}
};

/*
 * -----------------------------------------------------------
 * NAL unit span
 * -----------------------------------------------------------
 *
 * The input represents exactly one complete NAL unit:
 *
 *     byte 0   NAL header
 *     byte 1   NAL header
 *     byte 2+  EBSP payload
 *
 * This layer does NOT understand:
 *
 *     Annex-B start codes
 *     length prefixes
 *
 * Those belong to the framing layer.
 */

using NalUnitSpan = std::span<const std::uint8_t>;

/*
 * -----------------------------------------------------------
 * Parse a complete NAL unit
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline NalUnit parse_nal_unit(NalUnitSpan bytes) {
    if (bytes.size() < 2) {
        throw NalUnitParseError("NAL unit: truncated NAL header");
    }

    /*
     * HEVC NAL header is exactly two bytes.
     *
     * Interpret the bytes in network/bitstream order.
     */
    const std::uint16_t raw_header = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[0]) << 8) | static_cast<std::uint16_t>(bytes[1])
    );

    const NalUnitHeader header = unpack_nal_unit_header(raw_header);

    /*
     * forbidden_zero_bit must be zero.
     */
    if (header.forbidden_zero_bit) {
        throw NalUnitParseError("NAL unit: forbidden_zero_bit is non-zero");
    }

    /*
     * nuh_temporal_id_plus1 must be 1..7.
     */
    if (!header.valid_temporal_id()) {
        throw NalUnitParseError("NAL unit: invalid nuh_temporal_id_plus1");
    }

    /*
     * Everything following the two-byte header is the EBSP
     * payload.
     *
     * No allocation.
     * No copy.
     */
    const auto payload = bytes.subspan(2);

    return make_nal_unit(header, payload);
}

/*
 * -----------------------------------------------------------
 * Non-throwing variant
 * -----------------------------------------------------------
 */
[[nodiscard]]
inline bool try_parse_nal_unit(NalUnitSpan bytes, NalUnit& output) {
    try {
        output = parse_nal_unit(bytes);
        return true;
    } catch (const NalUnitParseError&) {
        return false;
    }
}

/*
 * -----------------------------------------------------------
 * RBSP reader creation
 * -----------------------------------------------------------
 *
 * The NAL payload is EBSP.
 *
 * RbspReader handles emulation-prevention bytes logically,
 * so this remains zero-copy.
 */

[[nodiscard]]
inline RbspReader make_nal_rbsp_reader(const NalUnit& nal) {
    return make_rbsp_reader(nal.payload_bytes());
}

/*
 * -----------------------------------------------------------
 * Convenience parse result
 * -----------------------------------------------------------
 *
 * Useful when the caller wants both objects immediately.
 */

struct ParsedNalUnit {
    NalUnit nal{};

    /*
     * The reader is intentionally not stored here.
     *
     * RbspReader is mutable parser state and is normally
     * created for the particular syntax parser being invoked.
     */
};

/*
 * -----------------------------------------------------------
 * NAL type dispatch predicates
 * -----------------------------------------------------------
 */

[[nodiscard]]
constexpr bool is_vps_nal(const NalUnit& nal) noexcept {
    return nal.type() == NalUnitType::VPS_NUT;
}

[[nodiscard]]
constexpr bool is_sps_nal(const NalUnit& nal) noexcept {
    return nal.type() == NalUnitType::SPS_NUT;
}

[[nodiscard]]
constexpr bool is_pps_nal(const NalUnit& nal) noexcept {
    return nal.type() == NalUnitType::PPS_NUT;
}

[[nodiscard]]
constexpr bool is_prefix_sei_nal(const NalUnit& nal) noexcept {
    return nal.type() == NalUnitType::PREFIX_SEI_NUT;
}

[[nodiscard]]
constexpr bool is_suffix_sei_nal(const NalUnit& nal) noexcept {
    return nal.type() == NalUnitType::SUFFIX_SEI_NUT;
}

[[nodiscard]]
constexpr bool is_slice_nal(const NalUnit& nal) noexcept {
    return is_vcl_nal_unit(nal.type());
}

/*
 * -----------------------------------------------------------
 * NAL classification
 * -----------------------------------------------------------
 */

enum class NalSyntaxKind : std::uint8_t {
    Vps,
    Sps,
    Pps,
    PrefixSei,
    SuffixSei,
    Slice,
    AccessUnitDelimiter,
    EndOfSequence,
    EndOfBitstream,
    Filler,
    Other
};

[[nodiscard]]
constexpr NalSyntaxKind classify_nal(NalUnitType type) noexcept {
    switch (type) {
        case NalUnitType::VPS_NUT:
            return NalSyntaxKind::Vps;

        case NalUnitType::SPS_NUT:
            return NalSyntaxKind::Sps;

        case NalUnitType::PPS_NUT:
            return NalSyntaxKind::Pps;

        case NalUnitType::PREFIX_SEI_NUT:
            return NalSyntaxKind::PrefixSei;

        case NalUnitType::SUFFIX_SEI_NUT:
            return NalSyntaxKind::SuffixSei;

        case NalUnitType::AUD_NUT:
            return NalSyntaxKind::AccessUnitDelimiter;

        case NalUnitType::EOS_NUT:
            return NalSyntaxKind::EndOfSequence;

        case NalUnitType::EOB_NUT:
            return NalSyntaxKind::EndOfBitstream;

        case NalUnitType::FD_NUT:
            return NalSyntaxKind::Filler;

        default:
            if (is_vcl_nal_unit(type)) {
                return NalSyntaxKind::Slice;
            }

            return NalSyntaxKind::Other;
    }
}

[[nodiscard]]
constexpr NalSyntaxKind classify_nal(const NalUnit& nal) noexcept {
    return classify_nal(nal.type());
}

/*
 * -----------------------------------------------------------
 * RBSP parser helper
 * -----------------------------------------------------------
 *
 * This provides the common pattern used by all syntax
 * parsers:
 *
 *     parse_nal_unit()
 *          ↓
 *     make_nal_rbsp_reader()
 *          ↓
 *     syntax parser
 */

template <typename Parser>
decltype(auto) parse_nal_rbsp(const NalUnit& nal, Parser&& parser) {
    auto reader = make_nal_rbsp_reader(nal);

    return parser(reader);
}

/*
 * -----------------------------------------------------------
 * Payload access
 * -----------------------------------------------------------
 */

[[nodiscard]]
constexpr std::span<const std::uint8_t> nal_ebsp_payload(const NalUnit& nal) noexcept {
    return nal.payload_bytes();
}

[[nodiscard]]
constexpr std::size_t nal_ebsp_payload_size(const NalUnit& nal) noexcept {
    return nal.payload_size();
}

/*
 * -----------------------------------------------------------
 * Basic structural validation
 * -----------------------------------------------------------
 */

[[nodiscard]]
constexpr bool validate_nal_unit(const NalUnit& nal) noexcept {
    if (!nal.header.valid()) {
        return false;
    }

    /*
     * A NAL header is always two bytes, but the payload is
     * allowed to be empty at this structural layer.
     */
    return true;
}

}  // namespace bs