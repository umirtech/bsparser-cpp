// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "hevc_common.hpp"
#include "hevc_scaling_list.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bs {

/*
 * H.265 Picture Parameter Set
 *
 * Corresponds to:
 *
 *     7.3.2.3.1 General picture parameter set RBSP syntax
 *
 * Extensions are represented separately:
 *
 *     7.3.2.3.2 PPS range extension
 *     7.3.2.3.3 PPS screen content coding extension
 *
 * This is a syntax/data model only.
 *
 * No bitstream parsing is performed here.
 */

/*
 * -----------------------------------------------------------
 * Tile configuration
 * -----------------------------------------------------------
 *
 * H.265:
 *
 *     tiles_enabled_flag
 *     num_tile_columns_minus1
 *     num_tile_rows_minus1
 *     uniform_spacing_flag
 *
 * followed by explicit column widths / row heights when
 * uniform_spacing_flag == 0.
 */

struct PpsTileConfiguration {
    bool tiles_enabled_flag = false;

    /*
     * Syntax values.
     */
    std::uint32_t num_tile_columns_minus1 = 0;
    std::uint32_t num_tile_rows_minus1 = 0;

    bool uniform_spacing_flag = false;

    /*
     * Explicit:
     *
     *     column_width_minus1[]
     *
     * Number of entries:
     *
     *     num_tile_columns_minus1
     *
     * because the final column width is inferred.
     */
    std::vector<std::uint32_t> column_width_minus1;

    /*
     * Explicit:
     *
     *     row_height_minus1[]
     *
     * Number of entries:
     *
     *     num_tile_rows_minus1
     *
     * because the final row height is inferred.
     */
    std::vector<std::uint32_t> row_height_minus1;

    /*
     * loop_filter_across_tiles_enabled_flag
     */
    bool loop_filter_across_tiles_enabled_flag = false;

    [[nodiscard]]
    constexpr std::size_t tile_column_count() const noexcept {
        if (!tiles_enabled_flag) {
            return 1;
        }

        return static_cast<std::size_t>(num_tile_columns_minus1) + 1;
    }

    [[nodiscard]]
    constexpr std::size_t tile_row_count() const noexcept {
        if (!tiles_enabled_flag) {
            return 1;
        }

        return static_cast<std::size_t>(num_tile_rows_minus1) + 1;
    }

    [[nodiscard]]
    bool uses_uniform_spacing() const noexcept {
        return uniform_spacing_flag;
    }
};

/*
 * -----------------------------------------------------------
 * Deblocking filter configuration
 * -----------------------------------------------------------
 */

struct PpsDeblockingFilter {
    /*
     * deblocking_filter_control_present_flag
     */
    bool deblocking_filter_control_present_flag = false;

    /*
     * deblocking_filter_override_enabled_flag
     */
    bool deblocking_filter_override_enabled_flag = false;

    /*
     * pps_deblocking_filter_disabled_flag
     */
    bool pps_deblocking_filter_disabled_flag = false;

    /*
     * Present when:
     *
     *     pps_deblocking_filter_disabled_flag == 0
     */
    std::int32_t pps_beta_offset_div2 = 0;
    std::int32_t pps_tc_offset_div2 = 0;
};

/*
 * -----------------------------------------------------------
 * PPS scaling-list configuration
 * -----------------------------------------------------------
 */

struct PpsScalingListConfiguration {
    /*
     * pps_scaling_list_data_present_flag
     */
    bool scaling_list_data_present_flag = false;

    ScalingListData scaling_list{};
};

/*
 * -----------------------------------------------------------
 * PPS extension flags
 * -----------------------------------------------------------
 */

struct PpsExtension {
    /*
     * pps_extension_present_flag
     */
    bool extension_present_flag = false;

    /*
     * pps_range_extension_flag
     */
    bool range_extension_flag = false;

    /*
     * pps_multilayer_extension_flag
     */
    bool multilayer_extension_flag = false;

    /*
     * pps_3d_extension_flag
     *
     * Kept here so the base structure can represent the
     * signaling without implementing the 3D extension yet.
     */
    bool extension_3d_flag = false;

    /*
     * pps_scc_extension_flag
     */
    bool scc_extension_flag = false;

    /*
     * Reserved extension flags.
     */
    std::array<bool, 4> reserved_extension_flags{};

    /*
     * Whether extension_data_flag bits were present.
     */
    bool extension_data_present = false;
};

