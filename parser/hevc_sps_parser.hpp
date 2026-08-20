// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "rbsp_bitstream_reader.hpp"
#include "hevc_sps.hpp"
#include "hevc_profile_tier_level_parser.hpp"
#include "hevc_vui_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bs {

/*
 * H.265 sequence_parameter_set_rbsp()
 *
 * 7.3.2.2.1
 *
 * The parser consumes RBSP syntax directly from
 * RbspBitstreamReader.
 *
 * No RBSP/EBSP copy is made.
 */

/*
 * -----------------------------------------------------------
 * Parser result
 * -----------------------------------------------------------
 */

struct SpsParseResult {
    bool ok = false;
    std::size_t bits_consumed = 0;
};

/*
 * -----------------------------------------------------------
 * Limits
 * -----------------------------------------------------------
 */

inline constexpr std::uint8_t kSpsMaxSubLayersMinus1 = 7;

inline constexpr std::uint32_t kSpsMaxSequenceParameterSetId = 15;

/*
 * -----------------------------------------------------------
 * Scaling-list parser
 * -----------------------------------------------------------
 *
 * scaling_list_data()
 *
 * The scaling-list syntax is:
 *
 * for sizeId = 0..3
 *   for matrixId = 0..5
 *     scaling_list_pred_mode_flag
 *
 *     if false:
 *         scaling_list_pred_matrix_id_delta
 *
 *     else:
 *         if sizeId > 1:
 *             scaling_list_dc_coef_minus8
 *
 *         scaling_list_delta_coef[]
 */

/*
 * Parse one scaling-list matrix.
 */
inline void parse_scaling_list_matrix(
    RbspBitstreamReader& bs, ScalingListMatrix& matrix, std::size_t size_id, std::size_t matrix_id
) {
    initialize_scaling_list_matrix(matrix, size_id);

    /*
     * scaling_list_pred_mode_flag
     */
    matrix.pred_mode_flag = bs.read_bit();

    if (!matrix.pred_mode_flag) {
        /*
         * scaling_list_pred_matrix_id_delta
         *
         * ue(v)
         */
        matrix.pred_matrix_id_delta = bs.read_ue();

        /*
         * Validate the prediction source.
         *
         * matrixId - delta must be >= 0.
         */
        if (matrix.pred_matrix_id_delta > matrix_id) {
            throw std::runtime_error("SPS: invalid scaling-list prediction matrix id");
        }

        return;
    }

    /*
     * Explicit scaling-list coefficients.
     */

    std::int32_t next_coef = 8;

    /*
     * sizeId > 1 has a separately coded DC coefficient.
     */
    if (size_id > 1) {
        matrix.dc_coef_minus8 = bs.read_se();

        matrix.dc_coef = scaling_list_dc_coefficient(matrix.dc_coef_minus8);

        next_coef = matrix.dc_coef;
    } else {
        matrix.dc_coef_minus8 = 0;
        matrix.dc_coef = 8;
    }

    const std::size_t coefficient_count = scaling_list_coefficient_count(size_id);

    matrix.coefficient_count = static_cast<std::uint8_t>(coefficient_count);

    /*
     * scaling_list_delta_coef
     *
     * The syntax is represented in scan order.
     *
     * Your ScalingListData intentionally stores coefficients
     * in syntax/scan order, so no raster transformation occurs
     * here.
     */
    for (std::size_t i = 0; i < coefficient_count; ++i) {
        const std::int32_t delta = bs.read_se();

        const auto coefficient = scaling_list_next_coefficient(next_coef, delta);

        matrix.coefficients[i] = coefficient;

        next_coef = static_cast<std::int32_t>(coefficient);
    }
}

/*
 * Parse complete scaling_list_data().
 */
inline void parse_scaling_list_data(RbspBitstreamReader& bs, ScalingListData& data) {
    initialize_scaling_list_data(data);

    for (std::size_t size_id = 0; size_id < kScalingListSizeIds; ++size_id) {
        /*
         * matrixId progression:
         *
         * sizeId 0,1,2:
         *     0,1,2,3,4,5
         *
         * sizeId 3:
         *     0,3
         */
        const std::size_t matrix_count = scaling_list_matrix_count(size_id);

        std::size_t matrix_id = 0;

        for (std::size_t matrix_index = 0; matrix_index < matrix_count; ++matrix_index) {
            auto& matrix = data.matrix(size_id, matrix_id);

            parse_scaling_list_matrix(bs, matrix, size_id, matrix_id);

            matrix_id = next_scaling_list_matrix_id(size_id, matrix_id);
        }
    }
}

