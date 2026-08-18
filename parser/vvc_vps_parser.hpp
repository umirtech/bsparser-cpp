#pragma once

#include "vvc_vps.hpp"

#include <cstdint>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC VPS parser (H.266 §7.3.2.3) — leading fields
 * -----------------------------------------------------------
 */
template <typename Reader>
[[nodiscard]]
inline VideoParameterSet parse_vps(Reader& r) {
    VideoParameterSet vps;

    vps.vps_id = static_cast<std::uint8_t>(r.read_bits(4));

    vps.max_layers_minus1 = static_cast<std::uint8_t>(r.read_bits(6));

    vps.max_sublayers_minus1 = static_cast<std::uint8_t>(r.read_bits(3));

    if (vps.max_layers_minus1 > 0 && vps.max_sublayers_minus1 > 0) {
        vps.default_ptl_dpb_hrd_max_tid_flag = r.read_bit();
    }

    if (vps.max_layers_minus1 > 0) {
        vps.all_independent_layers = r.read_bit();
    }

    vps.layer_ids.reserve(static_cast<std::size_t>(vps.max_layers_minus1) + 1u);

    for (std::uint8_t i = 0; i <= vps.max_layers_minus1; ++i) {
        vps.layer_ids.push_back(static_cast<std::uint8_t>(r.read_bits(6)));
    }

    return vps;
}

}  // namespace vvc
}  // namespace bs