/*
 * -----------------------------------------------------------
 * PPS range extension
 * -----------------------------------------------------------
 *
 * 7.3.2.3.2
 *
 * These fields are only meaningful when:
 *
 *     pps_range_extension_flag == 1
 */

struct PpsRangeExtension {
    /*
     * transform_skip_enabled_flag is actually a general
     * PPS field in the base syntax, but the range extension
     * adds additional transform-skip configuration.
     */

    /*
     * log2_max_transform_skip_block_size_minus2
     */
    std::uint32_t log2_max_transform_skip_block_size_minus2 = 0;

    /*
     * cross_component_prediction_enabled_flag
     */
    bool cross_component_prediction_enabled_flag = false;

    /*
     * chroma_qp_offset_list_enabled_flag
     */
    bool chroma_qp_offset_list_enabled_flag = false;

    /*
     * diff_cu_chroma_qp_offset_depth
     */
    std::uint32_t diff_cu_chroma_qp_offset_depth = 0;

    /*
     * chroma_qp_offset_list_len_minus1
     */
    std::uint32_t chroma_qp_offset_list_len_minus1 = 0;

    /*
     * cb_qp_offset_list[]
     */
    std::vector<std::int32_t> cb_qp_offset_list;

    /*
     * cr_qp_offset_list[]
     */
    std::vector<std::int32_t> cr_qp_offset_list;

    /*
     * log2_sao_offset_scale_luma
     */
    std::uint32_t log2_sao_offset_scale_luma = 0;

    /*
     * log2_sao_offset_scale_chroma
     */
    std::uint32_t log2_sao_offset_scale_chroma = 0;

    [[nodiscard]]
    std::size_t chroma_qp_offset_count() const noexcept {
        return chroma_qp_offset_list_len_minus1 + 1;
    }
};

/*
 * -----------------------------------------------------------
 * PPS screen-content-coding extension
 * -----------------------------------------------------------
 *
 * 7.3.2.3.4
 *
 * These fields are conditionally present when SCC extension
 * signaling is enabled.
 */

struct PpsSccExtension {
    /*
     * pps_curr_pic_ref_enabled_flag
     */
    bool pps_curr_pic_ref_enabled_flag = false;

    /*
     * residual_adaptive_colour_transform_enabled_flag
     */
    bool residual_adaptive_colour_transform_enabled_flag = false;

    /*
     * Present only when the ACT flag is enabled.
     */
    bool pps_slice_act_qp_offsets_present_flag = false;

    /*
     * Raw se(v) values:
     *
     *     pps_act_y_qp_offset_plus5
     *     pps_act_cb_qp_offset_plus5
     *     pps_act_cr_qp_offset_plus3
     */
    std::int32_t pps_act_y_qp_offset_plus5 = 0;
    std::int32_t pps_act_cb_qp_offset_plus5 = 0;
    std::int32_t pps_act_cr_qp_offset_plus3 = 0;

    /*
     * Palette predictor initializers.
     */
    bool pps_palette_predictor_initializers_present_flag = false;

    std::uint32_t pps_num_palette_predictor_initializers = 0;

    bool monochrome_palette_flag = false;

    std::uint32_t luma_bit_depth_entry_minus8 = 0;

    std::uint32_t chroma_bit_depth_entry_minus8 = 0;

    /*
     * pps_palette_predictor_initializer[ comp ][ i ]
     *
     * Width:
     *
     *     8 + luma_bit_depth_entry_minus8   (comp == 0)
     *     8 + chroma_bit_depth_entry_minus8 (comp != 0)
     */
    std::array<std::array<std::uint32_t, kMaxPalettePredictorSize>, 3>
        pps_palette_predictor_initializer{};
};

/*
 * -----------------------------------------------------------
 * PPS multilayer extension
 * -----------------------------------------------------------
 *
 * 7.3.2.3.5
 *
 * Present when pps_multilayer_extension_flag == 1.
 */

struct PpsRefLocationOffset {
    std::uint32_t ref_loc_offset_layer_id = 0;

    bool scaled_ref_layer_offset_present_flag = false;

    std::int32_t scaled_ref_layer_left_offset = 0;
    std::int32_t scaled_ref_layer_top_offset = 0;
    std::int32_t scaled_ref_layer_right_offset = 0;
    std::int32_t scaled_ref_layer_bottom_offset = 0;

    bool ref_region_offset_present_flag = false;

