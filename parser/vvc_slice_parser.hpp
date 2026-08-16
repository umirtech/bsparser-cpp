#pragma once

#include "vvc_ph.hpp"
#include "vvc_slice.hpp"
#include "vvc_sps.hpp"

#include <cstdint>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC slice segment header parser (H.266 §7.3.4.1)
 * -----------------------------------------------------------
 * Reads the leading fields, including the picture header (which may be
 * embedded in the slice header via sh_picture_header_in_slice_header_flag),
 * up to and including sh_slice_type.
 *
 * The embedded PH needs the SPS (POC LSB width / msb-cycle), so the dispatch
 * calls this twice: once with a null SPS to obtain pps_id, then again with
 * the resolved SPS.
 */
template <typename Reader>
[[nodiscard]]
inline SliceHeader parse_slice_header(
    Reader& r, const SequenceParameterSet* sps, const PictureParameterSet* pps
) {
    (void)pps;

    SliceHeader sh;

    sh.picture_header_in_slice_header_flag = r.read_bit();

    if (sh.picture_header_in_slice_header_flag) {
        sh.ph = (sps != nullptr) ? parse_ph(r, sps) : parse_ph(r, nullptr);
        sh.pps_id = sh.ph.pps_id;

        if (sps == nullptr) {
            /* Cannot proceed without the SPS; caller resolves and re-parses. */
            return sh;
        }
    }

    if (sps != nullptr) {
        if (sps->subpic_info_present_flag) {
            (void)r.read_bits(sps->subpic_id_len_minus1 + 1); /* sh_subpic_id */
        }

        /* sh_slice_address: omitted for the common single-tile picture. */

        for (std::uint32_t i = 0; i < sps->num_extra_sh_bits; ++i) {
            (void)r.read_bit(); /* sh_extra_bit */
        }

        if (sh.ph.inter_slice_allowed_flag) {
            const std::uint32_t raw = r.read_ue();
            sh.slice_type = (raw <= 2u) ? static_cast<SliceType>(raw) : SliceType::I;
        }
    }

    return sh;
}

}  // namespace vvc
}  // namespace bs