/*
 * -----------------------------------------------------------
 * Sub-layer ordering
 * -----------------------------------------------------------
 */

inline void parse_sps_sub_layer_ordering(RbspBitstreamReader& bs, SequenceParameterSet& sps) {
    sps.sps_sub_layer_ordering_info_present_flag = bs.read_bit();

    const std::size_t first_sub_layer =
        sps.sps_sub_layer_ordering_info_present_flag ? 0 : sps.sps_max_sub_layers_minus1;

    for (std::size_t i = first_sub_layer; i <= sps.sps_max_sub_layers_minus1; ++i) {
        auto& info = sps.sub_layer_ordering_info[i];

        /*
         * max_dec_pic_buffering_minus1
         */
        info.max_dec_pic_buffering_minus1 = bs.read_ue();

        /*
         * max_num_reorder_pics
         */
        info.max_num_reorder_pics = bs.read_ue();

        /*
         * max_latency_increase_plus1
         */
        info.max_latency_increase_plus1 = bs.read_ue();
    }

    /*
     * Infer lower temporal layers if only the highest layer
     * was explicitly coded.
     */
    if (!sps.sps_sub_layer_ordering_info_present_flag) {
        const auto highest = sps.sub_layer_ordering_info[sps.sps_max_sub_layers_minus1];

        for (std::size_t i = 0; i < sps.sps_max_sub_layers_minus1; ++i) {
            sps.sub_layer_ordering_info[i] = highest;
        }
    }
}

/*
 * -----------------------------------------------------------
 * Picture dimensions / conformance window
 * -----------------------------------------------------------
 */

inline void parse_sps_dimensions(RbspBitstreamReader& bs, SequenceParameterSet& sps) {
    /*
     * pic_width_in_luma_samples
     */
    sps.pic_width_in_luma_samples = bs.read_ue();

    /*
     * pic_height_in_luma_samples
     */
    sps.pic_height_in_luma_samples = bs.read_ue();

    /*
     * conformance_window_flag
     */
    sps.conformance_window_flag = bs.read_bit();

    if (!sps.conformance_window_flag) {
        sps.conformance_window = {};
        return;
    }

    /*
     * conf_win_left_offset
     */
    sps.conformance_window.left_offset = bs.read_ue();

    /*
     * conf_win_right_offset
     */
    sps.conformance_window.right_offset = bs.read_ue();

    /*
     * conf_win_top_offset
     */
    sps.conformance_window.top_offset = bs.read_ue();

    /*
     * conf_win_bottom_offset
     */
    sps.conformance_window.bottom_offset = bs.read_ue();
}

/*
 * -----------------------------------------------------------
 * Bit depth / POC
 * -----------------------------------------------------------
 */

inline void parse_sps_bit_depth_and_poc(RbspBitstreamReader& bs, SequenceParameterSet& sps) {
    /*
     * bit_depth_luma_minus8
     */
    sps.bit_depth_luma_minus8 = bs.read_ue();

    /*
     * bit_depth_chroma_minus8
     */
    sps.bit_depth_chroma_minus8 = bs.read_ue();

    derive_sps_bit_depth(sps);

    /*
     * log2_max_pic_order_cnt_lsb_minus4
     */
    sps.log2_max_pic_order_cnt_lsb_minus4 = bs.read_ue();

    if (sps.log2_max_pic_order_cnt_lsb_minus4 > 12) {
        throw std::runtime_error("SPS: invalid log2_max_pic_order_cnt_lsb_minus4");
    }
}

/*
 * -----------------------------------------------------------
 * Coding block parameters
 * -----------------------------------------------------------
 */