    std::int32_t ref_region_left_offset = 0;
    std::int32_t ref_region_top_offset = 0;
    std::int32_t ref_region_right_offset = 0;
    std::int32_t ref_region_bottom_offset = 0;

    bool resample_phase_set_present_flag = false;

    std::uint32_t phase_hor_luma = 0;
    std::uint32_t phase_ver_luma = 0;
    std::uint32_t phase_hor_chroma_plus8 = 0;
    std::uint32_t phase_ver_chroma_plus8 = 0;
};

/*
 * One octant of the colour-mapping table.
 *
 * The octants are stored in pre-order traversal; split
 * octants appear as internal nodes followed immediately by
 * their eight children.
 */
struct PpsColourMappingOctant {
    bool split_octant_flag = false;

    /*
     * coded_res_flag for each (i, j) partition; only present
     * for leaf octants.
     *
     * Layout:
     *
     *     [i * 4 + j]
     */
    std::vector<bool> partition_coded_res_flags;
};

struct PpsMultilayerExtension {
    bool poc_reset_info_present_flag = false;

    bool pps_infer_scaling_list_flag = false;

    std::uint32_t pps_scaling_list_ref_layer_id = 0;

    std::vector<PpsRefLocationOffset> ref_location_offsets;

    /*
     * Colour-mapping table.
     */
    bool colour_mapping_enabled_flag = false;

    std::uint32_t num_cm_ref_layers_minus1 = 0;

    std::vector<std::uint32_t> cm_ref_layer_id;

    std::uint32_t cm_octant_depth = 0;

    std::uint32_t cm_y_part_num_log2 = 0;

    std::uint32_t luma_bit_depth_cm_input_minus8 = 0;
    std::uint32_t chroma_bit_depth_cm_input_minus8 = 0;
    std::uint32_t luma_bit_depth_cm_output_minus8 = 0;
    std::uint32_t chroma_bit_depth_cm_output_minus8 = 0;

    std::uint32_t cm_res_quant_bits = 0;

    std::uint32_t cm_delta_flc_bits_minus1 = 0;

    std::int32_t cm_adapt_threshold_u_delta = 0;
    std::int32_t cm_adapt_threshold_v_delta = 0;

    /*
     * Octant tree in pre-order.
     */
    std::vector<PpsColourMappingOctant> colour_mapping_octants;
};

/*
 * -----------------------------------------------------------
 * PPS 3D extension
 * -----------------------------------------------------------
 *
 * Annex I (H.265 3D).
 *
 * Present when pps_3d_extension_flag == 1.
 */

struct Pps3dExtension {
    /*
     * dlts_present_flag
     */
    bool dlts_present_flag = false;

    std::uint32_t pps_depth_layers_minus1 = 0;

    std::uint32_t pps_bit_depth_for_depth_layers_minus8 = 0;

    /*
     * Per depth layer:
     *
     *     dlt_flag[i]
     *     dlt_pred_flag[i]
     *     dlt_val_flags_present_flag[i]
     *     dlt_value_flag[i][j]
     */
    struct DepthLayerTransform {
        bool dlt_flag = false;
        bool dlt_pred_flag = false;
        bool dlt_val_flags_present_flag = false;
        std::vector<bool> dlt_value_flag;
    };

    std::vector<DepthLayerTransform> depth_layer_transforms;
};

/*
 * -----------------------------------------------------------
 * Complete Picture Parameter Set
 * -----------------------------------------------------------
 */

struct PictureParameterSet {
    /*
     * =======================================================
     * Identification
     * =======================================================
     */

    /*
     * pps_pic_parameter_set_id
     *
     * ue(v)
     *
     * H.265 V11 limits this to 0..63.
     */
    std::uint32_t pps_pic_parameter_set_id = 0;

    /*
     * pps_seq_parameter_set_id
     *
     * ue(v)
     */
    std::uint32_t pps_seq_parameter_set_id = 0;

    /*
     * =======================================================
     * Slice-segment configuration
     * =======================================================
     */

    /*
     * dependent_slice_segments_enabled_flag
     */
    bool dependent_slice_segments_enabled_flag = false;

    /*
     * output_flag_present_flag
     */
    bool output_flag_present_flag = false;

    /*
     * num_extra_slice_header_bits
     *
     * u(3)
     */
    std::uint8_t num_extra_slice_header_bits = 0;

    /*
     * =======================================================
     * Entropy / QP configuration
     * =======================================================
     */

