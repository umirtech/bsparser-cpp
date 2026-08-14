#pragma once

#include "vvc_pps.hpp"

#include <cstdint>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC PPS parser (H.266 §7.3.2.5) — leading fields
 * -----------------------------------------------------------
 */
template <typename Reader>
[[nodiscard]]
inline PictureParameterSet
parse_pps(Reader& r)
{
    PictureParameterSet pps;

    pps.pps_id =
        static_cast<std::uint8_t>(r.read_bits(6));

    pps.sps_id =
        static_cast<std::uint8_t>(r.read_bits(4));

    pps.mixed_nalu_types_in_pic = r.read_bit();

    pps.pic_width_in_luma_samples =
        r.read_ue();

    pps.pic_height_in_luma_samples =
        r.read_ue();

    return pps;
}

} // namespace vvc
} // namespace bs