inline void parse_sps_coding_blocks(RbspBitstreamReader& bs, SequenceParameterSet& sps) {
    auto& cb = sps.coding_blocks;

    /*
     * log2_min_luma_coding_block_size_minus3
     */
    cb.log2_min_luma_coding_block_size_minus3 = bs.read_ue();

    /*
     * log2_diff_max_min_luma_coding_block_size
     */
    cb.log2_diff_max_min_luma_coding_block_size = bs.read_ue();

    /*
     * log2_min_luma_transform_block_size_minus2
     */
    cb.log2_min_luma_transform_block_size_minus2 = bs.read_ue();

    /*
     * log2_diff_max_min_luma_transform_block_size
     */
    cb.log2_diff_max_min_luma_transform_block_size = bs.read_ue();

    /*
     * max_transform_hierarchy_depth_inter
     */
    cb.max_transform_hierarchy_depth_inter = bs.read_ue();

    /*
     * max_transform_hierarchy_depth_intra
     */
    cb.max_transform_hierarchy_depth_intra = bs.read_ue();
}

/*
 * -----------------------------------------------------------
 * Scaling list / coding tools
 * -----------------------------------------------------------
 */

inline void parse_sps_coding_tools(RbspBitstreamReader& bs, SequenceParameterSet& sps) {
    /*
     * scaling_list_enabled_flag
     */
    sps.scaling_list_enabled_flag = bs.read_bit();

    if (sps.scaling_list_enabled_flag) {
        /*
         * sps_scaling_list_data_present_flag
         */
        sps.sps_scaling_list_data_present_flag = bs.read_bit();

        if (sps.sps_scaling_list_data_present_flag) {
            parse_scaling_list_data(bs, sps.scaling_list);
        }
    } else {
        sps.sps_scaling_list_data_present_flag = false;
    }

    /*
     * amp_enabled_flag
     */
    sps.amp_enabled_flag = bs.read_bit();

    /*
     * sample_adaptive_offset_enabled_flag
     */
    sps.sample_adaptive_offset_enabled_flag = bs.read_bit();
}

/*
 * -----------------------------------------------------------
 * PCM
 * -----------------------------------------------------------
 */

inline void parse_sps_pcm(RbspBitstreamReader& bs, SequenceParameterSet& sps) {
    auto& pcm = sps.pcm;

    /*
     * pcm_enabled_flag
     */
    pcm.pcm_enabled_flag = bs.read_bit();

    if (!pcm.pcm_enabled_flag) {
        pcm = {};
        return;
    }

    /*
     * pcm_sample_bit_depth_luma_minus1
     *
     * u(4)
     */
    pcm.pcm_sample_bit_depth_luma_minus1 = bs.read_u8(4);

    /*
     * pcm_sample_bit_depth_chroma_minus1
     *
     * u(4)
     */
    pcm.pcm_sample_bit_depth_chroma_minus1 = bs.read_u8(4);

    /*
     * log2_min_pcm_luma_coding_block_size_minus3
     */
    pcm.log2_min_pcm_luma_coding_block_size_minus3 = bs.read_ue();

    /*
     * log2_diff_max_min_pcm_luma_coding_block_size
     */
    pcm.log2_diff_max_min_pcm_luma_coding_block_size = bs.read_ue();

    /*
     * pcm_loop_filter_disabled_flag
     */
    pcm.pcm_loop_filter_disabled_flag = bs.read_bit();
}

/*
 * -----------------------------------------------------------
 * Short-term RPS
 * -----------------------------------------------------------
 */

/*
 * Derive a semantic RPS from a previously parsed RPS.
 *
 * This is used internally while parsing inter-RPS prediction,
 * because the syntax of the next RPS needs NumDeltaPocs of
 * the reference RPS.
 */
