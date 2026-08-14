#pragma once

#include "vvc_sps.hpp"

#include <cstdint>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC SPS parser (H.266 §7.3.2.4) — leading fields
 * -----------------------------------------------------------
 */
template <typename Reader>
[[nodiscard]]
inline SequenceParameterSet
parse_sps(Reader& r)
{
    SequenceParameterSet sps;

    sps.sps_id =
        static_cast<std::uint8_t>(r.read_bits(4));

    sps.vps_id =
        static_cast<std::uint8_t>(r.read_bits(4));

    sps.max_sublayers_minus1 =
        static_cast<std::uint8_t>(r.read_bits(3));

    sps.chroma_format_idc =
        static_cast<std::uint8_t>(r.read_bits(2));

    sps.log2_ctu_size_minus5 =
        static_cast<std::uint8_t>(r.read_bits(2));

    sps.ptl_dpb_hrd_params_present = r.read_bit();

    return sps;
}

} // namespace vvc
} // namespace bs
