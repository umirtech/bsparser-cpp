#pragma once

#include "vvc_sps.hpp"

#include <cstdint>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC SPS parser (H.266 §7.3.2.4)
 * -----------------------------------------------------------
 * Walks the SPS RBSP through the picture-order-count configuration
 * (sps_log2_max_pic_order_cnt_lsb_minus4 / sps_poc_msb_cycle_flag), which
 * gates the picture-header POC derivation.  The intermediate profile_tier_
 * level() / general_constraints_info() and sub-picture syntax is consumed
 * (parsed for the values that matter, otherwise skipped).
 */

namespace detail {

/*
 * general_constraints_info() (§7.3.3.2).  ~90 one-bit flags followed by the
 * additional/reserved bit run.  Nothing here is kept for POC purposes.
 */
template <typename Reader>
inline void skip_general_constraints_info(Reader& r) {
    if (!r.read_bit()) {
        r.byte_align();
        return;
    }

    (void)r.read_bit();   /* gci_intra_only */
    (void)r.read_bit();   /* gci_all_layers_independent */
    (void)r.read_bit();   /* gci_one_au_only */
    (void)r.read_bits(4); /* gci_sixteen_minus_max_bitdepth */
    (void)r.read_bits(2); /* gci_three_minus_max_chroma_format */

    (void)r.read_bit(); /* gci_no_mixed_nalu_types_in_pic */
    (void)r.read_bit(); /* gci_no_trail */
    (void)r.read_bit(); /* gci_no_stsa */
    (void)r.read_bit(); /* gci_no_rasl */
    (void)r.read_bit(); /* gci_no_radl */
    (void)r.read_bit(); /* gci_no_idr */
    (void)r.read_bit(); /* gci_no_cra */
    (void)r.read_bit(); /* gci_no_gdr */
    (void)r.read_bit(); /* gci_no_aps */
    (void)r.read_bit(); /* gci_no_idr_rpl */

    (void)r.read_bit(); /* gci_one_tile_per_pic */
    (void)r.read_bit(); /* gci_pic_header_in_slice_header */
    (void)r.read_bit(); /* gci_one_slice_per_pic */
    (void)r.read_bit(); /* gci_no_rectangular_slice */
    (void)r.read_bit(); /* gci_one_slice_per_subpic */
    (void)r.read_bit(); /* gci_no_subpic_info */

    (void)r.read_bits(2); /* gci_three_minus_max_log2_ctu_size */
    (void)r.read_bit();   /* gci_no_partition_constraints_override */
    (void)r.read_bit();   /* gci_no_mtt */
    (void)r.read_bit();   /* gci_no_qtbtt_dual_tree_intra */

    (void)r.read_bit(); /* gci_no_palette */
    (void)r.read_bit(); /* gci_no_ibc */
    (void)r.read_bit(); /* gci_no_isp */
    (void)r.read_bit(); /* gci_no_mrl */
    (void)r.read_bit(); /* gci_no_mip */
    (void)r.read_bit(); /* gci_no_cclm */

    (void)r.read_bit(); /* gci_no_ref_pic_resampling */
    (void)r.read_bit(); /* gci_no_res_change_in_clvs */
    (void)r.read_bit(); /* gci_no_weighted_prediction */
    (void)r.read_bit(); /* gci_no_ref_wraparound */
    (void)r.read_bit(); /* gci_no_temporal_mvp */
    (void)r.read_bit(); /* gci_no_sbtmvp */
    (void)r.read_bit(); /* gci_no_amvr */
    (void)r.read_bit(); /* gci_no_bdof */
    (void)r.read_bit(); /* gci_no_smvd */
    (void)r.read_bit(); /* gci_no_dmvr */
    (void)r.read_bit(); /* gci_no_mmvd */
    (void)r.read_bit(); /* gci_no_affine_motion */
    (void)r.read_bit(); /* gci_no_prof */
    (void)r.read_bit(); /* gci_no_bcw */
    (void)r.read_bit(); /* gci_no_ciip */
    (void)r.read_bit(); /* gci_no_gpm */

    (void)r.read_bit(); /* gci_no_luma_transform_size_64 */
    (void)r.read_bit(); /* gci_no_transform_skip */
    (void)r.read_bit(); /* gci_no_bdpcm */
    (void)r.read_bit(); /* gci_no_mts */
    (void)r.read_bit(); /* gci_no_lfnst */
    (void)r.read_bit(); /* gci_no_joint_cbcr */
    (void)r.read_bit(); /* gci_no_sbt */
    (void)r.read_bit(); /* gci_no_act */
    (void)r.read_bit(); /* gci_no_explicit_scaling_list */
    (void)r.read_bit(); /* gci_no_dep_quant */
    (void)r.read_bit(); /* gci_no_sign_data_hiding */
    (void)r.read_bit(); /* gci_no_cu_qp_delta */
    (void)r.read_bit(); /* gci_no_chroma_qp_offset */

    (void)r.read_bit(); /* gci_no_sao */
    (void)r.read_bit(); /* gci_no_alf */
    (void)r.read_bit(); /* gci_no_ccalf */
    (void)r.read_bit(); /* gci_no_lmcs */
    (void)r.read_bit(); /* gci_no_ladf */
    (void)r.read_bit(); /* gci_no_virtual_boundaries */

    const std::uint32_t num_additional_bits = r.read_bits(8);

    std::uint32_t additional_bits_used = 0;
    if (num_additional_bits > 5) {
        (void)r.read_bit(); /* gci_all_rap_pictures */
        (void)r.read_bit(); /* gci_no_extended_precision_processing */
        (void)r.read_bit(); /* gci_no_ts_residual_coding_rice */
        (void)r.read_bit(); /* gci_no_rrc_rice_extension */
        (void)r.read_bit(); /* gci_no_persistent_rice_adaptation */
        (void)r.read_bit(); /* gci_no_reverse_last_sig_coeff */
        additional_bits_used = 6;
    }

    for (std::uint32_t i = additional_bits_used; i < num_additional_bits; ++i) {
        (void)r.read_bit(); /* gci_reserved_bit */
    }

    r.byte_align();
}

/*
 * profile_tier_level( profileTierPresentFlag, MaxNumSubLayersMinus1 )
 * (§7.3.3.1).  For the SPS call profileTierPresentFlag == 1.
 */
template <typename Reader>
inline void skip_profile_tier_level(Reader& r, std::uint8_t max_sub_layers_minus1) {
    (void)r.read_bits(7); /* general_profile_idc */
    (void)r.read_bit();   /* general_tier_flag */

    (void)r.read_bits(8); /* general_level_idc */
    (void)r.read_bit();   /* ptl_frame_only_constraint_flag */
    (void)r.read_bit();   /* ptl_multilayer_enabled_flag */

    skip_general_constraints_info(r);

    bool sublayer_level_present[8] = {};
    for (int i = max_sub_layers_minus1 - 1; i >= 0; --i) {
        sublayer_level_present[i] = r.read_bit();
    }

    r.byte_align();

    for (int i = max_sub_layers_minus1 - 1; i >= 0; --i) {
        if (sublayer_level_present[i]) {
            (void)r.read_bits(8); /* sublayer_level_idc */
        }
    }

    const std::uint32_t num_sub_profiles = r.read_bits(8);
    for (std::uint32_t i = 0; i < num_sub_profiles; ++i) {
        (void)r.read_bits(32); /* general_sub_profile_idc */
    }
}

}  // namespace detail

