#pragma once

#include "vvc_ph.hpp"
#include "vvc_pps.hpp"
#include "vvc_sps.hpp"

#include <cstdint>
#include <limits>

namespace bs {
namespace vvc {

namespace detail_ph {

inline unsigned ceil_log2_ph(std::uint32_t v) noexcept {
    unsigned bits = 0;
    std::uint32_t p = 1;
    while (p < v) {
        p <<= 1;
        ++bits;
    }
    return bits;
}

template <typename Reader>
inline bool has_more_rbsp_data_generic_ph(Reader& r) {
    if constexpr (requires { r.more_rbsp_data(); }) {
        return r.more_rbsp_data();
    } else {
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
}

template <typename Reader>
inline void parse_ref_pic_list_struct_ph(
    Reader& r,
    PictureHeaderRefPicLists::List& out,
    std::uint32_t list_idx,
    std::uint32_t rpls_idx,
    const SequenceParameterSet& sps
) {
    (void)list_idx;
    (void)rpls_idx;
    out.num_ref_entries = r.read_ue();
    if (out.num_ref_entries > 64)
        out.num_ref_entries = 64;
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
    out.poc_lsb_lt.clear();
    out.delta_poc_msb_cycle_present_flag.clear();
    out.delta_poc_msb_cycle_lt.clear();
    int poc_lt_counter = 0;
    for (std::uint32_t i = 0; i < out.num_ref_entries; ++i) {
        PictureHeaderRefPicLists::Entry e{};
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
                if ((sps.weighted_pred_flag || sps.weighted_bipred_flag) && i != 0) {
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
            e.ilrp_idx = r.read_ue();
        }
        (void)poc_lt_counter;
        out.entries.push_back(e);
    }
}

template <typename Reader>
inline void parse_ref_pic_lists_ph(
    Reader& r,
    const SequenceParameterSet& sps,
    const PictureParameterSet& pps,
    PictureHeaderRefPicLists& out
) {
    for (int i = 0; i < 2; ++i) {
        auto& lst = out.rpl[i];
        bool rpl_sps_flag = false;
        if (sps.num_ref_pic_lists[i] > 0 && (i == 0 || (i == 1 && pps.rpl1_idx_present_flag))) {
            rpl_sps_flag = r.read_bit();
        } else {
            if (sps.num_ref_pic_lists[i] == 0) {
                rpl_sps_flag = false;
            } else {
                if (!pps.rpl1_idx_present_flag && i == 1) {
                    // infer from 0
                    rpl_sps_flag = out.rpl[0].rpl_sps_flag;
                }
            }
        }
        lst.rpl_sps_flag = rpl_sps_flag;
        if (rpl_sps_flag) {
            if (sps.num_ref_pic_lists[i] > 1 && (i == 0 || (i == 1 && pps.rpl1_idx_present_flag))) {
                unsigned bits = ceil_log2_ph(sps.num_ref_pic_lists[i]);
                lst.rpl_idx = r.read_bits(bits);
                if (lst.rpl_idx >= sps.num_ref_pic_lists[i])
                    lst.rpl_idx = 0;
            } else if (sps.num_ref_pic_lists[i] == 1) {
                lst.rpl_idx = 0;
            } else if (i == 1 && !pps.rpl1_idx_present_flag) {
                lst.rpl_idx = out.rpl[0].rpl_idx;
            }
            // copy from sps list for derived num_ref_entries etc.
            if (lst.rpl_idx < sps.ref_pic_lists[i].size()) {
                const auto& src = sps.ref_pic_lists[i][lst.rpl_idx];
                lst.num_ref_entries = src.num_ref_entries;
                lst.ltrp_in_header_flag = src.ltrp_in_header_flag;
                lst.entries.clear();
                lst.entries.reserve(src.entries.size());
                for (auto& se : src.entries) {
                    PictureHeaderRefPicLists::Entry e{};
                    e.inter_layer_ref_pic_flag = se.inter_layer_ref_pic_flag;
                    e.st_ref_pic_flag = se.st_ref_pic_flag;
                    e.abs_delta_poc_st = se.abs_delta_poc_st;
                    e.strp_entry_sign_flag = se.strp_entry_sign_flag;
                    e.rpls_poc_lsb_lt = se.rpls_poc_lsb_lt;
                    e.ilrp_idx = se.ilrp_idx;
                    lst.entries.push_back(e);
                }
            } else {
                lst.num_ref_entries = 0;
            }
        } else {
            parse_ref_pic_list_struct_ph(r, lst, i, sps.num_ref_pic_lists[i], sps);
        }
        // handle ltrp extra poc_lsb_lt and delta
        // count num_ltrp_entries
        int num_ltrp_entries = 0;
        for (auto& e : lst.entries) {
            if (!e.inter_layer_ref_pic_flag && !e.st_ref_pic_flag)
                num_ltrp_entries++;
        }
        // But also need to consider ltrp_in_header_flag for extra?
        // Actually poc_lsb_lt for each ltrp when ltrp_in_header_flag
        lst.poc_lsb_lt.clear();
        lst.delta_poc_msb_cycle_present_flag.clear();
        lst.delta_poc_msb_cycle_lt.clear();
        if (num_ltrp_entries > 0) {
            for (int j = 0; j < num_ltrp_entries; ++j) {
                if (lst.ltrp_in_header_flag) {
                    std::uint32_t bits =
                        static_cast<std::uint32_t>(sps.log2_max_pic_order_cnt_lsb_minus4) + 4;
                    std::uint32_t poc = r.read_bits(bits);
                    lst.poc_lsb_lt.push_back(poc);
                }
                bool present = r.read_bit();
                lst.delta_poc_msb_cycle_present_flag.push_back(present);
                if (present) {
                    std::uint32_t max = 1u << (32 - sps.log2_max_pic_order_cnt_lsb_minus4 - 4);
                    std::uint32_t v = r.read_ue();
                    if (v >= max)
                        v = max - 1;
                    lst.delta_poc_msb_cycle_lt.push_back(v);
                } else {
                    lst.delta_poc_msb_cycle_lt.push_back(0);
                }
            }
        }
    }
}

template <typename Reader>
inline void parse_pred_weight_table_ph(
    Reader& r,
    const SequenceParameterSet& sps,
    const PictureParameterSet& pps,
    PictureHeaderRefPicLists& ref_lists,
    std::uint8_t num_ref_idx_active[2]
) {
    (void)num_ref_idx_active;
    std::uint32_t luma_log2_weight_denom = r.read_ue();
    if (luma_log2_weight_denom > 7)
        luma_log2_weight_denom = 7;
    if (sps.chroma_format_idc != 0) {
        std::int32_t delta = r.read_se();
        (void)delta;
    }
    std::uint32_t num_l0_weights = 0;
    if (pps.wp_info_in_ph_flag) {
        std::uint32_t max0 = ref_lists.rpl[0].num_ref_entries;
        if (max0 > 15)
            max0 = 15;
        num_l0_weights = r.read_ue();
        if (num_l0_weights > max0)
            num_l0_weights = max0;
    } else {
        num_l0_weights = num_ref_idx_active[0];
    }
    std::vector<bool> luma_weight_l0_flag(num_l0_weights, false);
    for (std::uint32_t i = 0; i < num_l0_weights; ++i)
        luma_weight_l0_flag[i] = r.read_bit();
    std::vector<bool> chroma_weight_l0_flag(num_l0_weights, false);
    if (sps.chroma_format_idc != 0) {
        for (std::uint32_t i = 0; i < num_l0_weights; ++i)
            chroma_weight_l0_flag[i] = r.read_bit();
    }
    for (std::uint32_t i = 0; i < num_l0_weights; ++i) {
        if (luma_weight_l0_flag[i]) {
            (void)r.read_se();
            (void)r.read_se();
        }
        if (chroma_weight_l0_flag[i]) {
            for (int j = 0; j < 2; ++j) {
                (void)r.read_se();
                (void)r.read_se();
            }
        }
    }
    std::uint32_t num_l1_weights = 0;
    if (pps.weighted_bipred_flag && ref_lists.rpl[1].num_ref_entries > 0) {
        if (pps.wp_info_in_ph_flag) {
            std::uint32_t max1 = ref_lists.rpl[1].num_ref_entries;
            if (max1 > 15)
                max1 = 15;
            num_l1_weights = r.read_ue();
            if (num_l1_weights > max1)
                num_l1_weights = max1;
        } else {
            num_l1_weights = num_ref_idx_active[1];
        }
    } else {
        num_l1_weights = 0;
    }
    std::vector<bool> luma_weight_l1_flag(num_l1_weights, false);
    for (std::uint32_t i = 0; i < num_l1_weights; ++i)
        luma_weight_l1_flag[i] = r.read_bit();
    if (sps.chroma_format_idc != 0) {
        std::vector<bool> chroma_weight_l1_flag(num_l1_weights, false);
        for (std::uint32_t i = 0; i < num_l1_weights; ++i)
            chroma_weight_l1_flag[i] = r.read_bit();
        for (std::uint32_t i = 0; i < num_l1_weights; ++i) {
            if (luma_weight_l1_flag[i]) {
                (void)r.read_se();
                (void)r.read_se();
            }
            if (chroma_weight_l1_flag[i]) {
                for (int j = 0; j < 2; ++j) {
                    (void)r.read_se();
                    (void)r.read_se();
                }
            }
        }
    } else {
        for (std::uint32_t i = 0; i < num_l1_weights; ++i) {
            if (luma_weight_l1_flag[i]) {
                (void)r.read_se();
                (void)r.read_se();
            }
        }
    }
}

}  // namespace detail_ph

/*
 * -----------------------------------------------------------
 * VVC Picture Header parser (H.266 §7.3.2.7)
 * -----------------------------------------------------------
 */
template <typename Reader>
[[nodiscard]]
inline PictureHeader parse_ph(
    Reader& r, const SequenceParameterSet* sps, const PictureParameterSet* pps
) {
    PictureHeader ph;

    ph.gdr_or_irap_pic_flag = r.read_bit();
    ph.non_ref_pic_flag = r.read_bit();
    if (ph.gdr_or_irap_pic_flag) {
        ph.gdr_pic_flag = r.read_bit();
    } else {
        ph.gdr_pic_flag = false;
    }
    ph.inter_slice_allowed_flag = r.read_bit();
    if (ph.inter_slice_allowed_flag) {
        ph.intra_slice_allowed_flag = r.read_bit();
    } else {
        ph.intra_slice_allowed_flag = true;
    }
    ph.pps_id = static_cast<std::uint8_t>(r.read_ue());

    if (sps == nullptr) {
        return ph;
    }

    // Re-resolve pps if not supplied but we have sps? Need to infer poc bits anyway.
    // If pps is null we still can parse poc fields (they only need sps).
    const std::uint32_t poc_lsb_bits =
        static_cast<std::uint32_t>(sps->log2_max_pic_order_cnt_lsb_minus4) + 4;

    ph.poc_lsb = r.read_bits(poc_lsb_bits);
    ph.poc_lsb_bits = poc_lsb_bits;

    if (ph.gdr_pic_flag) {
        ph.recovery_poc_cnt = r.read_ue();
    }

    // ph_extra_bit conditional per sps_extra_ph_bit_present_flag
    ph.extra_bits.clear();
    {
        std::uint32_t total_bits = static_cast<std::uint32_t>(sps->num_extra_ph_bytes) * 8u;
        // num_extra_ph_bits is derived count but cbs checks sps_extra_ph_bit_present_flag per bit
        // We treat all bits as present if num_extra_ph_bytes>0; otherwise none.
        // To match ffmpeg: for i in 0..num_extra_ph_bytes*8-1 if sps_extra_ph_bit_present_flag[i]
        // read 1 bit. Our SPS stores num_extra_ph_bits as count of flags set, not per-bit map.
        // Simplified: read num_extra_ph_bits bits (as stored) instead of total_bytes*8 with flag.
        // This keeps bitstream aligned for streams where flags are all 1.
        // For streams where flags sparse, we would over-read. But such streams are rare.
        // Better to read sps->num_extra_ph_bits bits.
        for (std::uint32_t i = 0; i < sps->num_extra_ph_bits; ++i) {
            bool b = r.read_bit();
            ph.extra_bits.push_back(b);
        }
        // If num_extra_ph_bytes indicates more but flags zero, we skip (no bits).
        (void)total_bits;
    }

    if (sps->poc_msb_cycle_flag) {
        ph.poc_msb_cycle_present_flag = r.read_bit();
        if (ph.poc_msb_cycle_present_flag) {
            ph.poc_msb_cycle_val =
                r.read_bits(static_cast<std::uint32_t>(sps->poc_msb_cycle_len_minus1) + 1);
        }
    }

    // If pps is null, we cannot parse pps-dependent syntax accurately.
    // Infer defaults and return early (still have POC). This keeps backward compat for callers with
    // only sps.
    if (pps == nullptr) {
        // Still need to consume remaining? Without pps we cannot know which fields are present,
        // but we can approximate by inferring no alf/lmcs etc and not consuming their bits.
        // Trailing bits will be left; caller may ignore.
        return ph;
    }

    // ALF
    if (sps->alf_enabled_flag && pps->alf_info_in_ph_flag) {
        ph.alf_enabled_flag = r.read_bit();
        if (ph.alf_enabled_flag) {
            ph.num_alf_aps_ids_luma = static_cast<std::uint8_t>(r.read_bits(3));
            ph.alf_aps_id_luma.clear();
            ph.alf_aps_id_luma.reserve(ph.num_alf_aps_ids_luma);
            for (int i = 0; i < ph.num_alf_aps_ids_luma; ++i) {
                ph.alf_aps_id_luma.push_back(static_cast<std::uint8_t>(r.read_bits(3)));
            }
            if (sps->chroma_format_idc != 0) {
                ph.alf_cb_enabled_flag = r.read_bit();
                ph.alf_cr_enabled_flag = r.read_bit();
            } else {
                ph.alf_cb_enabled_flag = false;
                ph.alf_cr_enabled_flag = false;
            }
            if (ph.alf_cb_enabled_flag || ph.alf_cr_enabled_flag) {
                ph.alf_aps_id_chroma = static_cast<std::uint8_t>(r.read_bits(3));
            }
            if (sps->ccalf_enabled_flag) {
                ph.alf_cc_cb_enabled_flag = r.read_bit();
                if (ph.alf_cc_cb_enabled_flag)
                    ph.alf_cc_cb_aps_id = static_cast<std::uint8_t>(r.read_bits(3));
                ph.alf_cc_cr_enabled_flag = r.read_bit();
                if (ph.alf_cc_cr_enabled_flag)
                    ph.alf_cc_cr_aps_id = static_cast<std::uint8_t>(r.read_bits(3));
            }
        }
    } else {
        ph.alf_enabled_flag = false;
    }

    if (sps->lmcs_enabled_flag) {
        ph.lmcs_enabled_flag = r.read_bit();
        if (ph.lmcs_enabled_flag) {
            ph.lmcs_aps_id = static_cast<std::uint8_t>(r.read_bits(2));
            if (sps->chroma_format_idc != 0)
                ph.chroma_residual_scale_flag = r.read_bit();
            else
                ph.chroma_residual_scale_flag = false;
        }
    } else {
        ph.lmcs_enabled_flag = false;
        ph.chroma_residual_scale_flag = false;
    }

    if (sps->explicit_scaling_list_enabled_flag) {
        ph.explicit_scaling_list_enabled_flag = r.read_bit();
        if (ph.explicit_scaling_list_enabled_flag) {
            ph.scaling_list_aps_id = static_cast<std::uint8_t>(r.read_bits(3));
        }
    } else {
        ph.explicit_scaling_list_enabled_flag = false;
    }

    if (sps->virtual_boundaries_enabled_flag && !sps->virtual_boundaries_present_flag) {
        ph.virtual_boundaries_present_flag = r.read_bit();
        if (ph.virtual_boundaries_present_flag) {
            ph.num_ver_virtual_boundaries = r.read_ue();
            ph.virtual_boundary_pos_x_minus1.clear();
            ph.virtual_boundary_pos_x_minus1.reserve(ph.num_ver_virtual_boundaries);
            std::uint32_t max_x = (pps->pic_width_in_luma_samples + 7) / 8;
            if (max_x >= 2)
                max_x -= 2;
            else
                max_x = 0;
            for (std::uint32_t i = 0; i < ph.num_ver_virtual_boundaries; ++i) {
                std::uint32_t v = r.read_ue();
                if (v > max_x)
                    v = max_x;
                ph.virtual_boundary_pos_x_minus1.push_back(v);
            }
            ph.num_hor_virtual_boundaries = r.read_ue();
            ph.virtual_boundary_pos_y_minus1.clear();
            ph.virtual_boundary_pos_y_minus1.reserve(ph.num_hor_virtual_boundaries);
            std::uint32_t max_y = (pps->pic_height_in_luma_samples + 7) / 8;
            if (max_y >= 2)
                max_y -= 2;
            else
                max_y = 0;
            for (std::uint32_t i = 0; i < ph.num_hor_virtual_boundaries; ++i) {
                std::uint32_t v = r.read_ue();
                if (v > max_y)
                    v = max_y;
                ph.virtual_boundary_pos_y_minus1.push_back(v);
            }
        } else {
            ph.num_ver_virtual_boundaries = 0;
            ph.num_hor_virtual_boundaries = 0;
        }
    } else {
        ph.virtual_boundaries_present_flag = false;
    }

    if (pps->output_flag_present_flag && !ph.non_ref_pic_flag) {
        ph.pic_output_flag = r.read_bit();
    } else {
        ph.pic_output_flag = true;
    }

    if (pps->rpl_info_in_ph_flag) {
        detail_ph::parse_ref_pic_lists_ph(r, *sps, *pps, ph.ref_pic_lists);
    } else {
        // leave ref_pic_lists empty (inferred)
    }

    if (sps->partition_constraints_override_enabled_flag) {
        ph.partition_constraints_override_flag = r.read_bit();
    } else {
        ph.partition_constraints_override_flag = false;
    }

    std::uint32_t ctb_log2_size_y = static_cast<std::uint32_t>(sps->log2_ctu_size_minus5) + 5;
    std::uint32_t min_cb_log2_size_y = sps->log2_min_luma_coding_block_size_minus2 + 2;

    if (ph.intra_slice_allowed_flag) {
        if (ph.partition_constraints_override_flag) {
            ph.log2_diff_min_qt_min_cb_intra_slice_luma = r.read_ue();
            ph.max_mtt_hierarchy_depth_intra_slice_luma = r.read_ue();
            if (ph.max_mtt_hierarchy_depth_intra_slice_luma != 0) {
                ph.log2_diff_max_bt_min_qt_intra_slice_luma = r.read_ue();
                ph.log2_diff_max_tt_min_qt_intra_slice_luma = r.read_ue();
            }
            if (sps->qtbtt_dual_tree_intra_flag) {
                ph.log2_diff_min_qt_min_cb_intra_slice_chroma = r.read_ue();
                ph.max_mtt_hierarchy_depth_intra_slice_chroma = r.read_ue();
                if (ph.max_mtt_hierarchy_depth_intra_slice_chroma != 0) {
                    ph.log2_diff_max_bt_min_qt_intra_slice_chroma = r.read_ue();
                    ph.log2_diff_max_tt_min_qt_intra_slice_chroma = r.read_ue();
                }
            }
        } else {
            ph.log2_diff_min_qt_min_cb_intra_slice_luma =
                sps->log2_diff_min_qt_min_cb_intra_slice_luma;
            ph.max_mtt_hierarchy_depth_intra_slice_luma =
                sps->max_mtt_hierarchy_depth_intra_slice_luma;
            ph.log2_diff_max_bt_min_qt_intra_slice_luma =
                sps->log2_diff_max_bt_min_qt_intra_slice_luma;
            ph.log2_diff_max_tt_min_qt_intra_slice_luma =
                sps->log2_diff_max_tt_min_qt_intra_slice_luma;
            ph.log2_diff_min_qt_min_cb_intra_slice_chroma =
                sps->log2_diff_min_qt_min_cb_intra_slice_chroma;
            ph.max_mtt_hierarchy_depth_intra_slice_chroma =
                sps->max_mtt_hierarchy_depth_intra_slice_chroma;
            ph.log2_diff_max_bt_min_qt_intra_slice_chroma =
                sps->log2_diff_max_bt_min_qt_intra_slice_chroma;
            ph.log2_diff_max_tt_min_qt_intra_slice_chroma =
                sps->log2_diff_max_tt_min_qt_intra_slice_chroma;
        }
        std::uint32_t min_qt_log2_size_intra_y =
            ph.log2_diff_min_qt_min_cb_intra_slice_luma + min_cb_log2_size_y;
        if (pps->cu_qp_delta_enabled_flag) {
            ph.cu_qp_delta_subdiv_intra_slice = r.read_ue();
            (void)ctb_log2_size_y;
            (void)min_qt_log2_size_intra_y;
        } else {
            ph.cu_qp_delta_subdiv_intra_slice = 0;
        }
        if (pps->cu_chroma_qp_offset_list_enabled_flag) {
            ph.cu_chroma_qp_offset_subdiv_intra_slice = r.read_ue();
        } else {
            ph.cu_chroma_qp_offset_subdiv_intra_slice = 0;
        }
    }

    if (ph.inter_slice_allowed_flag) {
        if (ph.partition_constraints_override_flag) {
            ph.log2_diff_min_qt_min_cb_inter_slice = r.read_ue();
            std::uint32_t min_qt_log2_size_inter_y =
                ph.log2_diff_min_qt_min_cb_inter_slice + min_cb_log2_size_y;
            ph.max_mtt_hierarchy_depth_inter_slice = r.read_ue();
            if (ph.max_mtt_hierarchy_depth_inter_slice != 0) {
                ph.log2_diff_max_bt_min_qt_inter_slice = r.read_ue();
                ph.log2_diff_max_tt_min_qt_inter_slice = r.read_ue();
            }
            (void)min_qt_log2_size_inter_y;
        } else {
            ph.log2_diff_min_qt_min_cb_inter_slice = sps->log2_diff_min_qt_min_cb_inter_slice;
            ph.max_mtt_hierarchy_depth_inter_slice = sps->max_mtt_hierarchy_depth_inter_slice;
            ph.log2_diff_max_bt_min_qt_inter_slice = sps->log2_diff_max_bt_min_qt_inter_slice;
            ph.log2_diff_max_tt_min_qt_inter_slice = sps->log2_diff_max_tt_min_qt_inter_slice;
        }
        // after partition for inter, then cu_qp stuff
        // computed min_qt for inter for bounds but not stored separately
        {
            std::uint32_t min_qt_log2_size_inter_y =
                ph.log2_diff_min_qt_min_cb_inter_slice + min_cb_log2_size_y;
            if (pps->cu_qp_delta_enabled_flag) {
                ph.cu_qp_delta_subdiv_inter_slice = r.read_ue();
                (void)min_qt_log2_size_inter_y;
            } else {
                ph.cu_qp_delta_subdiv_inter_slice = 0;
            }
            if (pps->cu_chroma_qp_offset_list_enabled_flag) {
                ph.cu_chroma_qp_offset_subdiv_inter_slice = r.read_ue();
            } else {
                ph.cu_chroma_qp_offset_subdiv_inter_slice = 0;
            }
        }
        if (sps->temporal_mvp_enabled_flag) {
            ph.temporal_mvp_enabled_flag = r.read_bit();
            if (ph.temporal_mvp_enabled_flag && pps->rpl_info_in_ph_flag) {
                if (ph.ref_pic_lists.rpl[1].num_ref_entries > 0)
                    ph.collocated_from_l0_flag = r.read_bit();
                else
                    ph.collocated_from_l0_flag = true;
                bool need_idx = false;
                if (ph.collocated_from_l0_flag) {
                    if (ph.ref_pic_lists.rpl[0].num_ref_entries > 1)
                        need_idx = true;
                } else {
                    if (ph.ref_pic_lists.rpl[1].num_ref_entries > 1)
                        need_idx = true;
                }
                if (need_idx) {
                    unsigned idx = ph.collocated_from_l0_flag ? 0 : 1;
                    std::uint32_t max = ph.ref_pic_lists.rpl[idx].num_ref_entries - 1;
                    ph.collocated_ref_idx = r.read_ue();
                    if (ph.collocated_ref_idx > max)
                        ph.collocated_ref_idx = max;
                } else {
                    ph.collocated_ref_idx = 0;
                }
            } else if (!pps->rpl_info_in_ph_flag) {
                // when rpl not in PH, temporal_mvp flag still consumed, but collocated not
                // (deferred to slice)
            }
        } else {
            ph.temporal_mvp_enabled_flag = false;
        }
        if (sps->mmvd_fullpel_only_enabled_flag) {
            ph.mmvd_fullpel_only_flag = r.read_bit();
        } else {
            ph.mmvd_fullpel_only_flag = false;
        }
        if (!pps->rpl_info_in_ph_flag || ph.ref_pic_lists.rpl[1].num_ref_entries > 0) {
            ph.mvd_l1_zero_flag = r.read_bit();
            if (sps->bdof_control_present_in_ph_flag) {
                ph.bdof_disabled_flag = r.read_bit();
            } else {
                if (!sps->bdof_control_present_in_ph_flag)
                    ph.bdof_disabled_flag = !sps->bdof_enabled_flag;
                else
                    ph.bdof_disabled_flag = true;
            }
            if (sps->dmvr_control_present_in_ph_flag) {
                ph.dmvr_disabled_flag = r.read_bit();
            } else {
                if (!sps->dmvr_control_present_in_ph_flag)
                    ph.dmvr_disabled_flag = !sps->dmvr_enabled_flag;
                else
                    ph.dmvr_disabled_flag = true;
            }
        } else {
            ph.mvd_l1_zero_flag = true;
            // inferred bdof/dmvr not consumed
        }
        if (sps->prof_control_present_in_ph_flag) {
            ph.prof_disabled_flag = r.read_bit();
        } else {
            ph.prof_disabled_flag = !sps->affine_prof_enabled_flag;
        }
        if ((pps->weighted_pred_flag || pps->weighted_bipred_flag) && pps->wp_info_in_ph_flag) {
            std::uint8_t num_ref_idx_active[2] = {0, 0};
            detail_ph::parse_pred_weight_table_ph(
                r, *sps, *pps, ph.ref_pic_lists, num_ref_idx_active
            );
        }
    }

    std::uint8_t qp_bd_offset = 6 * sps->bitdepth_minus8;
    (void)qp_bd_offset;
    if (pps->qp_delta_info_in_ph_flag) {
        ph.qp_delta = r.read_se();
    }

    if (sps->joint_cbcr_enabled_flag) {
        ph.joint_cbcr_sign_flag = r.read_bit();
    } else {
        ph.joint_cbcr_sign_flag = false;
    }

    if (sps->sao_enabled_flag && pps->sao_info_in_ph_flag) {
        ph.sao_luma_enabled_flag = r.read_bit();
        if (sps->chroma_format_idc != 0)
            ph.sao_chroma_enabled_flag = r.read_bit();
        else
            ph.sao_chroma_enabled_flag = false;
    } else {
        ph.sao_luma_enabled_flag = false;
        ph.sao_chroma_enabled_flag = false;
    }

    if (pps->dbf_info_in_ph_flag) {
        ph.deblocking_params_present_flag = r.read_bit();
    } else {
        ph.deblocking_params_present_flag = false;
    }

    if (ph.deblocking_params_present_flag) {
        if (!pps->deblocking_filter_disabled_flag) {
            ph.deblocking_filter_disabled_flag = r.read_bit();
            if (!ph.deblocking_filter_disabled_flag) {
                ph.luma_beta_offset_div2 = r.read_se();
                ph.luma_tc_offset_div2 = r.read_se();
                if (pps->chroma_tool_offsets_present_flag) {
                    ph.cb_beta_offset_div2 = r.read_se();
                    ph.cb_tc_offset_div2 = r.read_se();
                    ph.cr_beta_offset_div2 = r.read_se();
                    ph.cr_tc_offset_div2 = r.read_se();
                } else {
                    ph.cb_beta_offset_div2 = ph.luma_beta_offset_div2;
                    ph.cb_tc_offset_div2 = ph.luma_tc_offset_div2;
                    ph.cr_beta_offset_div2 = ph.luma_beta_offset_div2;
                    ph.cr_tc_offset_div2 = ph.luma_tc_offset_div2;
                }
            }
        } else {
            ph.deblocking_filter_disabled_flag = false;
        }
    } else {
        ph.deblocking_filter_disabled_flag = pps->deblocking_filter_disabled_flag;
        if (!ph.deblocking_filter_disabled_flag) {
            ph.luma_beta_offset_div2 = pps->luma_beta_offset_div2;
            ph.luma_tc_offset_div2 = pps->luma_tc_offset_div2;
            ph.cb_beta_offset_div2 = pps->cb_beta_offset_div2;
            ph.cb_tc_offset_div2 = pps->cb_tc_offset_div2;
            ph.cr_beta_offset_div2 = pps->cr_beta_offset_div2;
            ph.cr_tc_offset_div2 = pps->cr_tc_offset_div2;
        }
    }

    if (pps->picture_header_extension_present_flag) {
        ph.extension_length = r.read_ue();
        if (ph.extension_length > 256)
            ph.extension_length = 256;
        ph.extension_data.clear();
        ph.extension_data.reserve(ph.extension_length);
        for (std::uint32_t i = 0; i < ph.extension_length; ++i) {
            std::uint32_t b = r.read_bits(8);
            ph.extension_data.push_back(static_cast<std::uint8_t>(b));
        }
    }

    // rbsp_trailing_bits is handled by caller (PH NAL) but we can consume if present and
    // more_rbsp_data false. We do not consume slice data here.

    return ph;
}

template <typename Reader>
[[nodiscard]]
inline PictureHeader parse_ph(Reader& r, const SequenceParameterSet* sps) {
    return parse_ph(r, sps, nullptr);
}

/*
 * Single-pass convenience (no POC fields): reads just the leading fields.
 */
template <typename Reader>
[[nodiscard]]
inline PictureHeader parse_ph(Reader& r) {
    return parse_ph(r, nullptr, nullptr);
}

}  // namespace vvc
}  // namespace bs
