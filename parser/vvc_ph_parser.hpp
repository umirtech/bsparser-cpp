#pragma once

#include "vvc_ph.hpp"

#include <cstdint>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC Picture Header parser (H.266 §7.3.2.6) — leading fields
 * -----------------------------------------------------------
 * Reads the leading pps_id; the POC LSB requires the SPS-derived
 * bit width, so it is not populated here.
 */
template <typename Reader>
[[nodiscard]]
inline PictureHeader parse_ph(Reader& r) {
    PictureHeader ph;

    ph.pps_id = static_cast<std::uint8_t>(r.read_bits(6));

    return ph;
}

}  // namespace vvc
}  // namespace bs
