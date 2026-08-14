#pragma once

#include <cstdint>

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
    return type >= 7 && type <= 11;
}

[[nodiscard]]
inline bool is_idr_nal_unit(std::uint8_t type) noexcept {
    return type == 7 || type == 8;
}

}  // namespace vvc
}  // namespace bs