template <typename Reader>
[[nodiscard]]
inline SequenceParameterSet parse_sps(Reader& r) {
    SequenceParameterSet sps;

    sps.sps_id = static_cast<std::uint8_t>(r.read_bits(4));

    sps.vps_id = static_cast<std::uint8_t>(r.read_bits(4));

    sps.max_sublayers_minus1 = static_cast<std::uint8_t>(r.read_bits(3));

    sps.chroma_format_idc = static_cast<std::uint8_t>(r.read_bits(2));

    sps.log2_ctu_size_minus5 = static_cast<std::uint8_t>(r.read_bits(2));

    sps.ptl_dpb_hrd_params_present = r.read_bit();

    if (sps.ptl_dpb_hrd_params_present) {
        detail::skip_profile_tier_level(r, sps.max_sublayers_minus1);
    }

    sps.gdr_enabled_flag = r.read_bit();

    sps.ref_pic_resampling_enabled_flag = r.read_bit();

    if (sps.ref_pic_resampling_enabled_flag) {
        (void)r.read_bit(); /* sps_res_change_in_clvs_allowed_flag */
    }

    const std::uint32_t pic_width = r.read_ue();
    const std::uint32_t pic_height = r.read_ue();

    if (r.read_bit()) {
        /* sps_conformance_window_flag */
        (void)r.read_ue();
        (void)r.read_ue();
        (void)r.read_ue();
        (void)r.read_ue();
    }

    if (r.read_bit()) {
        /* -------------------------------------------------------
         * sps_subpic_info_present_flag
         * -------------------------------------------------------
         */
        sps.subpic_info_present_flag = true;

        const std::uint32_t num_subpics_minus1 = r.read_ue();

        bool independent_subpics = false;
        bool same_size = false;

        if (num_subpics_minus1 > 0) {
            independent_subpics = r.read_bit();
            same_size = r.read_bit();
        }

        const std::uint32_t ctb_size = sps.ctb_size();

        /*
         * Sub-picture coordinates are signalled with wlen / hlen bits where
         * wlen = Ceil(Log2(tmp_width_val)), hlen = Ceil(Log2(tmp_height_val))
         * and tmp_width_val / tmp_height_val are the number of CTU columns /
         * rows (H.266 §7.4.3.4).
         */
        const std::uint32_t tmp_width_val = (pic_width + ctb_size - 1u) / ctb_size;
        const std::uint32_t tmp_height_val = (pic_height + ctb_size - 1u) / ctb_size;

        auto ceil_log2 = [](std::uint32_t v) noexcept -> unsigned {
            unsigned bits = 0;
            std::uint32_t power = 1;
            while (power < v) {
                power <<= 1u;
                ++bits;
            }
            return bits;
        };

        const unsigned wlen = ceil_log2(tmp_width_val);
        const unsigned hlen = ceil_log2(tmp_height_val);

        for (std::uint32_t i = 0; num_subpics_minus1 > 0 && i <= num_subpics_minus1; ++i) {
            if (!same_size || i == 0) {
                if (i > 0 && pic_width > ctb_size) {
                    (void)r.read_bits(wlen); /* sps_subpic_ctu_top_left_x */
                }
                if (i > 0 && pic_height > ctb_size) {
                    (void)r.read_bits(hlen); /* sps_subpic_ctu_top_left_y */
                }
                if (i < num_subpics_minus1 && pic_width > ctb_size) {
                    (void)r.read_bits(wlen); /* sps_subpic_width_minus1 */
                }
                if (i < num_subpics_minus1 && pic_height > ctb_size) {
                    (void)r.read_bits(hlen); /* sps_subpic_height_minus1 */
                }
            }
            if (!independent_subpics) {
                (void)r.read_bit(); /* sps_subpic_treated_as_pic_flag */
                (void)r.read_bit(); /* sps_loop_filter_across_subpic_enabled_flag */
            }
        }

        sps.subpic_id_len_minus1 = static_cast<std::uint8_t>(r.read_ue());

        if (r.read_bit()) {
            /* sps_subpic_id_mapping_explicitly_signalled_flag */
            if (r.read_bit()) {
                for (std::uint32_t i = 0; i <= num_subpics_minus1; ++i) {
                    (void)r.read_bits(sps.subpic_id_len_minus1 + 1);
                }
            }
        }
    }

    (void)r.read_ue();  /* sps_bitdepth_minus8 */
    (void)r.read_bit(); /* sps_entropy_coding_sync_enabled_flag */
    (void)r.read_bit(); /* sps_entry_point_offsets_present_flag */

    /* -------------------------------------------------------
     * Picture order count configuration.
     * -------------------------------------------------------
     */
    sps.log2_max_pic_order_cnt_lsb_minus4 = static_cast<std::uint8_t>(r.read_bits(4));

    sps.poc_msb_cycle_flag = r.read_bit();

    if (sps.poc_msb_cycle_flag) {
        sps.poc_msb_cycle_len_minus1 = static_cast<std::uint8_t>(r.read_ue());
    }

    const std::uint32_t num_extra_ph_bytes = r.read_bits(2);

    std::uint32_t num_extra_ph_bits = 0;
    for (std::uint32_t i = 0; i < num_extra_ph_bytes * 8u; ++i) {
        if (r.read_bit()) {
            ++num_extra_ph_bits;
        }
    }
    sps.num_extra_ph_bits = static_cast<std::uint8_t>(num_extra_ph_bits);

    const std::uint32_t num_extra_sh_bytes = r.read_bits(2);

    std::uint32_t num_extra_sh_bits = 0;
    for (std::uint32_t i = 0; i < num_extra_sh_bytes * 8u; ++i) {
        if (r.read_bit()) {
            ++num_extra_sh_bits;
        }
    }
    sps.num_extra_sh_bits = static_cast<std::uint8_t>(num_extra_sh_bits);

    return sps;
}

}  // namespace vvc
}  // namespace bs
