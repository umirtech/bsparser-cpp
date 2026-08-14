#pragma once

#include "vvc_opi.hpp"

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC OPI parser (H.266 §7.3.2.2)
 * -----------------------------------------------------------
 */
template <typename Reader>
[[nodiscard]]
inline Opi
parse_opi(Reader& r)
{
    Opi opi;

    opi.ols_info_present = r.read_bit();
    opi.ptl_present = r.read_bit();

    return opi;
}

} // namespace vvc
} // namespace bs
