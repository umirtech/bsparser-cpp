// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "rbsp_bitstream_reader.hpp"
#include "hevc_pps.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace bs {

/*
 * H.265:
 *
 *     7.3.2.3.1 General picture parameter set RBSP syntax
 *
 * The parser operates directly on the RBSP bitstream.
 *
 * No RBSP payload copy is required.
 */

/*
 * -----------------------------------------------------------
 * Parse result
 * -----------------------------------------------------------
 */

struct PpsParseResult {
    bool ok = false;
    std::size_t bits_consumed = 0;
};

/*
 * -----------------------------------------------------------
 * Limits
 * -----------------------------------------------------------
 */

inline constexpr std::uint32_t kMaxPpsId = 63;

inline constexpr std::uint32_t kMaxPpsSpsId = 15;

/*
 * -----------------------------------------------------------
 * Scaling-list parser
 * -----------------------------------------------------------
 *
 * The SPS parser already contains the scaling-list syntax
 * implementation. Keep PPS self-contained rather than
 * introducing a dependency from PPS -> SPS parser.
 */

/*
 * Parse one scaling-list matrix.
 */
inline void parse_pps_scaling_list_matrix(
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
         */
        matrix.pred_matrix_id_delta = bs.read_ue();

        if (matrix.pred_matrix_id_delta > matrix_id) {
            throw std::runtime_error("PPS: invalid scaling-list prediction matrix id");
        }

        return;
    }

    /*
     * Explicit matrix.
     */
    std::int32_t next_coef = 8;

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
inline void parse_pps_scaling_list_data(RbspBitstreamReader& bs, ScalingListData& data) {
    initialize_scaling_list_data(data);

    for (std::size_t size_id = 0; size_id < kScalingListSizeIds; ++size_id) {
        const std::size_t matrix_count = scaling_list_matrix_count(size_id);

        std::size_t matrix_id = 0;

        for (std::size_t matrix_index = 0; matrix_index < matrix_count; ++matrix_index) {
            auto& matrix = data.matrix(size_id, matrix_id);

            parse_pps_scaling_list_matrix(bs, matrix, size_id, matrix_id);

            matrix_id = next_scaling_list_matrix_id(size_id, matrix_id);
        }
    }
}

/*
 * -----------------------------------------------------------
 * Tile configuration
 * -----------------------------------------------------------
 */

inline void parse_pps_tiles(RbspBitstreamReader& bs, PictureParameterSet& pps) {
    auto& tiles = pps.tiles;

    /*
     * tiles_enabled_flag
     */
    tiles.tiles_enabled_flag = bs.read_bit();

    /*
     * entropy_coding_sync_enabled_flag
     *
     * This is signaled independently of tiles.
     */
    pps.entropy_coding_sync_enabled_flag = bs.read_bit();

    if (!tiles.tiles_enabled_flag) {
        tiles.num_tile_columns_minus1 = 0;
        tiles.num_tile_rows_minus1 = 0;
        tiles.uniform_spacing_flag = false;

        tiles.column_width_minus1.clear();
        tiles.row_height_minus1.clear();

        tiles.loop_filter_across_tiles_enabled_flag = false;

        return;
    }

    /*
     * num_tile_columns_minus1
     */
    tiles.num_tile_columns_minus1 = bs.read_ue();

    /*
     * num_tile_rows_minus1
     */
    tiles.num_tile_rows_minus1 = bs.read_ue();

    /*
     * uniform_spacing_flag
     */
    tiles.uniform_spacing_flag = bs.read_bit();

    initialize_pps_tiles(tiles);

    if (!tiles.uniform_spacing_flag) {
        /*
         * Explicit column widths.
         *
         * The final column is inferred.
         */
        for (std::size_t i = 0; i < tiles.column_width_minus1.size(); ++i) {
            tiles.column_width_minus1[i] = bs.read_ue();
        }

        /*
         * Explicit row heights.
         *
         * The final row is inferred.
         */
        for (std::size_t i = 0; i < tiles.row_height_minus1.size(); ++i) {
            tiles.row_height_minus1[i] = bs.read_ue();
        }
    }

    /*
     * loop_filter_across_tiles_enabled_flag
     */
    tiles.loop_filter_across_tiles_enabled_flag = bs.read_bit();
}

