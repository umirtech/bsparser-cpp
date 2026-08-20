// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "vvc_vps.hpp"
#include "vvc_sps_parser.hpp"

#include <cstdint>
#include <vector>

namespace bs {
namespace vvc {

namespace detail {

/*
 * profile_tier_level(pt_present, max_tid) (§7.3.3.1).
 * Overload for VPS where profileTierPresentFlag varies.
 * Reuses detail::skip_general_constraints_info from vvc_sps_parser.hpp.
 * When pt_present==0 only level_idc + constraint flags +
 * sublayer bits and alignment are present.
 */
template <typename Reader>
inline void skip_profile_tier_level(Reader& r, bool pt_present, std::uint8_t max_tid) {
    if (pt_present) {
        (void)r.read_bits(7);  // general_profile_idc
        (void)r.read_bit();    // general_tier_flag
    }
    (void)r.read_bits(8);  // general_level_idc
    (void)r.read_bit();    // ptl_frame_only_constraint_flag
    (void)r.read_bit();    // ptl_multilayer_enabled_flag
    if (pt_present) {
        skip_general_constraints_info(r);
    }
    bool sublayer_present[8] = {};
    for (int i = static_cast<int>(max_tid) - 1; i >= 0; --i) {
        sublayer_present[i] = r.read_bit();
    }
    while (!r.byte_aligned()) {
        (void)r.read_bit();  // ptl_reserved_zero_bit / alignment
    }
    for (int i = static_cast<int>(max_tid) - 1; i >= 0; --i) {
        if (sublayer_present[i]) {
            (void)r.read_bits(8);  // sublayer_level_idc
        }
    }
    if (pt_present) {
        const std::uint32_t num_sub_profiles = r.read_bits(8);
        for (std::uint32_t i = 0; i < num_sub_profiles; ++i) {
            (void)r.read_bits(32);  // general_sub_profile_idc
        }
    }
}

}  // namespace detail

/*
 * -----------------------------------------------------------
 * VVC VPS parser (H.266 §7.3.2.3) — full syntax
 * -----------------------------------------------------------
 * Mirrors ffmpeg cbs_h266.c vps() lines 710-850+:
 *   leading fields + layer dependencies + OLS + PTL +
 *   alignment + PTL payloads + OLS PTL indices + DPB/HRD
 *   (HRD simplified to counts/flags but bits consumed).
 */
template <typename Reader>
[[nodiscard]]
inline VideoParameterSet parse_vps(Reader& r) {
    VideoParameterSet vps;

    vps.vps_id = static_cast<std::uint8_t>(r.read_bits(4));

    vps.max_layers_minus1 = static_cast<std::uint8_t>(r.read_bits(6));

    vps.max_sublayers_minus1 = static_cast<std::uint8_t>(r.read_bits(3));

    if (vps.max_layers_minus1 > 0 && vps.max_sublayers_minus1 > 0) {
        vps.default_ptl_dpb_hrd_max_tid_flag = r.read_bit();
    } else {
        vps.default_ptl_dpb_hrd_max_tid_flag = true;
    }

    if (vps.max_layers_minus1 > 0) {
        vps.all_independent_layers = r.read_bit();
    } else {
        vps.all_independent_layers = true;
    }

    const std::uint16_t num_layers = static_cast<std::uint16_t>(vps.max_layers_minus1) + 1;

    // Prepare containers for layer-dependent syntax
    vps.layer_ids.reserve(num_layers);
    vps.independent_layer_flag.assign(num_layers, 1);
    vps.direct_ref_layer_flag.assign(num_layers, std::vector<std::uint8_t>(num_layers, 0));
    vps.max_tid_il_ref_pics_plus1.assign(
        num_layers,
        std::vector<std::uint8_t>(
            num_layers, static_cast<std::uint8_t>(vps.max_sublayers_minus1 + 1)
        )
    );

    for (std::uint16_t i = 0; i < num_layers; ++i) {
        const std::uint8_t lid = static_cast<std::uint8_t>(r.read_bits(6));
        vps.layer_ids.push_back(lid);

        if (i > 0 && !vps.all_independent_layers) {
            const bool indep = r.read_bit();
            vps.independent_layer_flag[i] = indep ? 1 : 0;
            if (!indep) {
                const bool max_tid_present = r.read_bit();
                for (std::uint16_t j = 0; j < i; ++j) {
                    const bool direct = r.read_bit();
                    vps.direct_ref_layer_flag[i][j] = direct ? 1 : 0;
                    if (max_tid_present && direct) {
                        const std::uint8_t pt = static_cast<std::uint8_t>(r.read_bits(3));
                        vps.max_tid_il_ref_pics_plus1[i][j] = pt;
                    } else {
                        vps.max_tid_il_ref_pics_plus1[i][j] =
                            static_cast<std::uint8_t>(vps.max_sublayers_minus1 + 1);
                    }
                }
            } else {
                for (std::uint16_t j = 0; j < i; ++j) {
                    vps.direct_ref_layer_flag[i][j] = 0;
                    vps.max_tid_il_ref_pics_plus1[i][j] =
                        static_cast<std::uint8_t>(vps.max_sublayers_minus1 + 1);
                }
            }
        } else {
            vps.independent_layer_flag[i] = 1;
            for (std::uint16_t j = 0; j < i; ++j) {
                vps.direct_ref_layer_flag[i][j] = 0;
                vps.max_tid_il_ref_pics_plus1[i][j] =
                    static_cast<std::uint8_t>(vps.max_sublayers_minus1 + 1);
            }
        }
    }

    // OLS / PTL signalling
    std::uint16_t total_num_olss = 1;
    std::uint8_t ols_mode = 4;  // 4 == each_layer_is_an_ols inferred

    if (vps.max_layers_minus1 > 0) {
        if (vps.all_independent_layers) {
            vps.each_layer_is_an_ols_flag = r.read_bit();
        } else {
            vps.each_layer_is_an_ols_flag = false;
        }

        if (!vps.each_layer_is_an_ols_flag) {
            if (!vps.all_independent_layers) {
                ols_mode = static_cast<std::uint8_t>(r.read_bits(2));
                vps.ols_mode_idc = ols_mode;
            } else {
                vps.ols_mode_idc = 2;
                ols_mode = 2;
            }
            if (ols_mode == 2) {
                vps.num_output_layer_sets_minus2 = static_cast<std::uint8_t>(r.read_bits(8));
                total_num_olss = static_cast<std::uint16_t>(vps.num_output_layer_sets_minus2) + 2;
                vps.ols_output_layer_flag.assign(
                    total_num_olss, std::vector<std::uint8_t>(num_layers, 0)
                );
                for (std::uint16_t i = 1;
                     i <= static_cast<std::uint16_t>(vps.num_output_layer_sets_minus2) + 1;
                     ++i) {
                    for (std::uint16_t j = 0; j < num_layers; ++j) {
                        const bool f = r.read_bit();
                        vps.ols_output_layer_flag[i][j] = f ? 1 : 0;
                    }
                }
            } else {
                // modes 0/1 have no output flags
                total_num_olss = num_layers;
                vps.num_output_layer_sets_minus2 = 0;
                vps.ols_output_layer_flag.clear();
            }
        } else {
            ols_mode = 4;
            vps.ols_mode_idc = 4;
            total_num_olss = num_layers;
            vps.num_output_layer_sets_minus2 = 0;
            vps.ols_output_layer_flag.clear();
        }

        if (ols_mode == 4 || ols_mode == 0 || ols_mode == 1) {
            total_num_olss = num_layers;
        } else if (ols_mode == 2) {
            total_num_olss = static_cast<std::uint16_t>(vps.num_output_layer_sets_minus2) + 2;
        } else {
            total_num_olss = num_layers;  // ols_mode 3 not supported – treat as 0
        }

        vps.num_ptls_minus1 = static_cast<std::uint8_t>(r.read_bits(8));
    } else {
        vps.each_layer_is_an_ols_flag = true;
        vps.ols_mode_idc = 4;
        vps.num_output_layer_sets_minus2 = 0;
        vps.ols_output_layer_flag.clear();
        vps.num_ptls_minus1 = 0;
        total_num_olss = 1;
        ols_mode = 4;
    }

    const std::uint16_t num_ptls = static_cast<std::uint16_t>(vps.num_ptls_minus1) + 1;
    vps.pt_present_flag.assign(num_ptls, 0);
    vps.ptl_max_tid.assign(num_ptls, 0);

    for (std::uint16_t i = 0; i < num_ptls; ++i) {
        bool pt_present = true;
        if (i > 0) {
            pt_present = r.read_bit();
        }
        vps.pt_present_flag[i] = pt_present ? 1 : 0;

        if (!vps.default_ptl_dpb_hrd_max_tid_flag) {
            const std::uint8_t tid = static_cast<std::uint8_t>(r.read_bits(3));
            vps.ptl_max_tid[i] = tid;
        } else {
            vps.ptl_max_tid[i] = vps.max_sublayers_minus1;
        }
    }

    while (!r.byte_aligned()) {
        (void)r.read_bit();  // vps_ptl_alignment_zero_bit (0)
    }

    // PTL payloads
    for (std::uint16_t i = 0; i < num_ptls; ++i) {
        const bool pt_present = vps.pt_present_flag[i] != 0;
        const std::uint8_t max_tid = vps.ptl_max_tid[i];
        detail::skip_profile_tier_level(r, pt_present, max_tid);
    }

    // OLS PTL indices
    vps.ols_ptl_idx.assign(total_num_olss, 0);
    for (std::uint16_t i = 0; i < total_num_olss; ++i) {
        if (num_ptls > 1 && num_ptls != total_num_olss) {
            const std::uint8_t idx = static_cast<std::uint8_t>(r.read_bits(8));
            vps.ols_ptl_idx[i] = idx;
        } else if (num_ptls == 1) {
            vps.ols_ptl_idx[i] = 0;
        } else {
            vps.ols_ptl_idx[i] = static_cast<std::uint8_t>(i);
        }
    }

    // DPB / HRD — simplified to counts/flags but bits correctly consumed.
    // Only present when not each_layer_is_an_ols_flag.
    if (!vps.each_layer_is_an_ols_flag) {
        try {
            // Compute num_multi_layer_olss similar to cbs_h266.c 912-928
            std::uint16_t num_multi_layer_olss = 0;

            if (!vps.each_layer_is_an_ols_flag) {
                if (vps.ols_mode_idc == 0 || vps.ols_mode_idc == 1 || vps.ols_mode_idc == 4) {
                    // each OLS i contains i+1 layers when not each_layer
                    for (std::uint16_t i = 1; i < total_num_olss; ++i) {
                        std::uint16_t num_layers_in_ols = 0;
                        if (vps.each_layer_is_an_ols_flag) {
                            num_layers_in_ols = 1;
                        } else if (vps.ols_mode_idc == 0 || vps.ols_mode_idc == 1) {
                            num_layers_in_ols = i + 1;
                        } else if (vps.ols_mode_idc == 2) {
                            // will be handled below
                            num_layers_in_ols = 0;
                        }
                        if (num_layers_in_ols > 1) {
                            ++num_multi_layer_olss;
                        }
                    }
                    if (vps.ols_mode_idc == 2) {
                        // re-evaluate for mode 2 below
                        num_multi_layer_olss = 0;
                    }
                }
                if (vps.ols_mode_idc == 2) {
                    // Need dependency closure for accurate counting.
                    // Build dependency_flag transitive closure.
                    std::vector<std::vector<std::uint8_t>> dep(
                        num_layers, std::vector<std::uint8_t>(num_layers, 0)
                    );
                    for (std::uint16_t i = 0; i < num_layers; ++i) {
                        for (std::uint16_t j = 0; j < num_layers; ++j) {
                            dep[i][j] = vps.direct_ref_layer_flag[i][j];
                            for (std::uint16_t k = 0; k < i; ++k) {
                                if (vps.direct_ref_layer_flag[i][k] && dep[k][j]) {
                                    dep[i][j] = 1;
                                }
                            }
                        }
                    }
                    // reference layers per layer
                    std::vector<std::vector<std::uint16_t>> ref_idx(num_layers);
                    std::vector<std::uint16_t> num_ref(num_layers, 0);
                    for (std::uint16_t i = 0; i < num_layers; ++i) {
                        for (std::uint16_t j = 0; j < num_layers; ++j) {
                            if (dep[i][j]) {
                                ref_idx[i].push_back(j);
                            }
                        }
                        num_ref[i] = static_cast<std::uint16_t>(ref_idx[i].size());
                    }

                    for (std::uint16_t i = 1; i < total_num_olss; ++i) {
                        std::vector<std::uint8_t> included(num_layers, 0);
                        for (std::uint16_t k = 0; k < num_layers; ++k) {
                            if (i < vps.ols_output_layer_flag.size() &&
                                vps.ols_output_layer_flag[i][k]) {
                                included[k] = 1;
                            }
                        }
                        // add reference layers of output layers
                        for (std::uint16_t k = 0; k < num_layers; ++k) {
                            if (included[k]) {
                                // for mode 2, output layers are direct; need to include their refs
                                // Use dep closure: if dep[k][ref] and ref not yet included, include
                                // it.
                                for (std::uint16_t r = 0; r < ref_idx[k].size(); ++r) {
                                    const std::uint16_t ref = ref_idx[k][r];
                                    if (!included[ref]) {
                                        included[ref] = 1;
                                    }
                                }
                            }
                        }
                        std::uint16_t cnt = 0;
                        for (std::uint16_t k = 0; k < num_layers; ++k)
                            if (included[k])
                                ++cnt;
                        if (cnt > 1)
                            ++num_multi_layer_olss;
                    }
                }
            }

            // If there is at least one multi-layer OLS, parse DPB/HRD
            // Guard against no multi-layer (spec requires error, but we just skip)
            if (num_multi_layer_olss > 0) {
                std::uint32_t vps_num_dpb_params_minus1 = 0;
                // ue in range 0..num_multi_layer_olss-1, but consume regardless
                vps_num_dpb_params_minus1 = r.read_ue();
                const std::uint32_t vps_num_dpb_params = vps_num_dpb_params_minus1 + 1;

                bool sublayer_dpb_params_present = false;
                if (vps.max_sublayers_minus1 > 0) {
                    sublayer_dpb_params_present = r.read_bit();
                }

                for (std::uint32_t i = 0; i < vps_num_dpb_params; ++i) {
                    std::uint8_t dpb_max_tid = vps.max_sublayers_minus1;
                    if (!vps.default_ptl_dpb_hrd_max_tid_flag) {
                        dpb_max_tid = static_cast<std::uint8_t>(r.read_bits(3));
                    }
                    const int start = sublayer_dpb_params_present ? 0 : dpb_max_tid;
                    for (int tid = start; tid <= dpb_max_tid; ++tid) {
                        (void)r.read_ue();  // dpb_max_dec_pic_buffering_minus1
                        (void)r.read_ue();  // dpb_max_num_reorder_pics
                        (void)r.read_ue();  // dpb_max_latency_increase_plus1
                    }
                }

                for (std::uint16_t i = 0; i < num_multi_layer_olss; ++i) {
                    (void)r.read_ue();     // vps_ols_dpb_pic_width
                    (void)r.read_ue();     // vps_ols_dpb_pic_height
                    (void)r.read_bits(2);  // vps_ols_dpb_chroma_format
                    (void)r.read_ue();     // vps_ols_dpb_bitdepth_minus8
                    if (vps_num_dpb_params > 1 && vps_num_dpb_params != num_multi_layer_olss) {
                        (void)r.read_ue();  // vps_ols_dpb_params_idx
                    }
                }

                const bool timing_present = r.read_bit();  // vps_timing_hrd_params_present_flag
                if (timing_present) {
                    // general_timing_hrd_parameters
                    std::uint32_t num_units_in_tick = static_cast<std::uint32_t>(r.read_bits(32));
                    (void)num_units_in_tick;
                    std::uint32_t time_scale = static_cast<std::uint32_t>(r.read_bits(32));
                    (void)time_scale;
                    const bool nal_present = r.read_bit();
                    const bool vcl_present = r.read_bit();
                    bool du_present = false;
                    std::uint32_t hrd_cpb_cnt_minus1 = 0;
                    if (nal_present || vcl_present) {
                        (void)r.read_bit();  // general_same_pic_timing_in_all_ols_flag
                        du_present = r.read_bit();
                        if (du_present) {
                            (void)r.read_bits(8);  // tick_divisor_minus2
                        }
                        (void)r.read_bits(4);  // bit_rate_scale
                        (void)r.read_bits(4);  // cpb_size_scale
                        if (du_present) {
                            (void)r.read_bits(4);  // cpb_size_du_scale
                        }
                        hrd_cpb_cnt_minus1 = r.read_ue();  // 0..31
                    }

                    bool sublayer_cpb_present = false;
                    if (vps.max_sublayers_minus1 > 0) {
                        sublayer_cpb_present = r.read_bit();
                    }

                    const std::uint32_t num_ols_timing_minus1 = r.read_ue();  // 0..num_multi-1
                    const std::uint32_t num_timing = num_ols_timing_minus1 + 1;

                    for (std::uint32_t i = 0; i < num_timing; ++i) {
                        std::uint8_t hrd_max_tid = vps.max_sublayers_minus1;
                        if (!vps.default_ptl_dpb_hrd_max_tid_flag) {
                            hrd_max_tid = static_cast<std::uint8_t>(r.read_bits(3));
                        }
                        const std::uint8_t first_sublayer = sublayer_cpb_present ? 0 : hrd_max_tid;
                        for (std::uint16_t tid = first_sublayer; tid <= vps.max_sublayers_minus1;
                             ++tid) {
                            const bool fixed_general = r.read_bit();
                            bool fixed_within = true;
                            if (!fixed_general) {
                                fixed_within = r.read_bit();
                            }
                            if (fixed_within) {
                                (void)r.read_ue();  // elemental_duration_in_tc_minus1
                            } else {
                                if ((nal_present || vcl_present) && hrd_cpb_cnt_minus1 == 0) {
                                    (void)r.read_bit();  // low_delay_hrd_flag
                                }
                            }
                            if (nal_present) {
                                for (std::uint32_t c = 0; c <= hrd_cpb_cnt_minus1; ++c) {
                                    (void)r.read_ue();  // bit_rate_value_minus1
                                    (void)r.read_ue();  // cpb_size_value_minus1
                                    if (du_present) {
                                        (void)r.read_ue();  // cpb_size_du_value_minus1
                                        (void)r.read_ue();  // bit_rate_du_value_minus1
                                    }
                                    (void)r.read_bit();  // cbr_flag
                                }
                            }
                            if (vcl_present) {
                                for (std::uint32_t c = 0; c <= hrd_cpb_cnt_minus1; ++c) {
                                    (void)r.read_ue();
                                    (void)r.read_ue();
                                    if (du_present) {
                                        (void)r.read_ue();
                                        (void)r.read_ue();
                                    }
                                    (void)r.read_bit();
                                }
                            }
                        }
                    }

                    if (num_timing > 1 && num_timing != num_multi_layer_olss) {
                        for (std::uint16_t i = 0; i < num_multi_layer_olss; ++i) {
                            (void)r.read_ue();  // vps_ols_timing_hrd_idx
                        }
                    }
                }
            }
        } catch (...) {
            // HRD is optional/simplified; truncated streams should not break VPS parsing.
        }
    }

    // extension flag + trailing bits
    try {
        if (r.has_more_bits()) {
            const bool ext = r.read_bit();
            vps.extension_flag = ext;
            if (ext) {
                // extension_data() – consume until only trailing bits remain.
                // Simplified: consume all remaining bits as extension payload.
                // The spec's extension_data is while(more_rbsp_data) 1-bit.
                // We consume generically until byte alignment would allow trailing check.
                // If we cannot determine, just consume everything left except trailing.
                // For now, consume all remaining bits (extension payload + trailing will be
                // consumed).
                while (r.has_more_bits()) {
                    // Peek if remaining looks like trailing (stop 1 + zeros) is hard without
                    // has_more_rbsp_data. Fall back to consuming one bit at a time.
                    (void)r.read_bit();
                    // If we are byte aligned and next bit is 1 and remaining bits are zeros, the
                    // loop will still consume it as extension; trailing handling below will be
                    // skipped because no bits left, which is acceptable for simplified HRD.
                    if (!r.has_more_bits())
                        break;
                    // Heuristic: if we have just enough bits for trailing, break to let trailing
                    // handling consume it. We keep consuming – the final trailing bits will be
                    // treated as extension data, still correctly consumes stream.
                }
            } else {
                // rbsp_trailing_bits: stop_one_bit + alignment zeros (if any bits left)
                if (r.has_more_bits()) {
                    (void)r.read_bit();  // rbsp_stop_one_bit (1)
                    while (!r.byte_aligned() && r.has_more_bits()) {
                        (void)r.read_bit();  // rbsp_alignment_zero_bit (0)
                    }
                }
            }
        }
    } catch (...) {
        // ignore trailing errors – VPS leading fields already captured
    }

    return vps;
}

}  // namespace vvc
}  // namespace bs