[[nodiscard]]
inline std::vector<DerivedShortTermReference> derive_rps_references_recursive(
    const std::vector<ShortTermRefPicSet>& sets, std::size_t index
) {
    if (index >= sets.size()) {
        return {};
    }

    const auto& rps = sets[index];

    /*
     * Explicit RPS.
     */
    if (!rps.inter_ref_pic_set_prediction_flag) {
        const auto derived = derive_explicit_references(rps);

        return derived.references;
    }

    /*
     * Predicted RPS.
     */
    const std::size_t reference_index = rps.inter_prediction.reference_rps_idx;

    if (reference_index >= sets.size()) {
        return {};
    }

    const auto reference = derive_rps_references_recursive(sets, reference_index);

    std::vector<DerivedShortTermReference> result;

    result.reserve(reference.size() + 1);

    const std::int64_t delta_rps = rps.inter_prediction.delta_rps;

    /*
     * H.265 derives the new RPS by testing each delta POC
     * from the reference RPS plus DeltaRps.
     *
     * The additional j == NumDeltaPocs[RefRpsIdx] entry
     * represents DeltaRps itself.
     */
    for (std::size_t j = 0; j < reference.size(); ++j) {
        const auto& entry = rps.inter_prediction.entries[j];

        const std::int64_t delta_poc =
            static_cast<std::int64_t>(reference[j].delta_poc) + delta_rps;

        const bool use = entry.used_by_curr_pic_flag || entry.use_delta_flag;

        if (!use || delta_poc == 0) {
            continue;
        }

        result.push_back(
            {static_cast<std::int32_t>(delta_poc), entry.used_by_curr_pic_flag, delta_poc < 0}
        );
    }

    /*
     * j == NumDeltaPocs[RefRpsIdx]
     *
     * DeltaPoc = DeltaRps.
     */
    if (!rps.inter_prediction.entries.empty()) {
        const auto& entry = rps.inter_prediction.entries.back();

        if (entry.used_by_curr_pic_flag || entry.use_delta_flag) {
            if (delta_rps != 0) {
                result.push_back(
                    {static_cast<std::int32_t>(delta_rps),
                     entry.used_by_curr_pic_flag,
                     delta_rps < 0}
                );
            }
        }
    }

    return result;
}

/*
 * Return NumDeltaPocs for a parsed RPS.
 */
[[nodiscard]]
inline std::uint32_t sps_rps_num_delta_pocs(
    const std::vector<ShortTermRefPicSet>& sets, std::size_t index
) {
    if (index >= sets.size()) {
        return 0;
    }

    const auto references = derive_rps_references_recursive(sets, index);

    return static_cast<std::uint32_t>(references.size());
}

/*
 * Parse one short-term reference picture set.
 */
inline void parse_short_term_ref_pic_set(
    RbspBitstreamReader& bs, std::vector<ShortTermRefPicSet>& sets, std::size_t st_rps_idx
) {
    ShortTermRefPicSet rps{};

    rps.index = static_cast<std::uint32_t>(st_rps_idx);

    /*
     * inter_ref_pic_set_prediction_flag
     *
     * Present when stRpsIdx != 0.
     */
    if (st_rps_idx != 0) {
        rps.inter_ref_pic_set_prediction_flag = bs.read_bit();
    }

    if (!rps.inter_ref_pic_set_prediction_flag) {
        /*
         * Explicit RPS.
         */

        rps.num_negative_pics = bs.read_ue();

        rps.num_positive_pics = bs.read_ue();

        rps.negative_pics.resize(rps.num_negative_pics);

        rps.positive_pics.resize(rps.num_positive_pics);

        /*
         * delta_poc_s0_minus1[]
         */
        for (std::size_t i = 0; i < rps.negative_pics.size(); ++i) {
            rps.negative_pics[i].delta_poc_minus1 = bs.read_ue();

            rps.negative_pics[i].used_by_curr_pic = bs.read_bit();
        }

        /*
         * delta_poc_s1_minus1[]
         */
        for (std::size_t i = 0; i < rps.positive_pics.size(); ++i) {
            rps.positive_pics[i].delta_poc_minus1 = bs.read_ue();

            rps.positive_pics[i].used_by_curr_pic = bs.read_bit();
        }

        derive_explicit_rps(rps);

        sets[st_rps_idx] = std::move(rps);

        return;
    }

    /*
     * -------------------------------------------------------
     * Inter-RPS prediction
     * -------------------------------------------------------
     */

    auto& prediction = rps.inter_prediction;

    /*
     * In SPS syntax stRpsIdx is always less than
     * num_short_term_ref_pic_sets, so delta_idx_minus1 is
     * not present here.
     *
     * It is present only for slice-header additional RPS
     * syntax where stRpsIdx == num_short_term_ref_pic_sets.
     */
    prediction.delta_idx_present = false;

    prediction.delta_idx_minus1 = 0;

    /*
     * For SPS:
     *
     * RefRpsIdx =
     *     stRpsIdx - (delta_idx_minus1 + 1)
     *
     * therefore:
     *
     * RefRpsIdx = stRpsIdx - 1
     *
     * when delta_idx_minus1 is absent.
     */
    if (st_rps_idx == 0) {
        throw std::runtime_error("SPS: invalid inter-RPS prediction index");
    }

    prediction.reference_rps_idx = static_cast<std::uint32_t>(st_rps_idx - 1);

    /*
     * delta_rps_sign
     */
    prediction.delta_rps_sign = bs.read_bit();

    /*
     * abs_delta_rps_minus1
     */
    prediction.abs_delta_rps_minus1 = bs.read_ue();

    prediction.delta_rps =
        calculate_delta_rps(prediction.delta_rps_sign, prediction.abs_delta_rps_minus1);

    /*
     * Number of entries is:
     *
     * NumDeltaPocs[RefRpsIdx] + 1
     */
    const std::uint32_t reference_num_delta_pocs =
        sps_rps_num_delta_pocs(sets, prediction.reference_rps_idx);

    initialize_inter_rps_prediction(prediction, reference_num_delta_pocs);

    for (std::size_t j = 0; j < prediction.entries.size(); ++j) {
        auto& entry = prediction.entries[j];

        /*
         * used_by_curr_pic_flag
         */
        entry.used_by_curr_pic_flag = bs.read_bit();

        /*
         * use_delta_flag is present only when
         * used_by_curr_pic_flag == 0.
         */
        if (!entry.used_by_curr_pic_flag) {
            entry.use_delta_flag = bs.read_bit();

        } else {
            entry.use_delta_flag = true;
        }
    }

    /*
     * Derive NumDeltaPocs so the following RPS can use it.
     */
    sets[st_rps_idx] = std::move(rps);

    sets[st_rps_idx].num_delta_pocs = sps_rps_num_delta_pocs(sets, st_rps_idx);

    /*
     * num_negative_pics / num_positive_pics are semantic
     * values for predicted RPS and can also be populated.
     */
    const auto derived = derive_rps_references_recursive(sets, st_rps_idx);

    std::uint32_t negative = 0;
    std::uint32_t positive = 0;

    for (const auto& ref : derived) {
        if (ref.negative) {
            ++negative;
        } else {
            ++positive;
        }
    }

    sets[st_rps_idx].num_negative_pics = negative;

    sets[st_rps_idx].num_positive_pics = positive;
}