/*
 * -----------------------------------------------------------
 * Deblocking filter
 * -----------------------------------------------------------
 */

inline void parse_pps_deblocking(RbspBitstreamReader& bs, PictureParameterSet& pps) {
    auto& deblocking = pps.deblocking;

    /*
     * deblocking_filter_control_present_flag
     */
    deblocking.deblocking_filter_control_present_flag = bs.read_bit();

    if (!deblocking.deblocking_filter_control_present_flag) {
        deblocking.deblocking_filter_override_enabled_flag = false;

        deblocking.pps_deblocking_filter_disabled_flag = false;

        deblocking.pps_beta_offset_div2 = 0;
        deblocking.pps_tc_offset_div2 = 0;

        return;
    }

    /*
     * deblocking_filter_override_enabled_flag
     */
    deblocking.deblocking_filter_override_enabled_flag = bs.read_bit();

    /*
     * pps_deblocking_filter_disabled_flag
     */
    deblocking.pps_deblocking_filter_disabled_flag = bs.read_bit();

    if (!deblocking.pps_deblocking_filter_disabled_flag) {
        /*
         * pps_beta_offset_div2
         */
        deblocking.pps_beta_offset_div2 = bs.read_se();

        /*
         * pps_tc_offset_div2
         */
        deblocking.pps_tc_offset_div2 = bs.read_se();

    } else {
        deblocking.pps_beta_offset_div2 = 0;
        deblocking.pps_tc_offset_div2 = 0;
    }
}

/*
 * -----------------------------------------------------------
 * PPS range extension
 * -----------------------------------------------------------
 */

inline void parse_pps_range_extension(RbspBitstreamReader& bs, PictureParameterSet& pps) {
    auto& extension = pps.range_extension;

    /*
     * log2_max_transform_skip_block_size_minus2
     */
    extension.log2_max_transform_skip_block_size_minus2 = bs.read_ue();

    /*
     * cross_component_prediction_enabled_flag
     */
    extension.cross_component_prediction_enabled_flag = bs.read_bit();

    /*
     * chroma_qp_offset_list_enabled_flag
     */
    extension.chroma_qp_offset_list_enabled_flag = bs.read_bit();

    if (extension.chroma_qp_offset_list_enabled_flag) {
        /*
         * diff_cu_chroma_qp_offset_depth
         */
        extension.diff_cu_chroma_qp_offset_depth = bs.read_ue();

        /*
         * chroma_qp_offset_list_len_minus1
         */
        extension.chroma_qp_offset_list_len_minus1 = bs.read_ue();

        initialize_pps_range_extension(extension);

        for (std::size_t i = 0; i < extension.cb_qp_offset_list.size(); ++i) {
            extension.cb_qp_offset_list[i] = bs.read_se();

            extension.cr_qp_offset_list[i] = bs.read_se();
        }

    } else {
        extension.diff_cu_chroma_qp_offset_depth = 0;
        extension.chroma_qp_offset_list_len_minus1 = 0;

        extension.cb_qp_offset_list.clear();
        extension.cr_qp_offset_list.clear();
    }

    /*
     * log2_sao_offset_scale_luma
     */
    extension.log2_sao_offset_scale_luma = bs.read_ue();

    /*
     * log2_sao_offset_scale_chroma
     */
    extension.log2_sao_offset_scale_chroma = bs.read_ue();
}

/*
 * -----------------------------------------------------------
 * SCC extension
 * -----------------------------------------------------------
 *
 * 7.3.2.3.4
 */