    /*
     * sign_data_hiding_enabled_flag
     */
    bool sign_data_hiding_enabled_flag = false;

    /*
     * cabac_init_present_flag
     */
    bool cabac_init_present_flag = false;

    /*
     * =======================================================
     * Default reference picture counts
     * =======================================================
     */

    /*
     * num_ref_idx_l0_default_active_minus1
     */
    std::uint32_t num_ref_idx_l0_default_active_minus1 = 0;

    /*
     * num_ref_idx_l1_default_active_minus1
     */
    std::uint32_t num_ref_idx_l1_default_active_minus1 = 0;

    /*
     * =======================================================
     * Initial QP / chroma QP
     * =======================================================
     */

    /*
     * init_qp_minus26
     *
     * se(v)
     */
    std::int32_t init_qp_minus26 = 0;

    /*
     * pps_cb_qp_offset
     */
    std::int32_t pps_cb_qp_offset = 0;

    /*
     * pps_cr_qp_offset
     */
    std::int32_t pps_cr_qp_offset = 0;

    /*
     * =======================================================
     * Intra / transform tools
     * =======================================================
     */

    /*
     * constrained_intra_pred_flag
     */
    bool constrained_intra_pred_flag = false;

    /*
     * transform_skip_enabled_flag
     */
    bool transform_skip_enabled_flag = false;

    /*
     * cu_qp_delta_enabled_flag
     */
    bool cu_qp_delta_enabled_flag = false;

    /*
     * diff_cu_qp_delta_depth
     *
     * Only present when cu_qp_delta_enabled_flag == 1.
     */
    std::uint32_t diff_cu_qp_delta_depth = 0;

    /*
     * =======================================================
     * Chroma QP offsets
     * =======================================================
     */

    /*
     * slice_chroma_qp_offsets_present_flag
     */
    bool slice_chroma_qp_offsets_present_flag = false;

    /*
     * =======================================================
     * Weighted prediction
     * =======================================================
     */

    /*
     * weighted_pred_flag
     */
    bool weighted_pred_flag = false;

    /*
     * weighted_bipred_flag
     */
    bool weighted_bipred_flag = false;

    /*
     * =======================================================
     * Transquant bypass
     * =======================================================
     */

    /*
     * transquant_bypass_enabled_flag
     */
    bool transquant_bypass_enabled_flag = false;

    /*
     * =======================================================
     * Tiles / entropy synchronization
     * =======================================================
     */

    /*
     * tiles_enabled_flag and associated syntax.
     */
    PpsTileConfiguration tiles{};

    /*
     * entropy_coding_sync_enabled_flag
     */
    bool entropy_coding_sync_enabled_flag = false;

    /*
     * =======================================================
     * Loop filtering
     * =======================================================
     */

    /*
     * pps_loop_filter_across_slices_enabled_flag
     */
    bool pps_loop_filter_across_slices_enabled_flag = false;

    PpsDeblockingFilter deblocking{};

    /*
     * =======================================================
     * Scaling list
     * =======================================================
     */

    PpsScalingListConfiguration scaling_list_configuration{};

    /*
     * =======================================================
     * Reference picture list modification
     * =======================================================
     */

    /*
     * lists_modification_present_flag
     */
    bool lists_modification_present_flag = false;

    /*
     * =======================================================
     * Merge / parallel processing
     * =======================================================
     */

    /*
     * log2_parallel_merge_level_minus2
     */
    std::uint32_t log2_parallel_merge_level_minus2 = 0;

    /*
     * =======================================================
     * Slice-header extension
     * =======================================================
     */

    /*
     * slice_segment_header_extension_present_flag
     */
    bool slice_segment_header_extension_present_flag = false;

    /*
     * =======================================================
     * Extensions
     * =======================================================
     */

    PpsExtension extension{};

    PpsRangeExtension range_extension{};

    PpsSccExtension scc_extension{};

    PpsMultilayerExtension multilayer_extension{};

    Pps3dExtension three_d_extension{};

    /*
     * =======================================================
     * Helpers
     * =======================================================
     */

    [[nodiscard]]
    constexpr std::uint32_t pps_id() const noexcept {
        return pps_pic_parameter_set_id;
    }

    [[nodiscard]]
    constexpr std::uint32_t sps_id() const noexcept {
        return pps_seq_parameter_set_id;
    }

