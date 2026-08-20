// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <cstdint>

namespace bs {

/*
 * H.265 / HEVC NAL unit header
 *
 * 7.3.1
 *
 * nal_unit_header() {
 *
 *     forbidden_zero_bit        u(1)
 *     nal_unit_type             u(6)
 *     nuh_layer_id              u(6)
 *     nuh_temporal_id_plus1     u(3)
 *
 * }
 *
 * Total:
 *
 *     1 + 6 + 6 + 3 = 16 bits
 *
 * The structure stores the decoded syntax values.
 *
 * It intentionally does not contain the two raw bytes.
 */

/*
 * -----------------------------------------------------------
 * NAL unit type
 * -----------------------------------------------------------
 */

enum class NalUnitType : std::uint8_t {

    TRAIL_N = 0,
    TRAIL_R = 1,

    TSA_N = 2,
    TSA_R = 3,

    STSA_N = 4,
    STSA_R = 5,

    RADL_N = 6,
    RADL_R = 7,

    RASL_N = 8,
    RASL_R = 9,

    RSV_VCL_N10 = 10,
    RSV_VCL_R11 = 11,
    RSV_VCL_N12 = 12,
    RSV_VCL_R13 = 13,
    RSV_VCL_N14 = 14,
    RSV_VCL_R15 = 15,

    BLA_W_LP = 16,
    BLA_W_RADL = 17,
    BLA_N_LP = 18,

    IDR_W_RADL = 19,
    IDR_N_LP = 20,

    CRA_NUT = 21,

    RSV_IRAP_VCL22 = 22,
    RSV_IRAP_VCL23 = 23,

    RSV_VCL24 = 24,
    RSV_VCL25 = 25,
    RSV_VCL26 = 26,
    RSV_VCL27 = 27,
    RSV_VCL28 = 28,
    RSV_VCL29 = 29,
    RSV_VCL30 = 30,
    RSV_VCL31 = 31,

    VPS_NUT = 32,
    SPS_NUT = 33,
    PPS_NUT = 34,

    AUD_NUT = 35,
    EOS_NUT = 36,
    EOB_NUT = 37,
    FD_NUT = 38,

    PREFIX_SEI_NUT = 39,
    SUFFIX_SEI_NUT = 40,

    RSV_NVCL41 = 41,
    RSV_NVCL42 = 42,
    RSV_NVCL43 = 43,
    RSV_NVCL44 = 44,
    RSV_NVCL45 = 45,
    RSV_NVCL46 = 46,
    RSV_NVCL47 = 47,

    UNSPEC48 = 48,
    UNSPEC49 = 49,
    UNSPEC50 = 50,
    UNSPEC51 = 51,
    UNSPEC52 = 52,
    UNSPEC53 = 53,
    UNSPEC54 = 54,
    UNSPEC55 = 55,
    UNSPEC56 = 56,
    UNSPEC57 = 57,
    UNSPEC58 = 58,
    UNSPEC59 = 59,
    UNSPEC60 = 60,
    UNSPEC61 = 61,
    UNSPEC62 = 62,
    UNSPEC63 = 63
};

/*
 * -----------------------------------------------------------
 * NAL header
 * -----------------------------------------------------------
 */

struct NalUnitHeader {
    /*
     * forbidden_zero_bit
     *
     * This must always be zero for a conforming HEVC stream.
     */
    bool forbidden_zero_bit = false;

    /*
     * nal_unit_type
     *
     * u(6)
     */
    NalUnitType nal_unit_type = NalUnitType::TRAIL_N;

    /*
     * nuh_layer_id
     *
     * u(6)
     *
     * Valid range:
     *
     *     0 .. 63
     */
    std::uint8_t nuh_layer_id = 0;

    /*
     * nuh_temporal_id_plus1
     *
     * u(3)
     *
     * Valid range:
     *
     *     1 .. 7
     *
     * The actual temporal_id is:
     *
     *     nuh_temporal_id_plus1 - 1
     */
    std::uint8_t nuh_temporal_id_plus1 = 1;

    /*
     * -------------------------------------------------------
     * Helpers
     * -------------------------------------------------------
     */

    [[nodiscard]]
    constexpr std::uint8_t nal_type() const noexcept {
        return static_cast<std::uint8_t>(nal_unit_type);
    }

    [[nodiscard]]
    constexpr std::uint8_t temporal_id() const noexcept {
        if (nuh_temporal_id_plus1 == 0) {
            return 0;
        }

        return static_cast<std::uint8_t>(nuh_temporal_id_plus1 - 1);
    }