/*
 * Parse all SPS short-term RPS entries.
 */
inline void parse_sps_short_term_rps(RbspBitstreamReader& bs, SequenceParameterSet& sps) {
    auto& rps = sps.reference_picture_sets;

    /*
     * num_short_term_ref_pic_sets
     */
    rps.num_short_term_ref_pic_sets = bs.read_ue();

    initialize_sps_rps(sps);

    for (std::size_t i = 0; i < rps.short_term_ref_pic_sets.size(); ++i) {
        parse_short_term_ref_pic_set(bs, rps.short_term_ref_pic_sets, i);
    }
}

/*
 * -----------------------------------------------------------
 * Long-term reference pictures
 * -----------------------------------------------------------
 */

inline void parse_sps_long_term_rps(RbspBitstreamReader& bs, SequenceParameterSet& sps) {
    auto& rps = sps.reference_picture_sets;

    /*
     * long_term_ref_pics_present_flag
     */
    rps.long_term_ref_pics_present_flag = bs.read_bit();

    if (!rps.long_term_ref_pics_present_flag) {
        rps.num_long_term_ref_pics_sps = 0;

        rps.lt_ref_pic_poc_lsb_sps.clear();

        rps.used_by_curr_pic_lt_sps_flag.clear();

        return;
    }

    /*
     * num_long_term_ref_pics_sps
     */
    rps.num_long_term_ref_pics_sps = bs.read_ue();

    initialize_sps_long_term_rps(sps);

    const unsigned poc_lsb_bits = static_cast<unsigned>(sps.log2_max_pic_order_cnt_lsb_minus4) + 4;

    for (std::size_t i = 0; i < rps.num_long_term_ref_pics_sps; ++i) {
        /*
         * lt_ref_pic_poc_lsb_sps[i]
         */
        rps.lt_ref_pic_poc_lsb_sps[i] = static_cast<std::uint32_t>(bs.read_bits(poc_lsb_bits));

        /*
         * used_by_curr_pic_lt_sps_flag[i]
         */
        rps.used_by_curr_pic_lt_sps_flag[i] = bs.read_bit();
    }
}

