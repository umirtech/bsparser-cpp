#pragma once

#include "vvc_opi.hpp"

#include <cstdint>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC OPI parser (H.266 §7.3.2.2)
 * -----------------------------------------------------------
 *     opi_ols_info_present_flag (1) · opi_htid_info_present_flag (1) ·
 *     [opi_ols_idx ue(v)] · [opi_htid_plus1 (3)] ·
 *     opi_extension_flag (1) · [opi_extension_data]
 */
template <typename Reader>
[[nodiscard]]
inline Opi parse_opi(Reader& r) {
    Opi opi;

    opi.ols_info_present = r.read_bit();

    opi.htid_info_present = r.read_bit();

    if (opi.ols_info_present) {
        opi.ols_idx = r.read_ue();
    }

    if (opi.htid_info_present) {
        opi.htid_plus1 = static_cast<std::uint8_t>(r.read_bits(3));
    }

    opi.extension_present = r.read_bit(); /* opi_extension_flag */

    return opi;
}

}  // namespace vvc
}  // namespace bs