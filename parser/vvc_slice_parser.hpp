#pragma once

#include "vvc_slice.hpp"

#include <cstdint>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC slice segment header parser (H.266 §7.3.4.1)
 * -----------------------------------------------------------
 * Leading fields for the common single-tile stream: pps_id then
 * slice_type (both ue(v)). Tile-address and redundant-POC fields
 * are not modelled.
 */
template <typename Reader>
[[nodiscard]]
inline SliceHeader
parse_slice_header(Reader& r)
{
    SliceHeader sh;

    sh.pps_id = r.read_ue();

    sh.slice_type =
        static_cast<SliceType>(r.read_ue());

    return sh;
}

} // namespace vvc
} // namespace bs