/*
 * -----------------------------------------------------------
 * VUI
 * -----------------------------------------------------------
 */

inline void parse_sps_vui(RbspBitstreamReader& bs, SequenceParameterSet& sps) {
    /*
     * vui_parameters_present_flag
     */
    sps.vui_parameters_present_flag = bs.read_bit();

    if (!sps.vui_parameters_present_flag) {
        sps.vui = {};
        return;
    }

    parse_vui_parameters(bs, sps.sps_max_sub_layers_minus1, sps.vui);
}

/*
 * -----------------------------------------------------------
 * SPS extension
 * -----------------------------------------------------------
 *
 * The extension payloads follow the H.265 2019-11 syntax
 * structure:
 *
 *     sps_range_extension_flag        u(1)
 *     sps_multilayer_extension_flag   u(1)
 *     sps_3d_extension_flag           u(1)
 *     sps_scc_extension_flag          u(1)
 *     sps_extension_4bits             u(4)
 *     if( sps_range_extension_flag )
 *         sps_range_extension()
 *     if( sps_multilayer_extension_flag )
 *         sps_multilayer_extension()
 *     if( sps_3d_extension_flag )
 *         sps_3d_extension()
 *     if( sps_scc_extension_flag )
 *         sps_scc_extension()
 *     while( more_rbsp_data() )
 *         sps_extension_data_flag     u(1)
 */

/*
 * 7.3.2.2.4 sps_range_extension()
 *
 * Nine single-bit tool-enablement flags.
 */
inline void parse_sps_range_extension(RbspBitstreamReader& bs, SequenceParameterSet& sps) {
    auto& extension = sps.range_extension;

    extension.transform_skip_rotation_enabled_flag = bs.read_bit();

    extension.transform_skip_context_enabled_flag = bs.read_bit();

    extension.implicit_rdpcm_enabled_flag = bs.read_bit();

    extension.explicit_rdpcm_enabled_flag = bs.read_bit();

    extension.extended_precision_processing_flag = bs.read_bit();

    extension.intra_smoothing_disabled_flag = bs.read_bit();

    extension.high_precision_offsets_enabled_flag = bs.read_bit();

    extension.persistent_rice_adaptation_enabled_flag = bs.read_bit();

    extension.cabac_bypass_alignment_enabled_flag = bs.read_bit();
}

/*
 * 7.3.2.2.5 sps_multilayer_extension()
 */
inline void parse_sps_multilayer_extension(RbspBitstreamReader& bs, SequenceParameterSet& sps) {
    sps.multilayer_extension.inter_view_mv_vert_constraint_flag = bs.read_bit();
}

/*
 * Annex I sps_3d_extension()
 *
 * Two entries; the second carries the texture-coding flags.
 */
inline void parse_sps_3d_extension(RbspBitstreamReader& bs, SequenceParameterSet& sps) {
    for (std::size_t i = 0; i < sps.three_d_extension.views.size(); ++i) {
        auto& view = sps.three_d_extension.views[i];

        view.iv_di_mc_enabled_flag = bs.read_bit();

        view.iv_mv_scal_enabled_flag = bs.read_bit();

        if (i == 0) {
            view.log2_ivmc_sub_pb_size_minus3 = bs.read_ue();

            view.iv_res_pred_enabled_flag = bs.read_bit();

            view.depth_ref_enabled_flag = bs.read_bit();

            view.vsp_mc_enabled_flag = bs.read_bit();

            view.dbbp_enabled_flag = bs.read_bit();

        } else {
            view.tex_mc_enabled_flag = bs.read_bit();

            view.log2_ivmc_sub_pb_size_minus3 = bs.read_ue();

            view.intra_contour_enabled_flag = bs.read_bit();

            view.intra_dc_only_wedge_enabled_flag = bs.read_bit();

            view.cqt_cu_part_pred_enabled_flag = bs.read_bit();

            view.inter_dc_only_enabled_flag = bs.read_bit();

            view.skip_intra_enabled_flag = bs.read_bit();
        }
    }
}

/*
 * 7.3.2.2.6 sps_scc_extension()
 */