inline void parse_pps_scc_extension(RbspBitstreamReader& bs, PictureParameterSet& pps) {
    auto& extension = pps.scc_extension;

    /*
     * pps_curr_pic_ref_enabled_flag
     */
    extension.pps_curr_pic_ref_enabled_flag = bs.read_bit();

    /*
     * residual_adaptive_colour_transform_enabled_flag
     */
    extension.residual_adaptive_colour_transform_enabled_flag = bs.read_bit();

    if (extension.residual_adaptive_colour_transform_enabled_flag) {
        /*
         * pps_slice_act_qp_offsets_present_flag
         */
        extension.pps_slice_act_qp_offsets_present_flag = bs.read_bit();

        /*
         * pps_act_y_qp_offset_plus5
         * pps_act_cb_qp_offset_plus5
         * pps_act_cr_qp_offset_plus3
         */
        extension.pps_act_y_qp_offset_plus5 = bs.read_se();

        extension.pps_act_cb_qp_offset_plus5 = bs.read_se();

        extension.pps_act_cr_qp_offset_plus3 = bs.read_se();

    } else {
        extension.pps_slice_act_qp_offsets_present_flag = false;

        extension.pps_act_y_qp_offset_plus5 = 0;
        extension.pps_act_cb_qp_offset_plus5 = 0;
        extension.pps_act_cr_qp_offset_plus3 = 0;
    }

    /*
     * Palette predictor initializers.
     */
    extension.pps_palette_predictor_initializers_present_flag = bs.read_bit();

    if (!extension.pps_palette_predictor_initializers_present_flag) {
        extension.pps_num_palette_predictor_initializers = 0;
        extension.monochrome_palette_flag = false;
        extension.luma_bit_depth_entry_minus8 = 0;
        extension.chroma_bit_depth_entry_minus8 = 0;

        return;
    }

    extension.pps_num_palette_predictor_initializers = bs.read_ue();

    if (extension.pps_num_palette_predictor_initializers == 0) {
        extension.monochrome_palette_flag = false;
        extension.luma_bit_depth_entry_minus8 = 0;
        extension.chroma_bit_depth_entry_minus8 = 0;

        return;
    }

    if (extension.pps_num_palette_predictor_initializers > kMaxPalettePredictorSize) {
        throw std::runtime_error("PPS: too many palette predictor initializers");
    }

    extension.monochrome_palette_flag = bs.read_bit();

    extension.luma_bit_depth_entry_minus8 = bs.read_ue();

    if (!extension.monochrome_palette_flag) {
        extension.chroma_bit_depth_entry_minus8 = bs.read_ue();

    } else {
        extension.chroma_bit_depth_entry_minus8 = 0;
    }

    const std::size_t num_comps = extension.monochrome_palette_flag ? 1u : 3u;

    const unsigned luma_bits = static_cast<unsigned>(extension.luma_bit_depth_entry_minus8) + 8u;

    const unsigned chroma_bits =
        static_cast<unsigned>(extension.chroma_bit_depth_entry_minus8) + 8u;

    for (std::size_t comp = 0; comp < num_comps; ++comp) {
        const unsigned width = comp == 0 ? luma_bits : chroma_bits;

        for (std::size_t i = 0; i < extension.pps_num_palette_predictor_initializers; ++i) {
            extension.pps_palette_predictor_initializer[comp][i] =
                static_cast<std::uint32_t>(bs.read_bits(width));
        }
    }
}

/*
 * -----------------------------------------------------------
 * Multilayer extension
 * -----------------------------------------------------------
 *
 * 7.3.2.3.5
 */

/*
 * floor(log2(value)) with floor_log2(0) == 0.
 */
[[nodiscard]]
inline unsigned pps_floor_log2_u32(std::uint32_t value) noexcept {
    if (value == 0) {
        return 0;
    }

    unsigned result = 0;

    while (value >>= 1u) {
        ++result;
    }

    return result;
}

/*
 * colour_mapping_octants()
 *
 * Recursive octree of the colour-mapping table.
 *
 * Octants are stored in pre-order.
 */