    [[nodiscard]]
    constexpr bool valid_temporal_id() const noexcept {
        return nuh_temporal_id_plus1 >= 1 && nuh_temporal_id_plus1 <= 7;
    }

    [[nodiscard]]
    constexpr bool valid_layer_id() const noexcept {
        return nuh_layer_id <= 63;
    }

    [[nodiscard]]
    constexpr bool valid() const noexcept {
        return !forbidden_zero_bit && valid_temporal_id() && valid_layer_id();
    }
};

/*
 * -----------------------------------------------------------
 * NAL classification helpers
 * -----------------------------------------------------------
 */

/*
 * VCL NAL units occupy types 0..31.
 */
[[nodiscard]]
constexpr bool is_vcl_nal_unit(NalUnitType type) noexcept {
    return static_cast<std::uint8_t>(type) <= 31;
}

[[nodiscard]]
constexpr bool is_vcl_nal_unit(std::uint8_t type) noexcept {
    return type <= 31;
}

/*
 * Non-VCL NAL units.
 */
[[nodiscard]]
constexpr bool is_non_vcl_nal_unit(NalUnitType type) noexcept {
    return !is_vcl_nal_unit(type);
}

/*
 * IRAP VCL NAL units:
 *
 *     BLA_W_LP
 *     BLA_W_RADL
 *     BLA_N_LP
 *     IDR_W_RADL
 *     IDR_N_LP
 *     CRA_NUT
 *
 * Types 16..21.
 */
[[nodiscard]]
constexpr bool is_irap_nal_unit(NalUnitType type) noexcept {
    const auto value = static_cast<std::uint8_t>(type);

    return value >= 16 && value <= 21;
}

[[nodiscard]]
constexpr bool is_irap_nal_unit(std::uint8_t type) noexcept {
    return type >= 16 && type <= 21;
}

/*
 * IDR pictures.
 */
[[nodiscard]]
constexpr bool is_idr_nal_unit(NalUnitType type) noexcept {
    return type == NalUnitType::IDR_W_RADL || type == NalUnitType::IDR_N_LP;
}

[[nodiscard]]
constexpr bool is_idr_nal_unit(std::uint8_t type) noexcept {
    return type == 19 || type == 20;
}

/*
 * CRA picture.
 */
[[nodiscard]]
constexpr bool is_cra_nal_unit(NalUnitType type) noexcept {
    return type == NalUnitType::CRA_NUT;
}

[[nodiscard]]
constexpr bool is_cra_nal_unit(std::uint8_t type) noexcept {
    return type == 21;
}

/*
 * BLA picture.
 */
[[nodiscard]]
constexpr bool is_bla_nal_unit(NalUnitType type) noexcept {
    const auto value = static_cast<std::uint8_t>(type);

    return value >= 16 && value <= 18;
}

/*
 * RADL picture.
 */
[[nodiscard]]
constexpr bool is_radl_nal_unit(NalUnitType type) noexcept {
    return type == NalUnitType::RADL_N || type == NalUnitType::RADL_R;
}

/*
 * RASL picture.
 */
[[nodiscard]]
constexpr bool is_rasl_nal_unit(NalUnitType type) noexcept {
    return type == NalUnitType::RASL_N || type == NalUnitType::RASL_R;
}

/*
 * Reference-picture VCL NAL.
 *
 * The *_N variants are non-reference pictures.
 *
 * For the standard VCL types:
 *
 *     even  -> non-reference
 *     odd   -> reference
 *
 * except that the IRAP range follows the same convention.
 */
[[nodiscard]]
constexpr bool is_reference_vcl_nal_unit(NalUnitType type) noexcept {
    const auto value = static_cast<std::uint8_t>(type);

    if (value > 31) {
        return false;
    }

    return (value & 1U) != 0;
}

[[nodiscard]]
constexpr bool is_reference_vcl_nal_unit(std::uint8_t type) noexcept {
    if (type > 31) {
        return false;
    }

    return (type & 1U) != 0;
}

/*
 * -----------------------------------------------------------
 * Common parameter-set NAL types
 * -----------------------------------------------------------
 */

[[nodiscard]]
constexpr bool is_vps_nal_unit(NalUnitType type) noexcept {
    return type == NalUnitType::VPS_NUT;
}

[[nodiscard]]
constexpr bool is_sps_nal_unit(NalUnitType type) noexcept {
    return type == NalUnitType::SPS_NUT;
}

[[nodiscard]]
constexpr bool is_pps_nal_unit(NalUnitType type) noexcept {
    return type == NalUnitType::PPS_NUT;
}

[[nodiscard]]
constexpr bool is_sei_nal_unit(NalUnitType type) noexcept {
    return type == NalUnitType::PREFIX_SEI_NUT || type == NalUnitType::SUFFIX_SEI_NUT;
}

/*
 * -----------------------------------------------------------
 * NAL type conversion
 * -----------------------------------------------------------
 */

[[nodiscard]]
constexpr NalUnitType nal_unit_type_from_value(std::uint8_t value) noexcept {
    return static_cast<NalUnitType>(value & 0x3FU);
}

/*
 * -----------------------------------------------------------
 * Raw 16-bit representation
 * -----------------------------------------------------------
 *
 * The NAL header occupies exactly two bytes.
 *
 * Bit layout:
 *
 *     bit 15       forbidden_zero_bit
 *     bits 14..9   nal_unit_type
 *     bits 8..3    nuh_layer_id
 *     bits 2..0    nuh_temporal_id_plus1
 *
 * This helper is useful when a caller already has the two
 * header bytes available.
 */

[[nodiscard]]
constexpr std::uint16_t pack_nal_unit_header(const NalUnitHeader& header) noexcept {
    return (static_cast<std::uint16_t>(header.forbidden_zero_bit ? 1U : 0U) << 15) |

           (static_cast<std::uint16_t>(header.nal_type() & 0x3FU) << 9) |

           (static_cast<std::uint16_t>(header.nuh_layer_id & 0x3FU) << 3) |

           static_cast<std::uint16_t>(header.nuh_temporal_id_plus1 & 0x07U);
}

/*
 * Decode the 16-bit NAL header representation.
 *
 * The input is interpreted in HEVC bit order, with the first
 * header bit occupying bit 15.
 */
[[nodiscard]]
constexpr NalUnitHeader unpack_nal_unit_header(std::uint16_t value) noexcept {
    NalUnitHeader header{};

    header.forbidden_zero_bit = ((value >> 15) & 1U) != 0;

    header.nal_unit_type =
        nal_unit_type_from_value(static_cast<std::uint8_t>((value >> 9) & 0x3FU));

    header.nuh_layer_id = static_cast<std::uint8_t>((value >> 3) & 0x3FU);

    header.nuh_temporal_id_plus1 = static_cast<std::uint8_t>(value & 0x07U);

    return header;
}

/*
 * -----------------------------------------------------------
 * Raw byte helpers
 * -----------------------------------------------------------
 */

/*
 * Pack a NAL header into the two bytes used by HEVC.
 *
 * The first byte contains:
 *
 *     forbidden_zero_bit
 *     nal_unit_type
 *     top two layer-id bits
 *
 * The second byte contains:
 *
 *     lower four layer-id bits
 *     temporal-id-plus1
 */
[[nodiscard]]
constexpr std::uint16_t pack_nal_unit_header_bytes(const NalUnitHeader& header) noexcept {
    return pack_nal_unit_header(header);
}

/*
 * -----------------------------------------------------------
 * Validation
 * -----------------------------------------------------------
 */

[[nodiscard]]
constexpr bool validate_nal_unit_header(const NalUnitHeader& header) noexcept {
    if (header.forbidden_zero_bit) {
        return false;
    }

    if (!header.valid_temporal_id()) {
        return false;
    }

    if (!header.valid_layer_id()) {
        return false;
    }

    return true;
}

/*
 * -----------------------------------------------------------
 * Semantic predicates on the structure
 * -----------------------------------------------------------
 */

[[nodiscard]]
constexpr bool is_vcl(const NalUnitHeader& header) noexcept {
    return is_vcl_nal_unit(header.nal_unit_type);
}

[[nodiscard]]
constexpr bool is_irap(const NalUnitHeader& header) noexcept {
    return is_irap_nal_unit(header.nal_unit_type);
}

[[nodiscard]]
constexpr bool is_idr(const NalUnitHeader& header) noexcept {
    return is_idr_nal_unit(header.nal_unit_type);
}

[[nodiscard]]
constexpr bool is_cra(const NalUnitHeader& header) noexcept {
    return is_cra_nal_unit(header.nal_unit_type);
}

[[nodiscard]]
constexpr bool is_reference_picture(const NalUnitHeader& header) noexcept {
    return is_reference_vcl_nal_unit(header.nal_unit_type);
}

/*
 * -----------------------------------------------------------
 * Parameter-set classification
 * -----------------------------------------------------------
 */

[[nodiscard]]
constexpr bool is_parameter_set(const NalUnitHeader& header) noexcept {
    return header.nal_unit_type == NalUnitType::VPS_NUT ||
           header.nal_unit_type == NalUnitType::SPS_NUT ||
           header.nal_unit_type == NalUnitType::PPS_NUT;
}

}  // namespace bs