// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <cstdint>
#include <vector>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC (H.266) NAL unit types (H.266 Table 7-1)
 * -----------------------------------------------------------
 */
enum class NalUnitType : std::uint8_t {
    TraiNut = 0,
    StsaNut = 1,
    RadlNut = 2,
    RaslNut = 3,
    ReservedVcl4 = 4,
    ReservedVcl5 = 5,
    ReservedVcl6 = 6,
    IdrWRadl = 7,
    IdrNLp = 8,
    CraNut = 9,
    GdrNut = 10,
    ReservedIrap11 = 11,
    OpiNut = 12,
    DciNut = 13,
    VpsNut = 14,
    SpsNut = 15,
    PpsNut = 16,
    PrefixApsNut = 17,
    SuffixApsNut = 18,
    PhNut = 19,
    AudNut = 20,
    EosNut = 21,
    EobNut = 22,
    SeiPrefixNut = 23,
    SeiSuffixNut = 24,
    FdNut = 25,
    ReservedNvcl26 = 26,
    ReservedNvcl27 = 27
};

[[nodiscard]]
inline bool is_vcl_nal_unit(std::uint8_t type) noexcept {
    return type <= 11;
}

[[nodiscard]]
inline bool is_irap_nal_unit(std::uint8_t type) noexcept {
    /* IRAP = IDR_W_RADL(7), IDR_N_LP(8), CRA(9), RSV_IRAP(11).
     * GDR_NUT(10) is not an IRAP. */
    return (type >= 7 && type <= 9) || type == 11;
}

[[nodiscard]]
inline bool is_idr_nal_unit(std::uint8_t type) noexcept {
    return type == 7 || type == 8;
}

/*
 * profile_tier_level() (H.266 7.3.3.1) — the scalar values retained from
 * DCI / VPS / SPS.  The constraint-info bit pattern is validated but not
 * stored; see vvc_sps_parser.hpp.
 */
struct VvcProfileTierLevel {
    std::uint8_t general_profile_idc = 0;
    bool general_tier_flag = false;
    std::uint8_t general_level_idc = 0;
    bool ptl_frame_only_constraint_flag = false;
    bool ptl_multilayer_enabled_flag = false;
    bool sublayer_level_present[8] = {};
    std::uint8_t sublayer_level_idc[8] = {};
    std::uint32_t num_sub_profiles = 0;
    std::vector<std::uint32_t> sub_profile_ids;
};

}  // namespace vvc
}  // namespace bs