inline void parse_pps_colour_mapping_octants(
    RbspBitstreamReader& bs, PpsMultilayerExtension& ext, unsigned inp_depth
) {
    PpsColourMappingOctant octant{};

    octant.split_octant_flag = inp_depth < ext.cm_octant_depth ? bs.read_bit() : false;

    if (octant.split_octant_flag) {
        ext.colour_mapping_octants.push_back(std::move(octant));

        for (unsigned k = 0; k < 2; ++k) {
            for (unsigned m = 0; m < 2; ++m) {
                for (unsigned n = 0; n < 2; ++n) {
                    parse_pps_colour_mapping_octants(bs, ext, inp_depth + 1);
                }
            }
        }

        return;
    }

    const std::size_t part_num_y = std::size_t{1} << ext.cm_y_part_num_log2;

    octant.partition_coded_res_flags.resize(part_num_y * 4);

    const int bit_depth_cm_input_y = 8 + static_cast<int>(ext.luma_bit_depth_cm_input_minus8);

    const int bit_depth_cm_output_y = 8 + static_cast<int>(ext.luma_bit_depth_cm_output_minus8);

    for (std::size_t i = 0; i < part_num_y; ++i) {
        for (unsigned j = 0; j < 4; ++j) {
            const bool coded_res_flag = bs.read_bit();

            octant.partition_coded_res_flags[i * 4 + j] = coded_res_flag;

            if (!coded_res_flag) {
                continue;
            }

            for (unsigned c = 0; c < 3; ++c) {
                const std::uint32_t res_coeff_q = bs.read_ue();

                int cm_res_bits = 10 + bit_depth_cm_input_y - bit_depth_cm_output_y -
                                  static_cast<int>(ext.cm_res_quant_bits) -
                                  (static_cast<int>(ext.cm_delta_flc_bits_minus1) + 1);

                if (cm_res_bits < 0) {
                    cm_res_bits = 0;
                }

                const std::uint32_t res_coeff_r =
                    cm_res_bits > 0 ? static_cast<std::uint32_t>(
                                          bs.read_bits(static_cast<unsigned>(cm_res_bits))
                                      )
                                    : 0u;

                if (res_coeff_q != 0 || res_coeff_r != 0) {
                    /*
                     * res_coeff_s
                     */
                    (void)bs.read_bit();
                }
            }
        }
    }

    ext.colour_mapping_octants.push_back(std::move(octant));
}

/*
 * colour_mapping_table()
 */
inline void parse_pps_colour_mapping_table(RbspBitstreamReader& bs, PpsMultilayerExtension& ext) {
    /*
     * num_cm_ref_layers_minus1
     */
    ext.num_cm_ref_layers_minus1 = bs.read_ue();

    if (ext.num_cm_ref_layers_minus1 >= 63) {
        throw std::runtime_error("PPS: too many colour-mapping reference layers");
    }

    ext.cm_ref_layer_id.resize(ext.num_cm_ref_layers_minus1 + 1);

    for (std::size_t i = 0; i < ext.cm_ref_layer_id.size(); ++i) {
        ext.cm_ref_layer_id[i] = bs.read_u8(6);
    }

    /*
     * cm_octant_depth
     */
    ext.cm_octant_depth = bs.read_u8(2);

    /*
     * cm_y_part_num_log2
     */
    ext.cm_y_part_num_log2 = bs.read_u8(2);

    ext.luma_bit_depth_cm_input_minus8 = bs.read_ue();

    ext.chroma_bit_depth_cm_input_minus8 = bs.read_ue();

    ext.luma_bit_depth_cm_output_minus8 = bs.read_ue();

    ext.chroma_bit_depth_cm_output_minus8 = bs.read_ue();

    /*
     * cm_res_quant_bits
     */
    ext.cm_res_quant_bits = bs.read_u8(2);

    /*
     * cm_delta_flc_bits_minus1
     */
    ext.cm_delta_flc_bits_minus1 = bs.read_u8(2);

    if (ext.cm_octant_depth == 1) {
        ext.cm_adapt_threshold_u_delta = bs.read_se();

        ext.cm_adapt_threshold_v_delta = bs.read_se();

    } else {
        ext.cm_adapt_threshold_u_delta = 0;
        ext.cm_adapt_threshold_v_delta = 0;
    }

    ext.colour_mapping_octants.clear();

    parse_pps_colour_mapping_octants(bs, ext, 0);
}