    [[nodiscard]]
    constexpr std::uint32_t num_ref_idx_l0_default_active() const noexcept {
        return num_ref_idx_l0_default_active_minus1 + 1;
    }

    [[nodiscard]]
    constexpr std::uint32_t num_ref_idx_l1_default_active() const noexcept {
        return num_ref_idx_l1_default_active_minus1 + 1;
    }

    [[nodiscard]]
    constexpr std::int32_t initial_qp() const noexcept {
        return 26 + init_qp_minus26;
    }

    [[nodiscard]]
    constexpr std::uint32_t parallel_merge_level() const noexcept {
        return std::uint32_t{1} << (log2_parallel_merge_level_minus2 + 2);
    }

    [[nodiscard]]
    constexpr std::uint32_t cu_qp_delta_depth() const noexcept {
        if (!cu_qp_delta_enabled_flag) {
            return 0;
        }

        return diff_cu_qp_delta_depth;
    }

    [[nodiscard]]
    constexpr bool has_scaling_list() const noexcept {
        return scaling_list_configuration.scaling_list_data_present_flag;
    }

    [[nodiscard]]
    constexpr bool has_range_extension() const noexcept {
        return extension.range_extension_flag;
    }

    [[nodiscard]]
    constexpr bool has_scc_extension() const noexcept {
        return extension.scc_extension_flag;
    }

    [[nodiscard]]
    constexpr bool valid() const noexcept {
        return pps_pic_parameter_set_id <= 63 && pps_seq_parameter_set_id <= 15 &&
               num_extra_slice_header_bits <= 2;
    }
};

/*
 * -----------------------------------------------------------
 * Tile helpers
 * -----------------------------------------------------------
 */

/*
 * Initialize explicit tile arrays.
 *
 * The syntax has:
 *
 *     for(i = 0; i < num_tile_columns_minus1; i++)
 *         column_width_minus1[i]
 *
 * and:
 *
 *     for(i = 0; i < num_tile_rows_minus1; i++)
 *         row_height_minus1[i]
 */
inline void initialize_pps_tiles(PpsTileConfiguration& tiles) {
    if (!tiles.tiles_enabled_flag) {
        tiles.column_width_minus1.clear();
        tiles.row_height_minus1.clear();
        return;
    }

    if (tiles.uniform_spacing_flag) {
        tiles.column_width_minus1.clear();
        tiles.row_height_minus1.clear();
        return;
    }

    tiles.column_width_minus1.resize(tiles.num_tile_columns_minus1);

    tiles.row_height_minus1.resize(tiles.num_tile_rows_minus1);
}

/*
 * Number of explicit column widths.
 */
[[nodiscard]]
constexpr std::size_t explicit_tile_column_width_count(const PpsTileConfiguration& tiles) noexcept {
    if (!tiles.tiles_enabled_flag || tiles.uniform_spacing_flag) {
        return 0;
    }

    return tiles.num_tile_columns_minus1;
}

/*
 * Number of explicit row heights.
 */
[[nodiscard]]
constexpr std::size_t explicit_tile_row_height_count(const PpsTileConfiguration& tiles) noexcept {
    if (!tiles.tiles_enabled_flag || tiles.uniform_spacing_flag) {
        return 0;
    }

    return tiles.num_tile_rows_minus1;
}

/*
 * -----------------------------------------------------------
 * Scaling-list initialization
 * -----------------------------------------------------------
 */

inline void initialize_pps_scaling_list(PictureParameterSet& pps) {
    initialize_scaling_list_data(pps.scaling_list_configuration.scaling_list);
}

/*
 * -----------------------------------------------------------
 * Chroma QP offset-list initialization
 * -----------------------------------------------------------
 */

inline void initialize_pps_range_extension(PpsRangeExtension& extension) {
    if (!extension.chroma_qp_offset_list_enabled_flag) {
        extension.cb_qp_offset_list.clear();
        extension.cr_qp_offset_list.clear();
        return;
    }

    const auto count = static_cast<std::size_t>(extension.chroma_qp_offset_list_len_minus1) + 1;

    extension.cb_qp_offset_list.resize(count);
    extension.cr_qp_offset_list.resize(count);
}

/*
 * -----------------------------------------------------------
 * Validation
 * -----------------------------------------------------------
 */

/*
 * H.265 PPS ID range.
 */
inline constexpr std::uint32_t kMaxPpsPicParameterSetId = 63;

/*
 * H.265 SPS ID range referenced by PPS.
 */
