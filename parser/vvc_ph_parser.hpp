#pragma once

#include "vvc_ph.hpp"
#include "vvc_sps.hpp"

#include <cstdint>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC Picture Header parser (H.266 §7.3.2.8)
 * -----------------------------------------------------------
 * Parses the leading fields up to and including the picture-order-count
 * configuration.
 *
 * The POC LSB width, the extra-PH-bit count and the msb-cycle length all come
 * from the SPS referenced by pps_id, which is decoded inside the header.  The
 * dispatch therefore calls this twice: once with a null SPS to obtain pps_id,
 * then again (fresh reader) with the resolved SPS to read the POC fields.
 */
template <typename Reader>
[[nodiscard]]
inline PictureHeader parse_ph(Reader& r, const SequenceParameterSet* sps) {
    PictureHeader ph;

    ph.gdr_or_irap_pic_flag = r.read_bit();

    ph.non_ref_pic_flag = r.read_bit();

    if (ph.gdr_or_irap_pic_flag) {
        ph.gdr_pic_flag = r.read_bit();
    }

    ph.inter_slice_allowed_flag = r.read_bit();

    if (ph.inter_slice_allowed_flag) {
        ph.intra_slice_allowed_flag = r.read_bit();
    }

    ph.pps_id = static_cast<std::uint8_t>(r.read_ue());

    if (sps == nullptr) {
        return ph;
    }

    const std::uint32_t poc_lsb_bits =
        static_cast<std::uint32_t>(sps->log2_max_pic_order_cnt_lsb_minus4) + 4;

    ph.poc_lsb = r.read_bits(poc_lsb_bits);

    ph.poc_lsb_bits = poc_lsb_bits;

    if (ph.gdr_pic_flag) {
        ph.recovery_poc_cnt = r.read_ue();
    }

    for (std::uint32_t i = 0; i < sps->num_extra_ph_bits; ++i) {
        (void)r.read_bit(); /* ph_extra_bit */
    }

    if (sps->poc_msb_cycle_flag) {
        ph.poc_msb_cycle_present_flag = r.read_bit();
        if (ph.poc_msb_cycle_present_flag) {
            ph.poc_msb_cycle_val =
                r.read_bits(static_cast<std::uint32_t>(sps->poc_msb_cycle_len_minus1) + 1);
        }
    }

    return ph;
}

/*
 * Single-pass convenience (no POC fields): reads just the leading fields.
 */
template <typename Reader>
[[nodiscard]]
inline PictureHeader parse_ph(Reader& r) {
    return parse_ph(r, nullptr);
}

}  // namespace vvc
}  // namespace bs