inline void parse_pps_multilayer_extension(RbspBitstreamReader& bs, PictureParameterSet& pps) {
    auto& ext = pps.multilayer_extension;

    /*
     * poc_reset_info_present_flag
     */
    ext.poc_reset_info_present_flag = bs.read_bit();

    /*
     * pps_infer_scaling_list_flag
     */
    ext.pps_infer_scaling_list_flag = bs.read_bit();

    if (ext.pps_infer_scaling_list_flag) {
        ext.pps_scaling_list_ref_layer_id = bs.read_u8(6);

    } else {
        ext.pps_scaling_list_ref_layer_id = 0;
    }

    /*
     * num_ref_loc_offsets
     */
    const std::uint32_t num_ref_loc_offsets = bs.read_ue();

    if (num_ref_loc_offsets >= 64) {
        throw std::runtime_error("PPS: too many reference-location offsets");
    }

    ext.ref_location_offsets.clear();

    ext.ref_location_offsets.resize(num_ref_loc_offsets);

    for (auto& entry : ext.ref_location_offsets) {
        entry.ref_loc_offset_layer_id = bs.read_u8(6);

        entry.scaled_ref_layer_offset_present_flag = bs.read_bit();

        if (entry.scaled_ref_layer_offset_present_flag) {
            entry.scaled_ref_layer_left_offset = bs.read_se();

            entry.scaled_ref_layer_top_offset = bs.read_se();

            entry.scaled_ref_layer_right_offset = bs.read_se();

            entry.scaled_ref_layer_bottom_offset = bs.read_se();
        }

        entry.ref_region_offset_present_flag = bs.read_bit();

        if (entry.ref_region_offset_present_flag) {
            entry.ref_region_left_offset = bs.read_se();

            entry.ref_region_top_offset = bs.read_se();

            entry.ref_region_right_offset = bs.read_se();

            entry.ref_region_bottom_offset = bs.read_se();
        }

        entry.resample_phase_set_present_flag = bs.read_bit();

        if (entry.resample_phase_set_present_flag) {
            entry.phase_hor_luma = bs.read_ue();

            entry.phase_ver_luma = bs.read_ue();

            entry.phase_hor_chroma_plus8 = bs.read_ue();

            entry.phase_ver_chroma_plus8 = bs.read_ue();
        }
    }

    /*
     * colour_mapping_enabled_flag
     */
    ext.colour_mapping_enabled_flag = bs.read_bit();

    if (ext.colour_mapping_enabled_flag) {
        parse_pps_colour_mapping_table(bs, ext);

    } else {
        ext.num_cm_ref_layers_minus1 = 0;
        ext.cm_ref_layer_id.clear();
        ext.cm_octant_depth = 0;
        ext.cm_y_part_num_log2 = 0;
        ext.luma_bit_depth_cm_input_minus8 = 0;
        ext.chroma_bit_depth_cm_input_minus8 = 0;
        ext.luma_bit_depth_cm_output_minus8 = 0;
        ext.chroma_bit_depth_cm_output_minus8 = 0;
        ext.cm_res_quant_bits = 0;
        ext.cm_delta_flc_bits_minus1 = 0;
        ext.cm_adapt_threshold_u_delta = 0;
        ext.cm_adapt_threshold_v_delta = 0;
        ext.colour_mapping_octants.clear();
    }
}

/*
 * -----------------------------------------------------------
 * 3D extension
 * -----------------------------------------------------------
 *
 * Annex I.
 */

/*
 * delta_dlt()
 */