inline void parse_sps_scc_extension(RbspBitstreamReader& bs, SequenceParameterSet& sps) {
    auto& extension = sps.scc_extension;

    extension.sps_curr_pic_ref_enabled_flag = bs.read_bit();

    extension.palette_mode_enabled_flag = bs.read_bit();

    if (extension.palette_mode_enabled_flag) {
        extension.palette_max_size = bs.read_ue();

        extension.delta_palette_max_predictor_size = bs.read_ue();

        extension.sps_palette_predictor_initializers_present_flag = bs.read_bit();

        if (extension.sps_palette_predictor_initializers_present_flag) {
            extension.sps_num_palette_predictor_initializers_minus1 = bs.read_ue();

            if (extension.sps_num_palette_predictor_initializers_minus1 >=
                kMaxPalettePredictorSize) {
                throw std::runtime_error("SPS: too many palette predictor initializers");
            }

            const std::size_t num_comps = sps.chroma_format == ChromaFormat::Monochrome ? 1u : 3u;

            const unsigned luma_bits = static_cast<unsigned>(sps.luma_bit_depth());

            const unsigned chroma_bits = static_cast<unsigned>(sps.chroma_bit_depth());

            for (std::size_t comp = 0; comp < num_comps; ++comp) {
                const unsigned width = comp == 0 ? luma_bits : chroma_bits;

                for (std::size_t i = 0;
                     i <= extension.sps_num_palette_predictor_initializers_minus1;
                     ++i) {
                    extension.sps_palette_predictor_initializer[comp][i] =
                        static_cast<std::uint32_t>(bs.read_bits(width));
                }
            }

        } else {
            extension.sps_num_palette_predictor_initializers_minus1 = 0;
        }

    } else {
        extension.palette_max_size = 0;
        extension.delta_palette_max_predictor_size = 0;
        extension.sps_palette_predictor_initializers_present_flag = false;
        extension.sps_num_palette_predictor_initializers_minus1 = 0;
    }

    /*
     * These two fields follow unconditionally.
     */
    extension.motion_vector_resolution_control_idc = static_cast<std::uint32_t>(bs.read_u8(2));

    extension.intra_boundary_filtering_disabled_flag = bs.read_bit();
}

inline void parse_sps_extension(RbspBitstreamReader& bs, SequenceParameterSet& sps) {
    auto& extension = sps.extension;

    /*
     * sps_extension_present_flag
     */
    extension.sps_extension_present_flag = bs.read_bit();

    if (!extension.sps_extension_present_flag) {
        return;
    }

    /*
     * The four base extension flags.
     */
    extension.range_extension_flag = bs.read_bit();

    extension.multilayer_extension_flag = bs.read_bit();

    extension.extension_3d_flag = bs.read_bit();

    extension.scc_extension_flag = bs.read_bit();

    /*
     * Reserved sps_extension_4bits.
     */
    for (std::size_t i = 0; i < extension.reserved_extension_flags.size(); ++i) {
        extension.reserved_extension_flags[i] = bs.read_bit();
    }

    /*
     * Extension payloads appear in flag order.
     */
    if (extension.range_extension_flag) {
        parse_sps_range_extension(bs, sps);
    }

    if (extension.multilayer_extension_flag) {
        parse_sps_multilayer_extension(bs, sps);
    }

    if (extension.extension_3d_flag) {
        parse_sps_3d_extension(bs, sps);
    }

    if (extension.scc_extension_flag) {
        parse_sps_scc_extension(bs, sps);
    }

    /*
     * Remaining extension_data_flag bits.
     *
     * They continue until rbsp_trailing_bits().
     */
    extension.extension_data_present = false;

    while (bs.more_rbsp_data()) {
        if (bs.read_bit()) {
            extension.extension_data_present = true;
        }
    }
}

/*
 * -----------------------------------------------------------
 * Main SPS parser
 * -----------------------------------------------------------
 */

