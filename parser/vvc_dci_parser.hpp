#pragma once

#include "vvc_dci.hpp"

#include <cstdint>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC DCI parser (H.266 §7.3.2.1)
 * -----------------------------------------------------------
 */
template <typename Reader>
[[nodiscard]]
inline Dci parse_dci(Reader& r) {
    Dci dci;

    (void)r.read_bit(); /* dci_reserved_zero_bit */

    dci.num_sps = r.read_bits(8);

    dci.sps_ids.reserve(dci.num_sps + 1u);

    for (std::uint32_t i = 0; i <= dci.num_sps; ++i) {
        dci.sps_ids.push_back(static_cast<std::uint8_t>(r.read_bits(4)));
    }

    dci.bit_rate_present = r.read_bit();
    dci.pic_rate_present = r.read_bit();

    return dci;
}

}  // namespace vvc
}  // namespace bs