inline constexpr std::uint32_t kMaxPpsSequenceParameterSetId = 15;

/*
 * Validate core PPS fields.
 */
[[nodiscard]]
constexpr bool validate_pps_base(const PictureParameterSet& pps) noexcept {
    if (pps.pps_pic_parameter_set_id > kMaxPpsPicParameterSetId) {
        return false;
    }

    if (pps.pps_seq_parameter_set_id > kMaxPpsSequenceParameterSetId) {
        return false;
    }

    /*
     * H.265:
     *
     *     num_extra_slice_header_bits <= 2
     */
    if (pps.num_extra_slice_header_bits > 2) {
        return false;
    }

    return true;
}

/*
 * Validate tile array dimensions.
 */
[[nodiscard]]
inline bool validate_pps_tiles(const PpsTileConfiguration& tiles) noexcept {
    if (!tiles.tiles_enabled_flag) {
        return tiles.column_width_minus1.empty() && tiles.row_height_minus1.empty();
    }

    if (tiles.uniform_spacing_flag) {
        return tiles.column_width_minus1.empty() && tiles.row_height_minus1.empty();
    }

    return tiles.column_width_minus1.size() == tiles.num_tile_columns_minus1 &&
           tiles.row_height_minus1.size() == tiles.num_tile_rows_minus1;
}

/*
 * Validate the deblocking configuration.
 */
[[nodiscard]]
constexpr bool validate_pps_deblocking(const PpsDeblockingFilter& deblocking) noexcept {
    if (!deblocking.deblocking_filter_control_present_flag) {
        return true;
    }

    /*
     * If the filter is disabled, the offsets are not used
     * by the syntax.
     */
    if (deblocking.pps_deblocking_filter_disabled_flag) {
        return true;
    }

    /*
     * H.265 bounds:
     *
     * beta_offset_div2 and tc_offset_div2 are constrained
     * by the PPS semantics.
     *
     * Keep the model permissive here; exact conformance
     * checking belongs in the parser/validator layer.
     */
    return true;
}

/*
 * -----------------------------------------------------------
 * Semantic QP helpers
 * -----------------------------------------------------------
 */

/*
 * Effective luma QP before slice/CU deltas.
 */
[[nodiscard]]
constexpr std::int32_t pps_base_qp(const PictureParameterSet& pps) noexcept {
    return pps.initial_qp();
}

/*
 * Effective Cb offset contributed by the PPS.
 */
[[nodiscard]]
constexpr std::int32_t pps_cb_offset(const PictureParameterSet& pps) noexcept {
    return pps.pps_cb_qp_offset;
}

/*
 * Effective Cr offset contributed by the PPS.
 */
[[nodiscard]]
constexpr std::int32_t pps_cr_offset(const PictureParameterSet& pps) noexcept {
    return pps.pps_cr_qp_offset;
}

/*
 * -----------------------------------------------------------
 * PPS defaults
 * -----------------------------------------------------------
 */

/*
 * Construct a PPS in its normal syntax defaults.
 *
 * This is useful before parsing conditional syntax.
 */
inline void initialize_pps(PictureParameterSet& pps) {
    pps = {};

    pps.num_ref_idx_l0_default_active_minus1 = 0;
    pps.num_ref_idx_l1_default_active_minus1 = 0;

    pps.init_qp_minus26 = 0;

    pps.pps_cb_qp_offset = 0;
    pps.pps_cr_qp_offset = 0;

    pps.log2_parallel_merge_level_minus2 = 0;

    initialize_scaling_list_data(pps.scaling_list_configuration.scaling_list);
}

/*
 * -----------------------------------------------------------
 * Convenience predicates
 * -----------------------------------------------------------
 */

[[nodiscard]]
constexpr bool tiles_enabled(const PictureParameterSet& pps) noexcept {
    return pps.tiles.tiles_enabled_flag;
}

[[nodiscard]]
constexpr bool entropy_coding_sync_enabled(const PictureParameterSet& pps) noexcept {
    return pps.entropy_coding_sync_enabled_flag;
}

[[nodiscard]]
constexpr bool deblocking_control_present(const PictureParameterSet& pps) noexcept {
    return pps.deblocking.deblocking_filter_control_present_flag;
}

[[nodiscard]]
constexpr bool dependent_slice_segments_enabled(const PictureParameterSet& pps) noexcept {
    return pps.dependent_slice_segments_enabled_flag;
}

}  // namespace bs