inline void parse_pps_delta_dlt(RbspBitstreamReader& bs, Pps3dExtension& ext) {
    const unsigned value_bits = ext.pps_bit_depth_for_depth_layers_minus8 + 8u;

    const std::uint32_t num_val_delta_dlt = static_cast<std::uint32_t>(bs.read_bits(value_bits));

    if (num_val_delta_dlt == 0) {
        return;
    }

    std::uint32_t max_diff = 0;

    if (num_val_delta_dlt > 1) {
        max_diff = static_cast<std::uint32_t>(bs.read_bits(value_bits));
    }

    int min_diff_minus1 = -1;

    if (num_val_delta_dlt > 2 && max_diff != 0) {
        const unsigned len = pps_floor_log2_u32(max_diff) + 1u;

        min_diff_minus1 = static_cast<int>(bs.read_bits(len));
    }

    if (max_diff > static_cast<std::uint32_t>(min_diff_minus1 + 1)) {
        const unsigned len =
            pps_floor_log2_u32(max_diff - static_cast<std::uint32_t>(min_diff_minus1 + 1)) + 1u;

        for (std::uint32_t k = 1; k < num_val_delta_dlt; ++k) {
            bs.skip_bits(len);
        }
    }
}

inline void parse_pps_3d_extension(RbspBitstreamReader& bs, PictureParameterSet& pps) {
    auto& ext = pps.three_d_extension;

    /*
     * dlts_present_flag
     */
    ext.dlts_present_flag = bs.read_bit();

    if (!ext.dlts_present_flag) {
        ext.pps_depth_layers_minus1 = 0;
        ext.pps_bit_depth_for_depth_layers_minus8 = 0;
        ext.depth_layer_transforms.clear();

        return;
    }

    /*
     * pps_depth_layers_minus1
     */
    ext.pps_depth_layers_minus1 = bs.read_u8(6);

    /*
     * pps_bit_depth_for_depth_layers_minus8
     */
    ext.pps_bit_depth_for_depth_layers_minus8 = bs.read_u8(4);

    ext.depth_layer_transforms.clear();

    ext.depth_layer_transforms.resize(ext.pps_depth_layers_minus1 + 1);

    const unsigned value_bits = ext.pps_bit_depth_for_depth_layers_minus8 + 8u;

    for (auto& dlt : ext.depth_layer_transforms) {
        /*
         * dlt_flag[i]
         */
        dlt.dlt_flag = bs.read_bit();

        if (!dlt.dlt_flag) {
            continue;
        }

        /*
         * dlt_pred_flag[i]
         */
        dlt.dlt_pred_flag = bs.read_bit();

        if (dlt.dlt_pred_flag) {
            continue;
        }

        /*
         * dlt_val_flags_present_flag[i]
         */
        dlt.dlt_val_flags_present_flag = bs.read_bit();

        if (dlt.dlt_val_flags_present_flag) {
            const std::size_t count = std::size_t{1} << value_bits;

            dlt.dlt_value_flag.clear();

            dlt.dlt_value_flag.resize(count, false);

            for (std::size_t j = 0; j < count; ++j) {
                dlt.dlt_value_flag[j] = bs.read_bit();
            }

        } else {
            parse_pps_delta_dlt(bs, ext);
        }
    }
}

/*
 * -----------------------------------------------------------
 * Extension flags
 * -----------------------------------------------------------
 */

