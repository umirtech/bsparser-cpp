// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "vvc_ph.hpp"
#include "vvc_pps.hpp"
#include "vvc_slice.hpp"
#include "vvc_sps.hpp"

#include <cstdint>
#include <vector>

namespace bs {
namespace vvc {

namespace detail_slice {

inline unsigned ceil_log2_s(uint32_t v) noexcept {
    unsigned b = 0;
    uint32_t p = 1;
    while (p < v) {
        p <<= 1;
        ++b;
    }
    return b;
}

template <typename Reader>
inline bool has_more_rbsp_data_generic_s(Reader& r) {
    if constexpr (requires { r.more_rbsp_data(); })
        return r.more_rbsp_data();
    else {
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
inline void parse_ref_pic_list_struct_s(
    Reader& r,
    PictureHeaderRefPicLists::List& out,
    uint32_t list_idx,
    uint32_t rpls_idx,
    const SequenceParameterSet& sps
) {
    out.num_ref_entries = r.read_ue();
    if (out.num_ref_entries > 64)
        out.num_ref_entries = 64;
    bool ltrp = false;
    if (sps.long_term_ref_pics_flag && rpls_idx < sps.num_ref_pic_lists[list_idx] &&
        out.num_ref_entries > 0)
        ltrp = r.read_bit();
    if (sps.long_term_ref_pics_flag && rpls_idx == sps.num_ref_pic_lists[list_idx])
        ltrp = true;
    out.ltrp_in_header_flag = ltrp;
    out.entries.clear();
    out.entries.reserve(out.num_ref_entries);
    out.poc_lsb_lt.clear();
    out.delta_poc_msb_cycle_present_flag.clear();
    out.delta_poc_msb_cycle_lt.clear();
    for (uint32_t i = 0; i < out.num_ref_entries; ++i) {
        PictureHeaderRefPicLists::Entry e{};
        if (sps.inter_layer_prediction_enabled_flag)
            e.inter_layer_ref_pic_flag = r.read_bit();
        if (!e.inter_layer_ref_pic_flag) {
            if (sps.long_term_ref_pics_flag)
                e.st_ref_pic_flag = r.read_bit();
            else
                e.st_ref_pic_flag = true;
            if (e.st_ref_pic_flag) {
                e.abs_delta_poc_st = r.read_ue();
                uint32_t ad = e.abs_delta_poc_st;
                if ((sps.weighted_pred_flag || sps.weighted_bipred_flag) && i != 0) {
                } else
                    ad = e.abs_delta_poc_st + 1;
                if (ad > 0)
                    e.strp_entry_sign_flag = r.read_bit();
            } else {
                if (!ltrp) {
                    uint32_t bits = sps.log2_max_pic_order_cnt_lsb_minus4 + 4;
                    e.rpls_poc_lsb_lt = r.read_bits(bits);
                }
            }
        } else {
            e.ilrp_idx = r.read_ue();
        }
        out.entries.push_back(e);
    }
}

template <typename Reader>
inline void parse_ref_pic_lists_s(
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
            if (sps.num_ref_pic_lists[i] == 0)
                rpl_sps_flag = false;
            else if (!pps.rpl1_idx_present_flag && i == 1)
                rpl_sps_flag = out.rpl[0].rpl_sps_flag;
        }
        lst.rpl_sps_flag = rpl_sps_flag;
        if (rpl_sps_flag) {
            if (sps.num_ref_pic_lists[i] > 1 && (i == 0 || (i == 1 && pps.rpl1_idx_present_flag))) {
                unsigned bits = ceil_log2_s(sps.num_ref_pic_lists[i]);
                lst.rpl_idx = bits ? r.read_bits(bits) : 0;
                if (lst.rpl_idx >= sps.num_ref_pic_lists[i])
                    lst.rpl_idx = 0;
            } else if (sps.num_ref_pic_lists[i] == 1)
                lst.rpl_idx = 0;
            else if (i == 1 && !pps.rpl1_idx_present_flag)
                lst.rpl_idx = out.rpl[0].rpl_idx;
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
            } else
                lst.num_ref_entries = 0;
        } else {
            parse_ref_pic_list_struct_s(r, lst, i, sps.num_ref_pic_lists[i], sps);
        }
        int num_ltrp = 0;
        for (auto& e : lst.entries)
            if (!e.inter_layer_ref_pic_flag && !e.st_ref_pic_flag)
                ++num_ltrp;
        lst.poc_lsb_lt.clear();
        lst.delta_poc_msb_cycle_present_flag.clear();
        lst.delta_poc_msb_cycle_lt.clear();
        for (int j = 0; j < num_ltrp; ++j) {
            if (lst.ltrp_in_header_flag) {
                uint32_t bits = sps.log2_max_pic_order_cnt_lsb_minus4 + 4;
                lst.poc_lsb_lt.push_back(r.read_bits(bits));
            }
            bool p = r.read_bit();
            lst.delta_poc_msb_cycle_present_flag.push_back(p);
            if (p) {
                uint32_t max = 1u << (32 - sps.log2_max_pic_order_cnt_lsb_minus4 - 4);
                uint32_t v = r.read_ue();
                if (v >= max)
                    v = max - 1;
                lst.delta_poc_msb_cycle_lt.push_back(v);
            } else
                lst.delta_poc_msb_cycle_lt.push_back(0);
        }
    }
}

template <typename Reader>
inline void parse_pred_weight_table_s(
    Reader& r,
    const SequenceParameterSet& sps,
    const PictureParameterSet& pps,
    PictureHeaderRefPicLists& ref_lists,
    uint32_t num_ref_idx_active[2]
) {
    uint32_t luma_log2_weight_denom = r.read_ue();
    if (luma_log2_weight_denom > 7)
        luma_log2_weight_denom = 7;
    if (sps.chroma_format_idc != 0) {
        (void)r.read_se();
    }
    uint32_t num_l0 = 0;
    if (pps.wp_info_in_ph_flag) {
        uint32_t max0 = ref_lists.rpl[0].num_ref_entries;
        if (max0 > 15)
            max0 = 15;
        num_l0 = r.read_ue();
        if (num_l0 > max0)
            num_l0 = max0;
    } else
        num_l0 = num_ref_idx_active[0];
    std::vector<bool> l0_luma(num_l0, false), l0_chroma(num_l0, false);
    for (uint32_t i = 0; i < num_l0; ++i)
        l0_luma[i] = r.read_bit();
    if (sps.chroma_format_idc != 0)
        for (uint32_t i = 0; i < num_l0; ++i)
            l0_chroma[i] = r.read_bit();
    for (uint32_t i = 0; i < num_l0; ++i) {
        if (l0_luma[i]) {
            (void)r.read_se();
            (void)r.read_se();
        }
        if (sps.chroma_format_idc != 0 && l0_chroma[i]) {
            for (int j = 0; j < 2; ++j) {
                (void)r.read_se();
                (void)r.read_se();
            }
        }
    }
    uint32_t num_l1 = 0;
    if (pps.weighted_bipred_flag && ref_lists.rpl[1].num_ref_entries > 0) {
        if (pps.wp_info_in_ph_flag) {
            uint32_t max1 = ref_lists.rpl[1].num_ref_entries;
            if (max1 > 15)
                max1 = 15;
            num_l1 = r.read_ue();
            if (num_l1 > max1)
                num_l1 = max1;
        } else
            num_l1 = num_ref_idx_active[1];
    }
    std::vector<bool> l1_luma(num_l1, false);
    for (uint32_t i = 0; i < num_l1; ++i)
        l1_luma[i] = r.read_bit();
    std::vector<bool> l1_chroma(num_l1, false);
    if (sps.chroma_format_idc != 0)
        for (uint32_t i = 0; i < num_l1; ++i)
            l1_chroma[i] = r.read_bit();
    for (uint32_t i = 0; i < num_l1; ++i) {
        if (l1_luma[i]) {
            (void)r.read_se();
            (void)r.read_se();
        }
        if (sps.chroma_format_idc != 0 && l1_chroma[i]) {
            for (int j = 0; j < 2; ++j) {
                (void)r.read_se();
                (void)r.read_se();
            }
        }
    }
}

}  // namespace detail_slice

/*
 * -----------------------------------------------------------
 * VVC slice segment header parser (H.266 §7.3.2.11)
 * -----------------------------------------------------------
 */
template <typename Reader>
[[nodiscard]]
inline SliceHeader parse_slice_header(
    Reader& r,
    const SequenceParameterSet* sps,
    const PictureParameterSet* pps,
    const PictureHeader* stored_ph,
    int nal_unit_type = -1
) {
    SliceHeader sh;
    sh.picture_header_in_slice_header_flag = r.read_bit();
    const PictureHeader* ph_ptr = nullptr;

    if (sh.picture_header_in_slice_header_flag) {
        // Embedded PH: parse it
        if (sps != nullptr) {
            // need both sps and pps for full PH; pps may be null on first pass, use sps-only parse
            if (pps != nullptr)
                sh.ph = parse_ph(r, sps, pps);
            else
                sh.ph = parse_ph(r, sps);
        } else {
            sh.ph = parse_ph(r, nullptr);
        }
        sh.pps_id = sh.ph.pps_id;
        ph_ptr = &sh.ph;
        if (sps == nullptr) {
            return sh;
        }
        // For embedded case, re-resolve pps/sps already done externally; continue with ph_ptr
    } else {
        ph_ptr = stored_ph;
        if (ph_ptr) {
            sh.ph = *ph_ptr;
            sh.pps_id = ph_ptr->pps_id;
        } else {
            // No PH available, cannot continue beyond this point but still attempt minimal parsing
            // if sps present?
            sh.pps_id = 0;
        }
    }

    if (sps == nullptr) {
        // Without SPS we cannot parse slice addressing etc; return after PH.
        return sh;
    }

    // Resolve PPS for slice (when not embedded, ph_ptr gives pps_id; when embedded, sh.ph gives it)
    // pps pointer may be null if not provided, but we need it for many fields.
    // If pps is null, infer no_tiles etc.
    // Subpic handling
    if (sps->subpic_info_present_flag) {
        unsigned bits = static_cast<unsigned>(sps->subpic_id_len_minus1) + 1;
        if (bits > 32)
            bits = 32;
        sh.subpic_id = r.read_bits(bits);
        // find curr_subpic_idx via pps subpic_id mapping if pps available
        if (pps && !pps->subpic_id.empty()) {
            bool found = false;
            for (size_t i = 0; i < pps->subpic_id.size(); ++i) {
                if (pps->subpic_id[i] == sh.subpic_id) {
                    sh.curr_subpic_idx = static_cast<uint32_t>(i);
                    found = true;
                    break;
                }
            }
            if (!found)
                sh.curr_subpic_idx = 0;
        } else {
            sh.curr_subpic_idx = 0;
        }
    } else {
        sh.curr_subpic_idx = 0;
        sh.subpic_id = 0;
    }

    // Determine num_slices_in_subpic and num_tiles
    uint32_t num_tiles_in_pic = 1;
    bool rect_flag = true;
    if (pps) {
        num_tiles_in_pic = pps->num_tiles_in_pic ? pps->num_tiles_in_pic : 1;
        rect_flag = pps->rect_slice_flag;
    } else {
        rect_flag = true;
        num_tiles_in_pic = 1;
    }

    // Compute slice_address bits
    uint32_t num_slices_in_subpic = 1;
    if (pps) {
        if (pps->rect_slice_flag) {
            if (pps->single_slice_per_subpic_flag) {
                // each subpic is one slice
                num_slices_in_subpic = 1;
            } else {
                // approximate via total slices
                uint32_t total = pps->num_slices_in_pic_minus1 + 1;
                // if subpics present, distribute? use total
                num_slices_in_subpic = total;
            }
        } else {
            num_slices_in_subpic = num_tiles_in_pic;
        }
    }

    if ((rect_flag && num_slices_in_subpic > 1) || (!rect_flag && num_tiles_in_pic > 1)) {
        unsigned bits, max;
        if (!rect_flag) {
            bits = detail_slice::ceil_log2_s(num_tiles_in_pic);
            max = num_tiles_in_pic - 1;
        } else {
            bits = detail_slice::ceil_log2_s(num_slices_in_subpic);
            max = num_slices_in_subpic - 1;
        }
        if (bits) {
            sh.slice_address = r.read_bits(bits);
            if (sh.slice_address > max)
                sh.slice_address = max;
        } else
            sh.slice_address = 0;
        sh.slice_segment_address = sh.slice_address;
    } else {
        sh.slice_address = 0;
        sh.slice_segment_address = 0;
    }

    // sh_extra_bits
    sh.extra_bits.clear();
    if (sps) {
        for (uint32_t i = 0; i < sps->num_extra_sh_bits; ++i) {
            bool b = r.read_bit();
            sh.extra_bits.push_back(b);
        }
        // Also handle num_extra_sh_bytes *8 with present flags similarly to PH but simplified as
        // num_extra_sh_bits
    }

    if (!rect_flag && pps) {
        uint32_t tiles_minus_address =
            num_tiles_in_pic > sh.slice_address ? num_tiles_in_pic - sh.slice_address : 1;
        if (tiles_minus_address > 1) {
            sh.num_tiles_in_slice_minus1 = r.read_ue();
            if (sh.num_tiles_in_slice_minus1 >= tiles_minus_address)
                sh.num_tiles_in_slice_minus1 = tiles_minus_address - 1;
        } else
            sh.num_tiles_in_slice_minus1 = 0;
    } else {
        sh.num_tiles_in_slice_minus1 = 0;
    }

    // slice_type
    if (ph_ptr && ph_ptr->inter_slice_allowed_flag) {
        uint32_t raw = r.read_ue();
        if (raw > 2)
            raw = 2;
        sh.slice_type = static_cast<SliceType>(raw);
    } else {
        sh.slice_type = SliceType::I;
    }

    // no_output_of_prior_pics_flag for IDR/CRA/GDR
    if (nal_unit_type == 7 || nal_unit_type == 8 || nal_unit_type == 9 || nal_unit_type == 10) {
        sh.no_output_of_prior_pics_flag = r.read_bit();
    } else {
        sh.no_output_of_prior_pics_flag = false;
    }

    // ALF
    if (sps->alf_enabled_flag) {
        if (pps && !pps->alf_info_in_ph_flag) {
            sh.alf_enabled_flag = r.read_bit();
            if (sh.alf_enabled_flag) {
                sh.num_alf_aps_ids_luma = static_cast<uint8_t>(r.read_bits(3));
                sh.alf_aps_id_luma.clear();
                sh.alf_aps_id_luma.reserve(sh.num_alf_aps_ids_luma);
                for (int i = 0; i < sh.num_alf_aps_ids_luma; ++i)
                    sh.alf_aps_id_luma.push_back(static_cast<uint8_t>(r.read_bits(3)));
                if (sps->chroma_format_idc != 0) {
                    sh.alf_cb_enabled_flag = r.read_bit();
                    sh.alf_cr_enabled_flag = r.read_bit();
                }
                if (sh.alf_cb_enabled_flag || sh.alf_cr_enabled_flag) {
                    sh.alf_aps_id_chroma = static_cast<uint8_t>(r.read_bits(3));
                }
                if (sps->ccalf_enabled_flag) {
                    sh.alf_cc_cb_enabled_flag = r.read_bit();
                    if (sh.alf_cc_cb_enabled_flag)
                        sh.alf_cc_cb_aps_id = static_cast<uint8_t>(r.read_bits(3));
                    sh.alf_cc_cr_enabled_flag = r.read_bit();
                    if (sh.alf_cc_cr_enabled_flag)
                        sh.alf_cc_cr_aps_id = static_cast<uint8_t>(r.read_bits(3));
                }
            }
        } else if (ph_ptr) {
            // infer from PH
            sh.alf_enabled_flag = ph_ptr->alf_enabled_flag;
            sh.num_alf_aps_ids_luma = ph_ptr->num_alf_aps_ids_luma;
            sh.alf_aps_id_luma = ph_ptr->alf_aps_id_luma;
            sh.alf_cb_enabled_flag = ph_ptr->alf_cb_enabled_flag;
            sh.alf_cr_enabled_flag = ph_ptr->alf_cr_enabled_flag;
            sh.alf_aps_id_chroma = ph_ptr->alf_aps_id_chroma;
            sh.alf_cc_cb_enabled_flag = ph_ptr->alf_cc_cb_enabled_flag;
            sh.alf_cc_cb_aps_id = ph_ptr->alf_cc_cb_aps_id;
            sh.alf_cc_cr_enabled_flag = ph_ptr->alf_cc_cr_enabled_flag;
            sh.alf_cc_cr_aps_id = ph_ptr->alf_cc_cr_aps_id;
        }
    }

    if (sh.picture_header_in_slice_header_flag) {
        if (ph_ptr) {
            sh.lmcs_used_flag = ph_ptr->lmcs_enabled_flag;
            sh.explicit_scaling_list_used_flag = ph_ptr->explicit_scaling_list_enabled_flag;
        }
    } else {
        if (ph_ptr && ph_ptr->lmcs_enabled_flag)
            sh.lmcs_used_flag = r.read_bit();
        else
            sh.lmcs_used_flag = false;
        if (ph_ptr && ph_ptr->explicit_scaling_list_enabled_flag)
            sh.explicit_scaling_list_used_flag = r.read_bit();
        else
            sh.explicit_scaling_list_used_flag = false;
    }

    // Ref pic lists
    const PictureHeaderRefPicLists* ref_lists_ptr = nullptr;
    bool need_rpl = false;
    if (pps && !pps->rpl_info_in_ph_flag) {
        bool is_idr = (nal_unit_type == 7 || nal_unit_type == 8);
        if ((nal_unit_type != 7 && nal_unit_type != 8) || (sps && sps->idr_rpl_present_flag)) {
            (void)is_idr;
            need_rpl = true;
        } else {
            // check if IDR but without rpl present -> no rpl
            need_rpl = false;
        }
        // Simplified: if !pps_rpl_info_in_ph_flag and slice is VCL, we need RPL unless IDR with
        // !idr_rpl_present
        if (sps && !sps->idr_rpl_present_flag && (nal_unit_type == 7 || nal_unit_type == 8))
            need_rpl = false;
        else
            need_rpl = !pps->rpl_info_in_ph_flag;
    }
    // If need_rpl, parse; else use PH's lists
    if (pps && need_rpl) {
        // If we are here, slice header contains RPL
        detail_slice::parse_ref_pic_lists_s(r, *sps, *pps, sh.ref_pic_lists);
        ref_lists_ptr = &sh.ref_pic_lists;
    } else if (ph_ptr) {
        sh.ref_pic_lists = ph_ptr->ref_pic_lists;
        ref_lists_ptr = &sh.ref_pic_lists;
    } else {
        ref_lists_ptr = &sh.ref_pic_lists;
    }

    // num_ref_idx_active_override
    bool has_rpl0 = ref_lists_ptr && ref_lists_ptr->rpl[0].num_ref_entries > 0;
    bool has_rpl1 = ref_lists_ptr && ref_lists_ptr->rpl[1].num_ref_entries > 0;
    bool need_override = false;
    if (sh.slice_type != SliceType::I && has_rpl0 && ref_lists_ptr->rpl[0].num_ref_entries > 1)
        need_override = true;
    if (sh.slice_type == SliceType::B && has_rpl1 && ref_lists_ptr->rpl[1].num_ref_entries > 1)
        need_override = true;
    if (need_override) {
        sh.num_ref_idx_active_override_flag = r.read_bit();
        if (sh.num_ref_idx_active_override_flag) {
            for (int i = 0; i < (sh.slice_type == SliceType::B ? 2 : 1); ++i) {
                if (ref_lists_ptr->rpl[i].num_ref_entries > 1) {
                    sh.num_ref_idx_active_minus1[i] = r.read_ue();
                    if (sh.num_ref_idx_active_minus1[i] > 14)
                        sh.num_ref_idx_active_minus1[i] = 14;
                } else
                    sh.num_ref_idx_active_minus1[i] = 0;
            }
        }
    } else {
        sh.num_ref_idx_active_override_flag = false;
    }

    // Derive num_ref_idx_active
    for (int i = 0; i < 2; ++i)
        sh.num_ref_idx_active[i] = 0;
    for (int i = 0; i < 2; ++i) {
        if (sh.slice_type == SliceType::B || (sh.slice_type == SliceType::P && i == 0)) {
            if (sh.num_ref_idx_active_override_flag) {
                sh.num_ref_idx_active[i] = sh.num_ref_idx_active_minus1[i] + 1;
            } else if (pps) {
                uint32_t def = pps->num_ref_idx_default_active_minus1[i] + 1;
                uint32_t entries = ref_lists_ptr ? ref_lists_ptr->rpl[i].num_ref_entries : 0;
                sh.num_ref_idx_active[i] = entries < def ? entries : def;
            } else {
                sh.num_ref_idx_active[i] = 1;
            }
            if (sh.num_ref_idx_active[i] == 0)
                sh.num_ref_idx_active[i] = 1;
        }
    }

    if (sh.slice_type != SliceType::I) {
        if (pps && pps->cabac_init_present_flag)
            sh.cabac_init_flag = r.read_bit();
        else
            sh.cabac_init_flag = false;
        if (ph_ptr && ph_ptr->temporal_mvp_enabled_flag) {
            if (pps && !pps->rpl_info_in_ph_flag) {
                if (sh.slice_type == SliceType::B)
                    sh.collocated_from_l0_flag = r.read_bit();
                else
                    sh.collocated_from_l0_flag = true;
                bool need_idx = false;
                if (sh.collocated_from_l0_flag && sh.num_ref_idx_active[0] > 1)
                    need_idx = true;
                if (!sh.collocated_from_l0_flag && sh.num_ref_idx_active[1] > 1)
                    need_idx = true;
                if (need_idx) {
                    unsigned idx = sh.collocated_from_l0_flag ? 0 : 1;
                    uint32_t max = sh.num_ref_idx_active[idx] - 1;
                    sh.collocated_ref_idx = r.read_ue();
                    if (sh.collocated_ref_idx > max)
                        sh.collocated_ref_idx = max;
                } else
                    sh.collocated_ref_idx = 0;
            } else if (ph_ptr) {
                if (sh.slice_type == SliceType::B)
                    sh.collocated_from_l0_flag = ph_ptr->collocated_from_l0_flag;
                else
                    sh.collocated_from_l0_flag = true;
                sh.collocated_ref_idx = ph_ptr->collocated_ref_idx;
            }
        }
        if (pps && !pps->wp_info_in_ph_flag &&
            ((pps->weighted_pred_flag && sh.slice_type == SliceType::P) ||
             (pps->weighted_bipred_flag && sh.slice_type == SliceType::B))) {
            // pred_weight_table
            detail_slice::parse_pred_weight_table_s(
                r, *sps, *pps, sh.ref_pic_lists, sh.num_ref_idx_active
            );
        }
    }

    if (pps && !pps->qp_delta_info_in_ph_flag) {
        sh.qp_delta = r.read_se();
    } else if (ph_ptr) {
        sh.qp_delta = ph_ptr->qp_delta;
    }

    if (pps && pps->slice_chroma_qp_offsets_present_flag) {
        sh.cb_qp_offset = r.read_se();
        sh.cr_qp_offset = r.read_se();
        if (sps->joint_cbcr_enabled_flag)
            sh.joint_cbcr_qp_offset = r.read_se();
        else
            sh.joint_cbcr_qp_offset = 0;
        // validate range not needed
    } else {
        sh.cb_qp_offset = 0;
        sh.cr_qp_offset = 0;
        sh.joint_cbcr_qp_offset = 0;
    }

    if (pps && pps->cu_chroma_qp_offset_list_enabled_flag)
        sh.cu_chroma_qp_offset_enabled_flag = r.read_bit();
    else
        sh.cu_chroma_qp_offset_enabled_flag = false;

    if (sps->sao_enabled_flag && pps && !pps->sao_info_in_ph_flag) {
        sh.sao_luma_used_flag = r.read_bit();
        if (sps->chroma_format_idc != 0)
            sh.sao_chroma_used_flag = r.read_bit();
        else
            sh.sao_chroma_used_flag = ph_ptr ? ph_ptr->sao_chroma_enabled_flag : false;
    } else if (ph_ptr) {
        sh.sao_luma_used_flag = ph_ptr->sao_luma_enabled_flag;
        sh.sao_chroma_used_flag = ph_ptr->sao_chroma_enabled_flag;
    }

    if (pps && pps->deblocking_filter_override_enabled_flag && pps && !pps->dbf_info_in_ph_flag) {
        sh.deblocking_params_present_flag = r.read_bit();
    } else {
        sh.deblocking_params_present_flag = false;
    }
    if (sh.deblocking_params_present_flag) {
        if (pps && !pps->deblocking_filter_disabled_flag)
            sh.deblocking_filter_disabled_flag = r.read_bit();
        else
            sh.deblocking_filter_disabled_flag = false;
        if (!sh.deblocking_filter_disabled_flag) {
            sh.luma_beta_offset_div2 = r.read_se();
            sh.luma_tc_offset_div2 = r.read_se();
            if (pps && pps->chroma_tool_offsets_present_flag) {
                sh.cb_beta_offset_div2 = r.read_se();
                sh.cb_tc_offset_div2 = r.read_se();
                sh.cr_beta_offset_div2 = r.read_se();
                sh.cr_tc_offset_div2 = r.read_se();
            } else {
                sh.cb_beta_offset_div2 = sh.luma_beta_offset_div2;
                sh.cb_tc_offset_div2 = sh.luma_tc_offset_div2;
                sh.cr_beta_offset_div2 = sh.luma_beta_offset_div2;
                sh.cr_tc_offset_div2 = sh.luma_tc_offset_div2;
            }
        }
    } else {
        if (ph_ptr) {
            sh.deblocking_filter_disabled_flag = ph_ptr->deblocking_filter_disabled_flag;
            sh.luma_beta_offset_div2 = ph_ptr->luma_beta_offset_div2;
            sh.luma_tc_offset_div2 = ph_ptr->luma_tc_offset_div2;
            sh.cb_beta_offset_div2 = ph_ptr->cb_beta_offset_div2;
            sh.cb_tc_offset_div2 = ph_ptr->cb_tc_offset_div2;
            sh.cr_beta_offset_div2 = ph_ptr->cr_beta_offset_div2;
            sh.cr_tc_offset_div2 = ph_ptr->cr_tc_offset_div2;
        } else if (pps) {
            sh.deblocking_filter_disabled_flag = pps->deblocking_filter_disabled_flag;
        }
    }

    if (sps->dep_quant_enabled_flag)
        sh.dep_quant_used_flag = r.read_bit();
    else
        sh.dep_quant_used_flag = false;
    if (sps->sign_data_hiding_enabled_flag && !sh.dep_quant_used_flag)
        sh.sign_data_hiding_used_flag = r.read_bit();
    else
        sh.sign_data_hiding_used_flag = false;
    if (sps->transform_skip_enabled_flag && !sh.dep_quant_used_flag &&
        !sh.sign_data_hiding_used_flag)
        sh.ts_residual_coding_disabled_flag = r.read_bit();
    else
        sh.ts_residual_coding_disabled_flag = false;
    if (!sh.ts_residual_coding_disabled_flag && sps->ts_residual_coding_rice_present_in_sh_flag) {
        sh.ts_residual_coding_rice_idx_minus1 = r.read_bits(3);
    } else
        sh.ts_residual_coding_rice_idx_minus1 = 0;
    if (sps->reverse_last_sig_coeff_enabled_flag)
        sh.reverse_last_sig_coeff_flag = r.read_bit();
    else
        sh.reverse_last_sig_coeff_flag = false;

    if (pps && pps->slice_header_extension_present_flag) {
        sh.slice_header_extension_length = r.read_ue();
        if (sh.slice_header_extension_length > 256)
            sh.slice_header_extension_length = 256;
        sh.slice_header_extension_data.clear();
        sh.slice_header_extension_data.reserve(sh.slice_header_extension_length);
        for (uint32_t i = 0; i < sh.slice_header_extension_length; ++i) {
            uint32_t b = r.read_bits(8);
            sh.slice_header_extension_data.push_back(static_cast<uint8_t>(b));
        }
    }

    // entry points
    sh.num_entry_points = 0;
    if (sps->entry_point_offsets_present_flag) {
        bool entropy_sync = sps->entropy_coding_sync_enabled_flag;
        uint32_t height = 0;
        if (pps && pps->rect_slice_flag) {
            // Simplified calculation: use slice bounds not fully accurate
            // For single slice per pic, width*height approximation
            uint32_t width_in_tiles = 1;
            int slice_idx = static_cast<int>(sh.slice_address);
            // Use pps arrays if available to compute more accurately
            if (pps && slice_idx >= 0 &&
                static_cast<size_t>(slice_idx) < pps->slice_width_in_tiles_minus1.size()) {
                width_in_tiles = pps->slice_width_in_tiles_minus1[slice_idx] + 1;
            }
            // height
            if (entropy_sync) {
                if (pps && slice_idx >= 0 &&
                    static_cast<size_t>(slice_idx) < pps->slice_height_in_ctus.size())
                    height = pps->slice_height_in_ctus[slice_idx];
                else
                    height = 1;
            } else {
                if (pps && slice_idx >= 0 &&
                    static_cast<size_t>(slice_idx) < pps->slice_height_in_tiles_minus1.size())
                    height = pps->slice_height_in_tiles_minus1[slice_idx] + 1;
                else
                    height = 1;
            }
            sh.num_entry_points = width_in_tiles * height;
        } else {
            // non-rect
            uint32_t num_tiles = num_tiles_in_pic;
            uint32_t start = sh.slice_address;
            uint32_t end = start + sh.num_tiles_in_slice_minus1;
            if (end >= num_tiles)
                end = num_tiles - 1;
            for (uint32_t ti = start; ti <= end; ++ti) {
                uint32_t tile_y =
                    pps ? ti / (pps->num_tile_columns ? pps->num_tile_columns : 1) : 0;
                uint32_t h =
                    pps && tile_y < pps->row_height_val.size() ? pps->row_height_val[tile_y] : 1;
                sh.num_entry_points += entropy_sync ? h : 1;
            }
        }
        if (sh.num_entry_points > 0)
            sh.num_entry_points--;
        if (sh.num_entry_points > 0) {
            sh.entry_offset_len_minus1 = r.read_ue();
            if (sh.entry_offset_len_minus1 > 31)
                sh.entry_offset_len_minus1 = 31;
            sh.entry_point_offset_minus1.clear();
            sh.entry_point_offset_minus1.reserve(sh.num_entry_points);
            for (uint32_t i = 0; i < sh.num_entry_points; ++i) {
                uint32_t v = r.read_bits(sh.entry_offset_len_minus1 + 1);
                sh.entry_point_offset_minus1.push_back(v);
            }
        }
        if (!r.byte_aligned()) {
            try {
                bool one = r.read_bit();
                (void)one;
                while (!r.byte_aligned()) {
                    bool z = r.read_bit();
                    (void)z;
                    if (r.byte_aligned())
                        break;
                    if (!r.has_more_bits())
                        break;
                }
            } catch (...) {
            }
        }
    }
    // byte_alignment: final alignment for slice header (spec always byte aligns)
    if (!r.byte_aligned()) {
        try {
            bool one = r.read_bit();
            (void)one;
            while (!r.byte_aligned()) {
                bool z = r.read_bit();
                (void)z;
                if (r.byte_aligned())
                    break;
                if (!r.has_more_bits())
                    break;
            }
        } catch (...) {
        }
    }

    return sh;
}

// Overload without nal_type for backwards compat
template <typename Reader>
[[nodiscard]]
inline SliceHeader parse_slice_header(
    Reader& r,
    const SequenceParameterSet* sps,
    const PictureParameterSet* pps,
    const PictureHeader* stored_ph
) {
    return parse_slice_header(r, sps, pps, stored_ph, -1);
}

}  // namespace vvc
}  // namespace bs