inline SpsParseResult parse_sequence_parameter_set(
    RbspBitstreamReader& bs, SequenceParameterSet& sps
) {
    const std::size_t start = bs.bit_position();

    initialize_sps(sps);

    /*
     * =======================================================
     * SPS identification
     * =======================================================
     */

    /*
     * sps_video_parameter_set_id
     *
     * u(4)
     */
    sps.sps_video_parameter_set_id = bs.read_u8(4);

    /*
     * sps_max_sub_layers_minus1
     *
     * u(3)
     */
    sps.sps_max_sub_layers_minus1 = bs.read_u8(3);

    if (sps.sps_max_sub_layers_minus1 > kSpsMaxSubLayersMinus1) {
        throw std::runtime_error("SPS: invalid sps_max_sub_layers_minus1");
    }

    /*
     * sps_temporal_id_nesting_flag
     */
    sps.sps_temporal_id_nesting_flag = bs.read_bit();

    /*
     * profile_tier_level()
     *
     * profilePresentFlag = 1
     */
    parse_profile_tier_level(bs, true, sps.sps_max_sub_layers_minus1, sps.profile_tier_level);

    /*
     * sps_seq_parameter_set_id
     */
    sps.sps_seq_parameter_set_id = bs.read_ue();

    if (sps.sps_seq_parameter_set_id > kSpsMaxSequenceParameterSetId) {
        throw std::runtime_error("SPS: invalid sps_seq_parameter_set_id");
    }

    /*
     * =======================================================
     * Chroma format
     * =======================================================
     */

    const std::uint32_t chroma_format_idc = bs.read_ue();

    if (!is_valid_chroma_format(chroma_format_idc)) {
        throw std::runtime_error("SPS: invalid chroma_format_idc");
    }

    sps.chroma_format = chroma_format_from_idc(chroma_format_idc);

    /*
     * separate_colour_plane_flag
     */
    if (sps.chroma_format == ChromaFormat::YUV444) {
        sps.separate_colour_plane_flag = bs.read_bit();
    }

    /*
     * =======================================================
     * Dimensions
     * =======================================================
     */

    parse_sps_dimensions(bs, sps);

    if (sps.pic_width_in_luma_samples == 0 || sps.pic_height_in_luma_samples == 0) {
        throw std::runtime_error("SPS: invalid picture dimensions");
    }

    /*
     * =======================================================
     * Bit depth / POC
     * =======================================================
     */

    parse_sps_bit_depth_and_poc(bs, sps);

    /*
     * =======================================================
     * Sub-layer ordering
     * =======================================================
     */

    parse_sps_sub_layer_ordering(bs, sps);

    /*
     * =======================================================
     * Coding block configuration
     * =======================================================
     */

    parse_sps_coding_blocks(bs, sps);

    /*
     * =======================================================
     * Coding tools
     * =======================================================
     */

    parse_sps_coding_tools(bs, sps);

    /*
     * =======================================================
     * PCM
     * =======================================================
     */

    parse_sps_pcm(bs, sps);

    /*
     * =======================================================
     * Short-term RPS
     * =======================================================
     */

    parse_sps_short_term_rps(bs, sps);

    /*
     * =======================================================
     * Long-term reference pictures
     * =======================================================
     */

    parse_sps_long_term_rps(bs, sps);

    /*
     * =======================================================
     * Temporal MVP
     * =======================================================
     */

    sps.sps_temporal_mvp_enabled_flag = bs.read_bit();

    /*
     * =======================================================
     * Strong intra smoothing
     * =======================================================
     */

    sps.strong_intra_smoothing_enabled_flag = bs.read_bit();

    /*
     * =======================================================
     * VUI
     * =======================================================
     */

    parse_sps_vui(bs, sps);

    /*
     * =======================================================
     * SPS extension
     * =======================================================
     */

    parse_sps_extension(bs, sps);

    /*
     * =======================================================
     * Derived information
     * =======================================================
     */

    derive_sps_bit_depth(sps);

    derive_sps_geometry(sps);

    return {true, bs.bit_position() - start};
}

/*
 * -----------------------------------------------------------
 * Convenience overload
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline SequenceParameterSet parse_sequence_parameter_set(RbspBitstreamReader& bs) {
    SequenceParameterSet sps{};

    parse_sequence_parameter_set(bs, sps);

    return sps;
}

/*
 * -----------------------------------------------------------
 * Validation
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline bool validate_sequence_parameter_set(const SequenceParameterSet& sps) noexcept {
    if (!validate_sps_base(sps)) {
        return false;
    }

    if (!validate_sps_chroma(sps)) {
        return false;
    }

    if (!validate_sps_rps(sps)) {
        return false;
    }

    if (sps.vui_parameters_present_flag) {
        if (!validate_vui_parameters(sps.vui)) {
            return false;
        }
    }

    return true;
}

}  // namespace bs