inline void parse_pps_extensions(RbspBitstreamReader& bs, PictureParameterSet& pps) {
    auto& extension = pps.extension;

    /*
     * pps_extension_present_flag
     */
    extension.extension_present_flag = bs.read_bit();

    if (!extension.extension_present_flag) {
        return;
    }

    /*
     * pps_range_extension_flag
     */
    extension.range_extension_flag = bs.read_bit();

    /*
     * pps_multilayer_extension_flag
     */
    extension.multilayer_extension_flag = bs.read_bit();

    /*
     * pps_3d_extension_flag
     */
    extension.extension_3d_flag = bs.read_bit();

    /*
     * pps_scc_extension_flag
     */
    extension.scc_extension_flag = bs.read_bit();

    /*
     * Reserved pps_extension_4bits.
     */
    for (std::size_t i = 0; i < extension.reserved_extension_flags.size(); ++i) {
        extension.reserved_extension_flags[i] = bs.read_bit();
    }

    /*
     * The extension payload is parsed according to the flags,
     * in signaling order.
     */
    if (extension.range_extension_flag) {
        parse_pps_range_extension(bs, pps);
    }

    if (extension.multilayer_extension_flag) {
        parse_pps_multilayer_extension(bs, pps);
    }

    if (extension.extension_3d_flag) {
        parse_pps_3d_extension(bs, pps);
    }

    if (extension.scc_extension_flag) {
        parse_pps_scc_extension(bs, pps);
    }

    /*
     * Remaining extension_data_flag bits.
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
 * Main PPS parser
 * -----------------------------------------------------------
 */

inline PpsParseResult parse_picture_parameter_set(
    RbspBitstreamReader& bs, PictureParameterSet& pps
) {
    const std::size_t start = bs.bit_position();

    initialize_pps(pps);

    /*
     * =======================================================
     * Identification
     * =======================================================
     */

    /*
     * pps_pic_parameter_set_id
     */
    pps.pps_pic_parameter_set_id = bs.read_ue();

    if (pps.pps_pic_parameter_set_id > kMaxPpsId) {
        throw std::runtime_error("PPS: invalid pps_pic_parameter_set_id");
    }

    /*
     * pps_seq_parameter_set_id
     */
    pps.pps_seq_parameter_set_id = bs.read_ue();

    if (pps.pps_seq_parameter_set_id > kMaxPpsSpsId) {
        throw std::runtime_error("PPS: invalid pps_seq_parameter_set_id");
    }

    /*
     * =======================================================
     * Slice configuration
     * =======================================================
     */

    /*
     * dependent_slice_segments_enabled_flag
     */
    pps.dependent_slice_segments_enabled_flag = bs.read_bit();

    /*
     * output_flag_present_flag
     */
    pps.output_flag_present_flag = bs.read_bit();

    /*
     * num_extra_slice_header_bits
     *
     * u(3)
     */
    pps.num_extra_slice_header_bits = bs.read_u8(3);

    if (pps.num_extra_slice_header_bits > 2) {
        throw std::runtime_error("PPS: invalid num_extra_slice_header_bits");
    }

    /*
     * =======================================================
     * Entropy / QP
     * =======================================================
     */

    /*
     * sign_data_hiding_enabled_flag
     */
    pps.sign_data_hiding_enabled_flag = bs.read_bit();

    /*
     * cabac_init_present_flag
     */
    pps.cabac_init_present_flag = bs.read_bit();

    /*
     * =======================================================
     * Default reference counts
     * =======================================================
     */

    /*
     * num_ref_idx_l0_default_active_minus1
     */
    pps.num_ref_idx_l0_default_active_minus1 = bs.read_ue();

    /*
     * num_ref_idx_l1_default_active_minus1
     */
    pps.num_ref_idx_l1_default_active_minus1 = bs.read_ue();

    /*
     * =======================================================
     * Initial QP
     * =======================================================
     */

    /*
     * init_qp_minus26
     */
    pps.init_qp_minus26 = bs.read_se();

    /*
     * =======================================================
     * Intra / transform
     * =======================================================
     */

    /*
     * constrained_intra_pred_flag
     */
    pps.constrained_intra_pred_flag = bs.read_bit();

    /*
     * transform_skip_enabled_flag
     */
    pps.transform_skip_enabled_flag = bs.read_bit();

    /*
     * cu_qp_delta_enabled_flag
     */
    pps.cu_qp_delta_enabled_flag = bs.read_bit();

    if (pps.cu_qp_delta_enabled_flag) {
        /*
         * diff_cu_qp_delta_depth
         */
        pps.diff_cu_qp_delta_depth = bs.read_ue();

    } else {
        pps.diff_cu_qp_delta_depth = 0;
    }

    /*
     * =======================================================
     * Chroma QP
     * =======================================================
     */

    /*
     * pps_cb_qp_offset
     */
    pps.pps_cb_qp_offset = bs.read_se();

    /*
     * pps_cr_qp_offset
     */
    pps.pps_cr_qp_offset = bs.read_se();

    /*
     * slice_chroma_qp_offsets_present_flag
     */
    pps.slice_chroma_qp_offsets_present_flag = bs.read_bit();

    /*
     * =======================================================
     * Weighted prediction
     * =======================================================
     */

    /*
     * weighted_pred_flag
     */
    pps.weighted_pred_flag = bs.read_bit();

    /*
     * weighted_bipred_flag
     */
    pps.weighted_bipred_flag = bs.read_bit();

    /*
     * =======================================================
     * Transquant bypass
     * =======================================================
     */

    /*
     * transquant_bypass_enabled_flag
     */
    pps.transquant_bypass_enabled_flag = bs.read_bit();

    /*
     * =======================================================
     * Tiles / WPP
     * =======================================================
     */

    parse_pps_tiles(bs, pps);

    /*
     * =======================================================
     * Loop filtering
     * =======================================================
     */

    /*
     * pps_loop_filter_across_slices_enabled_flag
     */
    pps.pps_loop_filter_across_slices_enabled_flag = bs.read_bit();

    /*
     * Deblocking filter control.
     */
    parse_pps_deblocking(bs, pps);

    /*
     * =======================================================
     * Scaling list
     * =======================================================
     */

    /*
     * pps_scaling_list_data_present_flag
     */
    pps.scaling_list_configuration.scaling_list_data_present_flag = bs.read_bit();

    if (pps.scaling_list_configuration.scaling_list_data_present_flag) {
        parse_pps_scaling_list_data(bs, pps.scaling_list_configuration.scaling_list);
    }

    /*
     * =======================================================
     * Reference picture list modification
     * =======================================================
     */

    /*
     * lists_modification_present_flag
     */
    pps.lists_modification_present_flag = bs.read_bit();

    /*
     * =======================================================
     * Parallel merge
     * =======================================================
     */

    /*
     * log2_parallel_merge_level_minus2
     */
    pps.log2_parallel_merge_level_minus2 = bs.read_ue();

    /*
     * =======================================================
     * Slice header extension
     * =======================================================
     */

    /*
     * slice_segment_header_extension_present_flag
     */
    pps.slice_segment_header_extension_present_flag = bs.read_bit();

    /*
     * =======================================================
     * Extensions
     * =======================================================
     */

    parse_pps_extensions(bs, pps);

    /*
     * =======================================================
     * Validation
     * =======================================================
     */

    if (!validate_pps_base(pps)) {
        throw std::runtime_error("PPS: invalid PPS");
    }

    if (!validate_pps_tiles(pps.tiles)) {
        throw std::runtime_error("PPS: invalid tile configuration");
    }

    if (!validate_pps_deblocking(pps.deblocking)) {
        throw std::runtime_error("PPS: invalid deblocking configuration");
    }

    return {true, bs.bit_position() - start};
}

/*
 * -----------------------------------------------------------
 * Convenience overload
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline PictureParameterSet parse_picture_parameter_set(RbspBitstreamReader& bs) {
    PictureParameterSet pps{};

    parse_picture_parameter_set(bs, pps);

    return pps;
}

/*
 * -----------------------------------------------------------
 * Validation helper
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline bool validate_picture_parameter_set(const PictureParameterSet& pps) noexcept {
    if (!validate_pps_base(pps)) {
        return false;
    }

    if (!validate_pps_tiles(pps.tiles)) {
        return false;
    }

    if (!validate_pps_deblocking(pps.deblocking)) {
        return false;
    }

    return true;
}

}  // namespace bs