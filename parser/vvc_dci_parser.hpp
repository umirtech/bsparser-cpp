// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "vvc_dci.hpp"
#include "vvc_sps_parser.hpp"

#include <cstdint>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC DCI parser (H.266 §7.3.2.1)
 * -----------------------------------------------------------
 *     dci_reserved_zero_4bits (4) · dci_num_ptls_minus1 (4) ·
 *     profile_tier_level(1, 0) × (num_ptls_minus1 + 1) ·
 *     dci_extension_flag (1) · [dci_extension_data]
 *
 * The profile_tier_level() blocks are skipped; only the leading
 * fields are kept.
 */
template <typename Reader>
[[nodiscard]]
inline Dci parse_dci(Reader& r) {
    Dci dci;

    (void)r.read_bits(4); /* dci_reserved_zero_4bits */

    dci.num_ptls_minus1 = r.read_bits(4);

    for (std::uint32_t i = 0; i <= dci.num_ptls_minus1; ++i) {
        detail::skip_profile_tier_level(r, 0);
    }

    dci.extension_present = r.read_bit(); /* dci_extension_flag */

    return dci;
}

}  // namespace vvc
}  // namespace bs
