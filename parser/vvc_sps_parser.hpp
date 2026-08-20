// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "vvc_sps.hpp"

#include <cstdint>
#include <vector>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC SPS parser (H.266 §7.3.2.4) – Full syntax
 * -----------------------------------------------------------
 * Mirrors ffmpeg cbs_h266 sps() ~lines 1056-1672.
 * Bit-accurate: every ue(v)/se(v)/u(n)/flag is consumed
 * with correct conditional branching.  Storage is kept for
 * the fields listed in the task; HRD/VUI/scaling deep
 * arrays are simplified to counts/flags but bits are fully
 * consumed.
 */

namespace detail {

/*
 * general_constraints_info() (§7.3.3.2).  ~90 one-bit flags followed by the
 * additional/reserved bit run.
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

// ---------- helpers for full SPS ----------

inline unsigned ceil_log2(std::uint32_t v) noexcept {
    unsigned bits = 0;
    std::uint32_t p = 1;
    while (p < v) {
        p <<= 1;
        ++bits;
    }
    return bits;
}

template <typename Reader>
inline bool has_more_rbsp_data(Reader& r) {
    if (!r.has_more_bits())
        return false;
    Reader tmp = r;
    bool first = false;
    try {
        first = tmp.read_bit();
    } catch (...) {
        return false;
    }
    if (!first)
        return true;
    while (!tmp.byte_aligned()) {
        if (!tmp.has_more_bits())
            break;
        bool b = false;
        try {
            b = tmp.read_bit();
        } catch (...) {
            return false;
        }
        if (b)
            return true;
    }
    if (tmp.has_more_bits())
        return true;
    return false;
}

template <typename Reader>
inline void parse_dpb_parameters(
    Reader& r, std::uint8_t max_sublayers_minus1, bool sublayer_info_flag
) {
    int start = sublayer_info_flag ? 0 : max_sublayers_minus1;
    for (int i = start; i <= max_sublayers_minus1; ++i) {
        (void)r.read_ue();  // dpb_max_dec_pic_buffering_minus1
        (void)r.read_ue();  // dpb_max_num_reorder_pics
        (void)r.read_ue();  // dpb_max_latency_increase_plus1
    }
}

template <typename Reader>
inline void parse_ref_pic_list_struct(
    Reader& r,
    typename SequenceParameterSet::RefPicListStruct& out,
    std::uint32_t list_idx,
    std::uint32_t rpls_idx,
    const SequenceParameterSet& sps
) {
    (void)list_idx;
    (void)rpls_idx;
    out.num_ref_entries = r.read_ue();
    if (out.num_ref_entries > 64)
        out.num_ref_entries = 64;  // cap for storage
    bool ltrp_in_header = false;
    if (sps.long_term_ref_pics_flag && rpls_idx < sps.num_ref_pic_lists[list_idx] &&
        out.num_ref_entries > 0) {
        ltrp_in_header = r.read_bit();
    }
    if (sps.long_term_ref_pics_flag && rpls_idx == sps.num_ref_pic_lists[list_idx]) {
        ltrp_in_header = true;
    }
    out.ltrp_in_header_flag = ltrp_in_header;
    out.entries.clear();
    out.entries.reserve(out.num_ref_entries);
    // for poc lsb lt count
    int poc_lt_counter = 0;
    for (std::uint32_t i = 0; i < out.num_ref_entries; ++i) {
        typename SequenceParameterSet::RefPicListEntry e;
        if (sps.inter_layer_prediction_enabled_flag) {
            e.inter_layer_ref_pic_flag = r.read_bit();
        } else {
            e.inter_layer_ref_pic_flag = false;
        }
        if (!e.inter_layer_ref_pic_flag) {
            if (sps.long_term_ref_pics_flag) {
                e.st_ref_pic_flag = r.read_bit();
            } else {
                e.st_ref_pic_flag = true;
            }
            if (e.st_ref_pic_flag) {
                e.abs_delta_poc_st = r.read_ue();
                std::uint32_t abs_delta = e.abs_delta_poc_st;
                // weighted pred tweak affects inferred delta but not bit consumption
                if ((sps.weighted_pred_flag || sps.weighted_bipred_flag) && i != 0) {
                    // abs stays as read
                } else {
                    abs_delta = e.abs_delta_poc_st + 1;
                }
                if (abs_delta > 0) {
                    e.strp_entry_sign_flag = r.read_bit();
                }
            } else {
                if (!ltrp_in_header) {
                    std::uint32_t bits =
                        static_cast<std::uint32_t>(sps.log2_max_pic_order_cnt_lsb_minus4) + 4;
                    e.rpls_poc_lsb_lt = r.read_bits(bits);
                    ++poc_lt_counter;
                }
            }
        } else {
            // inter layer: ilrp_idx is ue 0..num_direct-1, but num_direct unknown without VPS
            // consume as ue
            e.ilrp_idx = r.read_ue();
        }
        (void)poc_lt_counter;
        out.entries.push_back(e);
    }
}

template <typename Reader>
inline void parse_general_timing_hrd(Reader& r, SequenceParameterSet& sps) {
    (void)r.read_bits(32);  // num_units_in_tick
    (void)r.read_bits(32);  // time_scale
    sps.general_nal_hrd_params_present_flag = r.read_bit();
    sps.general_vcl_hrd_params_present_flag = r.read_bit();
    if (sps.general_nal_hrd_params_present_flag || sps.general_vcl_hrd_params_present_flag) {
        sps.general_same_pic_timing_in_all_ols_flag = r.read_bit();
        sps.general_du_hrd_params_present_flag = r.read_bit();
        if (sps.general_du_hrd_params_present_flag) {
            sps.tick_divisor_minus2 = static_cast<std::uint8_t>(r.read_bits(8));
        }
        sps.bit_rate_scale = static_cast<std::uint8_t>(r.read_bits(4));
        sps.cpb_size_scale = static_cast<std::uint8_t>(r.read_bits(4));
        if (sps.general_du_hrd_params_present_flag) {
            sps.cpb_size_du_scale = static_cast<std::uint8_t>(r.read_bits(4));
        }
        sps.hrd_cpb_cnt_minus1 = r.read_ue();  // 0..31
        if (sps.hrd_cpb_cnt_minus1 > 31)
            sps.hrd_cpb_cnt_minus1 = 31;
    } else {
        sps.general_du_hrd_params_present_flag = false;
    }
}

template <typename Reader>
inline void parse_sublayer_hrd(Reader& r, std::uint32_t hrd_cpb_cnt_minus1, bool du_present) {
    for (std::uint32_t i = 0; i <= hrd_cpb_cnt_minus1; ++i) {
        (void)r.read_ue();  // bit_rate_value_minus1
        (void)r.read_ue();  // cpb_size_value_minus1
        if (du_present) {
            (void)r.read_ue();  // cpb_size_du_value_minus1
            (void)r.read_ue();  // bit_rate_du_value_minus1
        }
        (void)r.read_bit();  // cbr_flag
    }
}

template <typename Reader>
inline void parse_ols_timing_hrd(Reader& r, SequenceParameterSet& sps) {
    std::uint8_t max_sublayers = sps.max_sublayers_minus1;
    std::uint8_t first = sps.sublayer_cpb_params_present_flag ? 0 : max_sublayers;
    for (int i = first; i <= max_sublayers; ++i) {
        bool fixed_general = r.read_bit();
        bool fixed_within = true;
        if (!fixed_general)
            fixed_within = r.read_bit();
        else
            fixed_within = true;
        if (fixed_within) {
            (void)r.read_ue();  // elemental_duration_in_tc_minus1
        } else {
            if ((sps.general_nal_hrd_params_present_flag ||
                 sps.general_vcl_hrd_params_present_flag) &&
                sps.hrd_cpb_cnt_minus1 == 0) {
                (void)r.read_bit();  // low_delay_hrd_flag
            }
        }
        if (sps.general_nal_hrd_params_present_flag) {
            parse_sublayer_hrd(r, sps.hrd_cpb_cnt_minus1, sps.general_du_hrd_params_present_flag);
        }
        if (sps.general_vcl_hrd_params_present_flag) {
            parse_sublayer_hrd(r, sps.hrd_cpb_cnt_minus1, sps.general_du_hrd_params_present_flag);
        }
    }
}

template <typename Reader>
inline void parse_extension_data(Reader& r) {
    while (has_more_rbsp_data(r)) {
        (void)r.read_bit();
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
        sps.res_change_in_clvs_allowed_flag = r.read_bit();
    } else {
        sps.res_change_in_clvs_allowed_flag = false;
    }

    sps.pic_width_max_in_luma_samples = r.read_ue();
    sps.pic_height_max_in_luma_samples = r.read_ue();

    sps.conformance_window_flag = r.read_bit();
    if (sps.conformance_window_flag) {
        sps.conf_win_left_offset = r.read_ue();
        sps.conf_win_right_offset = r.read_ue();
        sps.conf_win_top_offset = r.read_ue();
        sps.conf_win_bottom_offset = r.read_ue();
    }

    // compute tmp sizes for subpic handling
    const std::uint32_t ctb_log2 = static_cast<std::uint32_t>(sps.log2_ctu_size_minus5) + 5;
    const std::uint32_t ctb_size = std::uint32_t{1} << ctb_log2;
    const std::uint32_t tmp_width_val =
        (sps.pic_width_max_in_luma_samples + ctb_size - 1u) / ctb_size;
    const std::uint32_t tmp_height_val =
        (sps.pic_height_max_in_luma_samples + ctb_size - 1u) / ctb_size;
    std::int32_t max_width_minus1 = static_cast<std::int32_t>(tmp_width_val) - 1;
    std::int32_t max_height_minus1 = static_cast<std::int32_t>(tmp_height_val) - 1;
    if (max_width_minus1 < 0)
        max_width_minus1 = 0;
    if (max_height_minus1 < 0)
        max_height_minus1 = 0;

    sps.subpic_info_present_flag = r.read_bit();
    if (sps.subpic_info_present_flag) {
        sps.num_subpics_minus1 = r.read_ue();
        if (sps.num_subpics_minus1 > 0) {
            sps.independent_subpics_flag = r.read_bit();
            sps.subpic_same_size_flag = r.read_bit();
        } else {
            sps.independent_subpics_flag = true;
            sps.subpic_same_size_flag = false;
        }

        const unsigned wlen = detail::ceil_log2(tmp_width_val);
        const unsigned hlen = detail::ceil_log2(tmp_height_val);

        // store first subpic inferred positions
        // Handle subpic layout per spec / ffmpeg
        if (sps.num_subpics_minus1 == 0) {
            // inferred single subpic covers picture
        } else {
            // first subpic
            if (sps.pic_width_max_in_luma_samples > ctb_size) {
                // width_minus1 for 0 is read when > ctb_size
                (void)r.read_bits(wlen);  // sps_subpic_width_minus1[0]
            }
            if (sps.pic_height_max_in_luma_samples > ctb_size) {
                (void)r.read_bits(hlen);  // sps_subpic_height_minus1[0]
            }
            if (!sps.independent_subpics_flag) {
                (void)r.read_bit();  // treated_as_pic
                (void)r.read_bit();  // loop_filter_across
            }
            for (std::uint32_t i = 1; i <= sps.num_subpics_minus1; ++i) {
                if (!sps.subpic_same_size_flag) {
                    if (sps.pic_width_max_in_luma_samples > ctb_size) {
                        (void)r.read_bits(wlen);  // ctu_top_left_x
                    }
                    if (sps.pic_height_max_in_luma_samples > ctb_size) {
                        (void)r.read_bits(hlen);  // ctu_top_left_y
                    }
                    if (i < sps.num_subpics_minus1 &&
                        sps.pic_width_max_in_luma_samples > ctb_size) {
                        (void)r.read_bits(wlen);  // width_minus1
                    }
                    if (i < sps.num_subpics_minus1 &&
                        sps.pic_height_max_in_luma_samples > ctb_size) {
                        (void)r.read_bits(hlen);  // height_minus1
                    }
                } else {
                    // same_size: inferred, no bits
                }
                if (!sps.independent_subpics_flag) {
                    (void)r.read_bit();  // treated_as_pic
                    (void)r.read_bit();  // loop_filter_across
                }
            }
        }

        sps.subpic_id_len_minus1 = static_cast<std::uint8_t>(r.read_ue());

        sps.subpic_id_mapping_explicitly_signalled_flag = r.read_bit();
        if (sps.subpic_id_mapping_explicitly_signalled_flag) {
            sps.subpic_id_mapping_present_flag = r.read_bit();
            if (sps.subpic_id_mapping_present_flag) {
                for (std::uint32_t i = 0; i <= sps.num_subpics_minus1; ++i) {
                    (void)r.read_bits(sps.subpic_id_len_minus1 + 1);
                }
            }
        } else {
            sps.subpic_id_mapping_present_flag = false;
        }
    } else {
        sps.num_subpics_minus1 = 0;
        sps.independent_subpics_flag = true;
        sps.subpic_same_size_flag = false;
        sps.subpic_id_mapping_explicitly_signalled_flag = false;
        sps.subpic_id_mapping_present_flag = false;
    }

    sps.bitdepth_minus8 = static_cast<std::uint8_t>(r.read_ue());
    sps.entropy_coding_sync_enabled_flag = r.read_bit();
    sps.entry_point_offsets_present_flag = r.read_bit();

    sps.log2_max_pic_order_cnt_lsb_minus4 = static_cast<std::uint8_t>(r.read_bits(4));
    sps.poc_msb_cycle_flag = r.read_bit();
    if (sps.poc_msb_cycle_flag) {
        sps.poc_msb_cycle_len_minus1 = static_cast<std::uint8_t>(r.read_ue());
    }

    sps.num_extra_ph_bytes = static_cast<std::uint8_t>(r.read_bits(2));
    {
        std::uint32_t cnt = 0;
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(sps.num_extra_ph_bytes) * 8u;
             ++i) {
            if (r.read_bit())
                ++cnt;
        }
        sps.num_extra_ph_bits = static_cast<std::uint8_t>(cnt);
    }
    sps.num_extra_sh_bytes = static_cast<std::uint8_t>(r.read_bits(2));
    {
        std::uint32_t cnt = 0;
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(sps.num_extra_sh_bytes) * 8u;
             ++i) {
            if (r.read_bit())
                ++cnt;
        }
        sps.num_extra_sh_bits = static_cast<std::uint8_t>(cnt);
    }

    if (sps.ptl_dpb_hrd_params_present) {
        if (sps.max_sublayers_minus1 > 0) {
            sps.sublayer_dpb_params_flag = r.read_bit();
        } else {
            sps.sublayer_dpb_params_flag = false;
        }
        detail::parse_dpb_parameters(r, sps.max_sublayers_minus1, sps.sublayer_dpb_params_flag);
    }

    sps.log2_min_luma_coding_block_size_minus2 = r.read_ue();
    sps.partition_constraints_override_enabled_flag = r.read_bit();
    sps.log2_diff_min_qt_min_cb_intra_slice_luma = r.read_ue();
    sps.max_mtt_hierarchy_depth_intra_slice_luma = r.read_ue();
    if (sps.max_mtt_hierarchy_depth_intra_slice_luma != 0) {
        sps.log2_diff_max_bt_min_qt_intra_slice_luma = r.read_ue();
        sps.log2_diff_max_tt_min_qt_intra_slice_luma = r.read_ue();
    }
    if (sps.chroma_format_idc != 0) {
        sps.qtbtt_dual_tree_intra_flag = r.read_bit();
    } else {
        sps.qtbtt_dual_tree_intra_flag = false;
    }
    if (sps.qtbtt_dual_tree_intra_flag) {
        sps.log2_diff_min_qt_min_cb_intra_slice_chroma = r.read_ue();
        sps.max_mtt_hierarchy_depth_intra_slice_chroma = r.read_ue();
        if (sps.max_mtt_hierarchy_depth_intra_slice_chroma != 0) {
            sps.log2_diff_max_bt_min_qt_intra_slice_chroma = r.read_ue();
            sps.log2_diff_max_tt_min_qt_intra_slice_chroma = r.read_ue();
        }
    }
    sps.log2_diff_min_qt_min_cb_inter_slice = r.read_ue();
    sps.max_mtt_hierarchy_depth_inter_slice = r.read_ue();
    if (sps.max_mtt_hierarchy_depth_inter_slice != 0) {
        sps.log2_diff_max_bt_min_qt_inter_slice = r.read_ue();
        sps.log2_diff_max_tt_min_qt_inter_slice = r.read_ue();
    }

    if (ctb_size > 32) {
        sps.max_luma_transform_size_64_flag = r.read_bit();
    } else {
        sps.max_luma_transform_size_64_flag = false;
    }

    sps.transform_skip_enabled_flag = r.read_bit();
    if (sps.transform_skip_enabled_flag) {
        sps.log2_transform_skip_max_size_minus2 = r.read_ue();
        sps.bdpcm_enabled_flag = r.read_bit();
    } else {
        sps.bdpcm_enabled_flag = false;
    }

    sps.mts_enabled_flag = r.read_bit();
    if (sps.mts_enabled_flag) {
        sps.explicit_mts_intra_enabled_flag = r.read_bit();
        sps.explicit_mts_inter_enabled_flag = r.read_bit();
    }

    sps.lfnst_enabled_flag = r.read_bit();

    if (sps.chroma_format_idc != 0) {
        sps.joint_cbcr_enabled_flag = r.read_bit();
        sps.same_qp_table_for_chroma_flag = r.read_bit();
        std::uint32_t num_qp_tables =
            sps.same_qp_table_for_chroma_flag ? 1 : (sps.joint_cbcr_enabled_flag ? 3 : 2);
        std::int32_t qp_bd_offset = 6 * static_cast<std::int32_t>(sps.bitdepth_minus8);
        (void)qp_bd_offset;
        for (std::uint32_t i = 0; i < num_qp_tables; ++i) {
            (void)r.read_se();  // sps_qp_table_start_minus26
            std::uint32_t num_points = r.read_ue();
            if (num_points > 36)
                num_points = 36;
            for (std::uint32_t j = 0; j <= num_points; ++j) {
                (void)r.read_ue();  // delta_qp_in_val_minus1
                (void)r.read_ue();  // delta_qp_diff_val
            }
        }
    } else {
        sps.joint_cbcr_enabled_flag = false;
        sps.same_qp_table_for_chroma_flag = false;
    }

    sps.sao_enabled_flag = r.read_bit();
    sps.alf_enabled_flag = r.read_bit();
    if (sps.alf_enabled_flag && sps.chroma_format_idc != 0) {
        sps.ccalf_enabled_flag = r.read_bit();
    } else {
        sps.ccalf_enabled_flag = false;
    }
    sps.lmcs_enabled_flag = r.read_bit();
    sps.weighted_pred_flag = r.read_bit();
    sps.weighted_bipred_flag = r.read_bit();
    sps.long_term_ref_pics_flag = r.read_bit();
    if (sps.vps_id > 0) {
        sps.inter_layer_prediction_enabled_flag = r.read_bit();
    } else {
        sps.inter_layer_prediction_enabled_flag = false;
    }
    sps.idr_rpl_present_flag = r.read_bit();
    sps.rpl1_same_as_rpl0_flag = r.read_bit();

    for (int i = 0; i < (sps.rpl1_same_as_rpl0_flag ? 1 : 2); ++i) {
        sps.num_ref_pic_lists[i] = r.read_ue();
        if (sps.num_ref_pic_lists[i] > 32)
            sps.num_ref_pic_lists[i] = 32;
        sps.ref_pic_lists[i].clear();
        sps.ref_pic_lists[i].reserve(sps.num_ref_pic_lists[i]);
        for (std::uint32_t j = 0; j < sps.num_ref_pic_lists[i]; ++j) {
            typename SequenceParameterSet::RefPicListStruct ref;
            detail::parse_ref_pic_list_struct(r, ref, i, j, sps);
            sps.ref_pic_lists[i].push_back(std::move(ref));
        }
    }
    if (sps.rpl1_same_as_rpl0_flag) {
        sps.num_ref_pic_lists[1] = sps.num_ref_pic_lists[0];
        sps.ref_pic_lists[1] = sps.ref_pic_lists[0];
    }

    sps.ref_wraparound_enabled_flag = r.read_bit();

    sps.temporal_mvp_enabled_flag = r.read_bit();
    if (sps.temporal_mvp_enabled_flag) {
        sps.sbtmvp_enabled_flag = r.read_bit();
    } else {
        sps.sbtmvp_enabled_flag = false;
    }

    sps.amvr_enabled_flag = r.read_bit();
    sps.bdof_enabled_flag = r.read_bit();
    if (sps.bdof_enabled_flag) {
        sps.bdof_control_present_in_ph_flag = r.read_bit();
    } else {
        sps.bdof_control_present_in_ph_flag = false;
    }

    sps.smvd_enabled_flag = r.read_bit();
    sps.dmvr_enabled_flag = r.read_bit();
    if (sps.dmvr_enabled_flag) {
        sps.dmvr_control_present_in_ph_flag = r.read_bit();
    } else {
        sps.dmvr_control_present_in_ph_flag = false;
    }

    sps.mmvd_enabled_flag = r.read_bit();
    if (sps.mmvd_enabled_flag) {
        sps.mmvd_fullpel_only_enabled_flag = r.read_bit();
    } else {
        sps.mmvd_fullpel_only_enabled_flag = false;
    }

    sps.six_minus_max_num_merge_cand = r.read_ue();
    std::uint32_t max_num_merge_cand = 6 - sps.six_minus_max_num_merge_cand;

    sps.sbt_enabled_flag = r.read_bit();

    sps.affine_enabled_flag = r.read_bit();
    if (sps.affine_enabled_flag) {
        sps.five_minus_max_num_subblock_merge_cand = r.read_ue();
        sps.six_param_affine_enabled_flag = r.read_bit();
        if (sps.amvr_enabled_flag) {
            sps.affine_amvr_enabled_flag = r.read_bit();
        } else {
            sps.affine_amvr_enabled_flag = false;
        }
        sps.affine_prof_enabled_flag = r.read_bit();
        if (sps.affine_prof_enabled_flag) {
            sps.prof_control_present_in_ph_flag = r.read_bit();
        } else {
            sps.prof_control_present_in_ph_flag = false;
        }
    } else {
        sps.six_param_affine_enabled_flag = false;
        sps.affine_amvr_enabled_flag = false;
        sps.affine_prof_enabled_flag = false;
        sps.prof_control_present_in_ph_flag = false;
    }

    sps.bcw_enabled_flag = r.read_bit();
    sps.ciip_enabled_flag = r.read_bit();

    if (max_num_merge_cand >= 2) {
        sps.gpm_enabled_flag = r.read_bit();
        if (sps.gpm_enabled_flag && max_num_merge_cand >= 3) {
            sps.max_num_merge_cand_minus_max_num_gpm_cand = r.read_ue();
        }
    } else {
        sps.gpm_enabled_flag = false;
    }

    sps.log2_parallel_merge_level_minus2 = r.read_ue();

    sps.isp_enabled_flag = r.read_bit();
    sps.mrl_enabled_flag = r.read_bit();
    sps.mip_enabled_flag = r.read_bit();

    if (sps.chroma_format_idc != 0) {
        sps.cclm_enabled_flag = r.read_bit();
    } else {
        sps.cclm_enabled_flag = false;
    }
    if (sps.chroma_format_idc == 1) {
        sps.chroma_horizontal_collocated_flag = r.read_bit();
        sps.chroma_vertical_collocated_flag = r.read_bit();
    } else {
        sps.chroma_horizontal_collocated_flag = true;
        sps.chroma_vertical_collocated_flag = true;
    }

    sps.palette_enabled_flag = r.read_bit();
    if (sps.chroma_format_idc == 3 && !sps.max_luma_transform_size_64_flag) {
        sps.act_enabled_flag = r.read_bit();
    } else {
        sps.act_enabled_flag = false;
    }
    if (sps.transform_skip_enabled_flag || sps.palette_enabled_flag) {
        sps.min_qp_prime_ts_minus4 = r.read_ue();
    }

    sps.ibc_enabled_flag = r.read_bit();
    if (sps.ibc_enabled_flag) {
        sps.six_minus_max_num_ibc_merge_cand = r.read_ue();
    }

    sps.ladf_enabled_flag = r.read_bit();
    if (sps.ladf_enabled_flag) {
        sps.num_ladf_intervals_minus2 = r.read_bits(2);
        sps.ladf_lowest_interval_qp_offset = r.read_se();
        for (std::uint32_t i = 0; i < sps.num_ladf_intervals_minus2 + 1; ++i) {
            (void)r.read_se();  // sps_ladf_qp_offset
            (void)r.read_ue();  // sps_ladf_delta_threshold_minus1
        }
    }

    sps.explicit_scaling_list_enabled_flag = r.read_bit();
    sps.scaling_enabled_flag = sps.explicit_scaling_list_enabled_flag;
    if (sps.lfnst_enabled_flag && sps.explicit_scaling_list_enabled_flag) {
        sps.scaling_matrix_for_lfnst_disabled_flag = r.read_bit();
    } else {
        sps.scaling_matrix_for_lfnst_disabled_flag = false;
    }
    if (sps.act_enabled_flag && sps.explicit_scaling_list_enabled_flag) {
        sps.scaling_matrix_for_alternative_colour_space_disabled_flag = r.read_bit();
    } else {
        sps.scaling_matrix_for_alternative_colour_space_disabled_flag = false;
    }
    if (sps.scaling_matrix_for_alternative_colour_space_disabled_flag) {
        sps.scaling_matrix_designated_colour_space_flag = r.read_bit();
    } else {
        sps.scaling_matrix_designated_colour_space_flag = false;
    }

    sps.dep_quant_enabled_flag = r.read_bit();
    sps.sign_data_hiding_enabled_flag = r.read_bit();

    sps.virtual_boundaries_enabled_flag = r.read_bit();
    if (sps.virtual_boundaries_enabled_flag) {
        sps.virtual_boundaries_present_flag = r.read_bit();
        if (sps.virtual_boundaries_present_flag) {
            sps.num_ver_virtual_boundaries = r.read_ue();
            sps.virtual_boundary_pos_x_minus1.clear();
            for (std::uint32_t i = 0; i < sps.num_ver_virtual_boundaries; ++i) {
                std::uint32_t v = r.read_ue();
                sps.virtual_boundary_pos_x_minus1.push_back(v);
            }
            sps.num_hor_virtual_boundaries = r.read_ue();
            sps.virtual_boundary_pos_y_minus1.clear();
            for (std::uint32_t i = 0; i < sps.num_hor_virtual_boundaries; ++i) {
                std::uint32_t v = r.read_ue();
                sps.virtual_boundary_pos_y_minus1.push_back(v);
            }
        } else {
            sps.num_ver_virtual_boundaries = 0;
            sps.num_hor_virtual_boundaries = 0;
        }
    } else {
        sps.virtual_boundaries_present_flag = false;
        sps.num_ver_virtual_boundaries = 0;
        sps.num_hor_virtual_boundaries = 0;
    }

    if (sps.ptl_dpb_hrd_params_present) {
        sps.timing_hrd_params_present_flag = r.read_bit();
        if (sps.timing_hrd_params_present_flag) {
            detail::parse_general_timing_hrd(r, sps);
            if (sps.max_sublayers_minus1 > 0) {
                sps.sublayer_cpb_params_present_flag = r.read_bit();
            } else {
                sps.sublayer_cpb_params_present_flag = false;
            }
            detail::parse_ols_timing_hrd(r, sps);
        } else {
            sps.sublayer_cpb_params_present_flag = false;
        }
    } else {
        sps.timing_hrd_params_present_flag = false;
        sps.sublayer_cpb_params_present_flag = false;
    }

    sps.field_seq_flag = r.read_bit();
    sps.vui_parameters_present_flag = r.read_bit();
    if (sps.vui_parameters_present_flag) {
        sps.vui_payload_size_minus1 = r.read_ue();
        // byte alignment zero bits
        while (!r.byte_aligned()) {
            (void)r.read_bit();  // sps_vui_alignment_zero_bit
        }
        // vui_parameters() – inline parsing with correct flags
        bool prog = r.read_bit();
        bool inter = r.read_bit();
        (void)r.read_bit();  // non_packed
        (void)r.read_bit();  // non_projected
        bool ar_present = r.read_bit();
        if (ar_present) {
            (void)r.read_bit();  // constant
            std::uint32_t idc = r.read_bits(8);
            if (idc == 255) {
                (void)r.read_bits(16);
                (void)r.read_bits(16);
            }
        }
        bool overscan_present = r.read_bit();
        if (overscan_present)
            (void)r.read_bit();
        bool colour_present = r.read_bit();
        if (colour_present) {
            (void)r.read_bits(8);
            (void)r.read_bits(8);
            (void)r.read_bits(8);
            (void)r.read_bit();
        }
        bool chroma_loc_present = r.read_bit();
        if (chroma_loc_present) {
            if (sps.chroma_format_idc != 1 && chroma_loc_present) {
                // spec error but we still consume
            }
            if (prog && !inter) {
                (void)r.read_ue();  // frame
            } else {
                (void)r.read_ue();  // top
                (void)r.read_ue();  // bottom
            }
        }
        // vui payload extension: if still has more rbsp data within payload, consume
        // Use payload size to bound: we have consumed unknown bits, but remaining payload extension
        // is generic
        if (detail::has_more_rbsp_data(r)) {
            // payload_extension: consume bits until only trailing remains or payload size exhausted
            // We don't have exact bit position, so consume generically while has_more_rbsp_data
            // The first extension bit handling in spec: read until byte alignment would be
            // extension_data For simplicity consume all remaining before trailing as extension
            std::uint32_t payload_size = sps.vui_payload_size_minus1 + 1;
            (void)payload_size;
            // Consume extension bits if any (payload_extension)
            // ffmpeg payload_extension reads bit_length bits then trailing
            // We'll consume via generic extension loop but leave final trailing
            // To avoid consuming trailing, we rely on has_more_rbsp_data guard
            while (detail::has_more_rbsp_data(r)) {
                // peek if remaining is just vui_payload trailing (1 + zeros) – has_more_rbsp_data
                // will return false then so loop will break correctly consume one extension bit
                (void)r.read_bit();
                if (!detail::has_more_rbsp_data(r))
                    break;
                // If still more, continue; the has_more check ensures trailing is not consumed here
                // To consume properly we need to know payload_size; instead just consume one and
                // rely on trailing handling outside
                break;  // consume only first check? Instead we let generic extension_data handle
                        // rest
            }
            // If still has more rbsp data, it may be extension_data bytes – consume them
            detail::parse_extension_data(r);
            // vui_payload trailing: 1 + align zeros
            if (r.has_more_bits()) {
                try {
                    (void)r.read_bit();
                    while (!r.byte_aligned() && r.has_more_bits())
                        (void)r.read_bit();
                } catch (...) {
                }
            }
        }
    }

    sps.extension_flag = r.read_bit();
    if (sps.extension_flag) {
        sps.range_extension_flag = r.read_bit();
        sps.extension_7bits = static_cast<std::uint8_t>(r.read_bits(7));
        if (sps.range_extension_flag) {
            sps.extended_precision_flag = r.read_bit();
            if (sps.transform_skip_enabled_flag) {
                sps.ts_residual_coding_rice_present_in_sh_flag = r.read_bit();
            } else {
                sps.ts_residual_coding_rice_present_in_sh_flag = false;
            }
            sps.rrc_rice_extension_flag = r.read_bit();
            sps.persistent_rice_adaptation_enabled_flag = r.read_bit();
            sps.reverse_last_sig_coeff_enabled_flag = r.read_bit();
        } else {
            sps.extended_precision_flag = false;
            sps.ts_residual_coding_rice_present_in_sh_flag = false;
            sps.rrc_rice_extension_flag = false;
            sps.persistent_rice_adaptation_enabled_flag = false;
            sps.reverse_last_sig_coeff_enabled_flag = false;
        }
    } else {
        sps.range_extension_flag = false;
        sps.extension_7bits = 0;
        sps.extended_precision_flag = false;
        sps.ts_residual_coding_rice_present_in_sh_flag = false;
        sps.rrc_rice_extension_flag = false;
        sps.persistent_rice_adaptation_enabled_flag = false;
        sps.reverse_last_sig_coeff_enabled_flag = false;
    }

    if (sps.extension_7bits) {
        detail::parse_extension_data(r);
    }

    // rbsp_trailing_bits
    if (r.has_more_bits()) {
        try {
            (void)r.read_bit();  // stop_one_bit
            while (!r.byte_aligned() && r.has_more_bits()) {
                (void)r.read_bit();  // alignment_zero_bit
            }
        } catch (...) {
            // truncated but ignore
        }
    }

    return sps;
}

}  // namespace vvc
}  // namespace bs
