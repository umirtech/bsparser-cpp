#pragma once

#include <cstdint>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC Sequence Parameter Set (SPS)
 * -----------------------------------------------------------
 * H.266 §7.3.2.4.  Parsed through the picture-order-count configuration
 * (sps_log2_max_pic_order_cnt_lsb_minus4 / sps_poc_msb_cycle) which gates the
 * picture-header POC derivation.
 */
struct SequenceParameterSet {
    std::uint8_t sps_id = 0;

    std::uint8_t vps_id = 0;

    std::uint8_t max_sublayers_minus1 = 0;

    std::uint8_t chroma_format_idc = 0;

    std::uint8_t log2_ctu_size_minus5 = 0;

    bool ptl_dpb_hrd_params_present = false;

    bool gdr_enabled_flag = false;

    bool ref_pic_resampling_enabled_flag = false;

    bool subpic_info_present_flag = false;

    std::uint8_t subpic_id_len_minus1 = 0;

    /*
     * Number of extra slice-header bits (NumExtraShBits).
     */
    std::uint8_t num_extra_sh_bits = 0;

    /*
     * -------------------------------------------------------
     * Picture order count configuration (§7.3.2.4)
     * -------------------------------------------------------
     */
    std::uint8_t log2_max_pic_order_cnt_lsb_minus4 = 0;

    bool poc_msb_cycle_flag = false;

    std::uint8_t poc_msb_cycle_len_minus1 = 0;

    /*
     * Number of extra PH bits (NumExtraPhBits), derived from
     * sps_num_extra_ph_bytes / sps_extra_ph_bit_present_flag.
     */
    std::uint8_t num_extra_ph_bits = 0;

    [[nodiscard]]
    std::uint32_t max_pic_order_cnt_lsb() const noexcept {
        return std::uint32_t{1} << (log2_max_pic_order_cnt_lsb_minus4 + 4);
    }

    [[nodiscard]]
    std::uint32_t ctb_size() const noexcept {
        return std::uint32_t{1} << (log2_ctu_size_minus5 + 5);
    }

    [[nodiscard]]
    bool valid() const noexcept {
        return sps_id <= 15 && chroma_format_idc <= 3;
    }
};

}  // namespace vvc
}  // namespace bs
