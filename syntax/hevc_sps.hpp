#pragma once

#include "hevc_common.hpp"
#include "hevc_profile_tier_level.hpp"
#include "hevc_scaling_list.hpp"
#include "hevc_short_term_ref_pic_set.hpp"
#include "hevc_vui.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bs {

/*
 * H.265 Sequence Parameter Set
 *
 * Corresponds to:
 *
 *     7.3.2.2.1 General sequence parameter set RBSP syntax
 *
 * The structure contains decoded syntax values.
 *
 * Parser code belongs in:
 *
 *     parser/hevc_sps_parser.hpp
 */

/*
 * -----------------------------------------------------------
 * SPS sub-layer ordering
 * -----------------------------------------------------------
 */

using SpsSubLayerOrderingInfo = SubLayerOrderingInfo;

/*
 * -----------------------------------------------------------
 * SPS coding-block configuration
 * -----------------------------------------------------------
 *
 * The syntax stores these values as logarithmic differences.
 *
 * We retain both the signaled values and provide helpers for
 * the actual block sizes.
 */

struct SpsCodingBlockParameters {
    /*
     * log2_min_luma_coding_block_size_minus3
     */
    std::uint32_t log2_min_luma_coding_block_size_minus3 = 0;

    /*
     * log2_diff_max_min_luma_coding_block_size
     */
    std::uint32_t log2_diff_max_min_luma_coding_block_size = 0;

    /*
     * log2_min_luma_transform_block_size_minus2
     */
    std::uint32_t log2_min_luma_transform_block_size_minus2 = 0;

    /*
     * log2_diff_max_min_luma_transform_block_size
     */
    std::uint32_t log2_diff_max_min_luma_transform_block_size = 0;

    /*
     * max_transform_hierarchy_depth_inter
     */
    std::uint32_t max_transform_hierarchy_depth_inter = 0;

    /*
     * max_transform_hierarchy_depth_intra
     */
    std::uint32_t max_transform_hierarchy_depth_intra = 0;

    /*
     * -------------------------------------------------------
     * Derived sizes
     * -------------------------------------------------------
     */

    [[nodiscard]]
    constexpr std::uint32_t min_luma_coding_block_size() const noexcept {
        return std::uint32_t{1} << (log2_min_luma_coding_block_size_minus3 + 3);
    }

    [[nodiscard]]
    constexpr std::uint32_t max_luma_coding_block_size() const noexcept {
        return std::uint32_t{1}
               << (log2_min_luma_coding_block_size_minus3 +
                   log2_diff_max_min_luma_coding_block_size + 3);
    }

    [[nodiscard]]
    constexpr std::uint32_t min_luma_transform_block_size() const noexcept {
        return std::uint32_t{1} << (log2_min_luma_transform_block_size_minus2 + 2);
    }

    [[nodiscard]]
    constexpr std::uint32_t max_luma_transform_block_size() const noexcept {
        return std::uint32_t{1}
               << (log2_min_luma_transform_block_size_minus2 +
                   log2_diff_max_min_luma_transform_block_size + 2);
    }
};

/*
 * -----------------------------------------------------------
 * SPS PCM configuration
 * -----------------------------------------------------------
 */

struct SpsPcmParameters {
    /*
     * pcm_enabled_flag
     */
    bool pcm_enabled_flag = false;

    /*
     * pcm_sample_bit_depth_luma_minus1
     */
    std::uint8_t pcm_sample_bit_depth_luma_minus1 = 0;

    /*
     * pcm_sample_bit_depth_chroma_minus1
     */
    std::uint8_t pcm_sample_bit_depth_chroma_minus1 = 0;

    /*
     * log2_min_pcm_luma_coding_block_size_minus3
     */
    std::uint32_t log2_min_pcm_luma_coding_block_size_minus3 = 0;

    /*
     * log2_diff_max_min_pcm_luma_coding_block_size
     */
    std::uint32_t log2_diff_max_min_pcm_luma_coding_block_size = 0;

    /*
     * pcm_loop_filter_disabled_flag
     */
    bool pcm_loop_filter_disabled_flag = false;

    [[nodiscard]]
    constexpr std::uint32_t min_pcm_luma_coding_block_size() const noexcept {
        return std::uint32_t{1} << (log2_min_pcm_luma_coding_block_size_minus3 + 3);
    }

    [[nodiscard]]
    constexpr std::uint32_t max_pcm_luma_coding_block_size() const noexcept {
        return std::uint32_t{1}
               << (log2_min_pcm_luma_coding_block_size_minus3 +
                   log2_diff_max_min_pcm_luma_coding_block_size + 3);
    }

    [[nodiscard]]
    constexpr std::uint8_t luma_bit_depth() const noexcept {
        return static_cast<std::uint8_t>(pcm_sample_bit_depth_luma_minus1 + 1);
    }

    [[nodiscard]]
    constexpr std::uint8_t chroma_bit_depth() const noexcept {
        return static_cast<std::uint8_t>(pcm_sample_bit_depth_chroma_minus1 + 1);
    }
};

/*
 * -----------------------------------------------------------
 * SPS reference-picture-set configuration
 * -----------------------------------------------------------
 */

struct SpsReferencePictureSetParameters {
    /*
     * num_short_term_ref_pic_sets
     */
    std::uint32_t num_short_term_ref_pic_sets = 0;

    /*
     * short_term_ref_pic_set[]
     */
    std::vector<ShortTermRefPicSet> short_term_ref_pic_sets;

    /*
     * long_term_ref_pics_present_flag
     */
    bool long_term_ref_pics_present_flag = false;

    /*
     * num_long_term_ref_pics_sps
     */
    std::uint32_t num_long_term_ref_pics_sps = 0;

    /*
     * lt_ref_pic_poc_lsb_sps[]
     *
     * Width is:
     *
     *     log2_max_pic_order_cnt_lsb_minus4 + 4
     */
    std::vector<std::uint32_t> lt_ref_pic_poc_lsb_sps;

    /*
     * used_by_curr_pic_lt_sps_flag[]
     */
    std::vector<bool> used_by_curr_pic_lt_sps_flag;
};

/*
 * -----------------------------------------------------------
 * SPS long-term reference picture information
 * -----------------------------------------------------------
 */

struct SpsLongTermReferencePictures {
    bool present = false;

    std::uint32_t count = 0;

    std::vector<std::uint32_t> poc_lsb;

    std::vector<bool> used_by_curr_pic;
};

/*
 * -----------------------------------------------------------
 * SPS extension flags
 * -----------------------------------------------------------
 *
 * Base/general SPS keeps extension signaling separate from
 * the actual extension syntax.
 */

struct SpsExtension {
    /*
     * sps_extension_present_flag
     */
    bool sps_extension_present_flag = false;

    /*
     * Base extension flags.
     *
     * The actual extension payload is parsed separately.
     */
    bool range_extension_flag = false;

    bool multilayer_extension_flag = false;

    /*
     * sps_3d_extension_flag
     */
    bool extension_3d_flag = false;

    bool scc_extension_flag = false;

    /*
     * Reserved sps_extension_4bits.
     */
    std::array<bool, 4> reserved_extension_flags{};

    /*
     * sps_extension_data_flag while more_rbsp_data().
     *
     * We don't need to store every repeated bit unless a
     * caller explicitly wants the opaque extension payload.
     */
    bool extension_data_present = false;
};

/*
 * -----------------------------------------------------------
 * SPS range extension
 * -----------------------------------------------------------
 *
 * 7.3.2.2.4
 *
 * Present when sps_range_extension_flag == 1.
 *
 * This consists of nine single-bit tool-enablement flags.
 */

struct SpsRangeExtension {
    bool transform_skip_rotation_enabled_flag = false;

    bool transform_skip_context_enabled_flag = false;

    bool implicit_rdpcm_enabled_flag = false;

    bool explicit_rdpcm_enabled_flag = false;

    bool extended_precision_processing_flag = false;

    bool intra_smoothing_disabled_flag = false;

    bool high_precision_offsets_enabled_flag = false;

    bool persistent_rice_adaptation_enabled_flag = false;

    bool cabac_bypass_alignment_enabled_flag = false;
};

/*
 * -----------------------------------------------------------
 * SPS multilayer extension
 * -----------------------------------------------------------
 *
 * 7.3.2.2.5
 *
 * Present when sps_multilayer_extension_flag == 1.
 *
 * The syntax consists of a single flag.
 */

struct SpsMultilayerExtension {
    /*
     * inter_view_mv_vert_constraint_flag
     */
    bool inter_view_mv_vert_constraint_flag = false;
};

/*
 * -----------------------------------------------------------
 * SPS 3D extension
 * -----------------------------------------------------------
 *
 * Annex I (H.265 3D).
 *
 * Present when sps_3d_extension_flag == 1.
 *
 * The extension is a two-entry loop with one structure per
 * entry; the second entry carries the texture-coding flags.
 */

struct Sps3dViewExtension {
    bool iv_di_mc_enabled_flag = false;

    bool iv_mv_scal_enabled_flag = false;

    /*
     * Entry 0 only.
     */
    bool iv_res_pred_enabled_flag = false;
    bool depth_ref_enabled_flag = false;
    bool vsp_mc_enabled_flag = false;
    bool dbbp_enabled_flag = false;

    /*
     * Entry 1 only.
     */
    bool tex_mc_enabled_flag = false;
    bool intra_contour_enabled_flag = false;
    bool intra_dc_only_wedge_enabled_flag = false;
    bool cqt_cu_part_pred_enabled_flag = false;
    bool inter_dc_only_enabled_flag = false;
    bool skip_intra_enabled_flag = false;

    /*
     * log2_ivmc_sub_pb_size_minus3
     */
    std::uint32_t log2_ivmc_sub_pb_size_minus3 = 0;
};

struct Sps3dExtension {
    std::array<Sps3dViewExtension, 2> views{};
};

/*
 * -----------------------------------------------------------
 * SPS screen-content-coding extension
 * -----------------------------------------------------------
 *
 * 7.3.2.2.6
 *
 * Present when sps_scc_extension_flag == 1.
 */

struct SpsSccExtension {
    /*
     * sps_curr_pic_ref_enabled_flag
     */
    bool sps_curr_pic_ref_enabled_flag = false;

    /*
     * palette_mode_enabled_flag
     */
    bool palette_mode_enabled_flag = false;

    /*
     * Present only when palette_mode_enabled_flag == 1.
     */
    std::uint32_t palette_max_size = 0;

    std::uint32_t delta_palette_max_predictor_size = 0;

    bool sps_palette_predictor_initializers_present_flag = false;

    /*
     * sps_num_palette_predictor_initializers_minus1
     */
    std::uint32_t sps_num_palette_predictor_initializers_minus1 = 0;

    /*
     * sps_palette_predictor_initializer[ comp ][ i ]
     *
     * Width:
     *
     *     comp == 0: BitDepthY
     *     comp != 0: BitDepthC
     */
    std::array<std::array<std::uint32_t, kMaxPalettePredictorSize>, 3>
        sps_palette_predictor_initializer{};

    /*
     * motion_vector_resolution_control_idc
     *
     * u(2)
     */
    std::uint32_t motion_vector_resolution_control_idc = 0;

    /*
     * intra_boundary_filtering_disabled_flag
     */
    bool intra_boundary_filtering_disabled_flag = false;
};

/*
 * -----------------------------------------------------------
 * SPS derived geometry
 * -----------------------------------------------------------
 *
 * This isn't direct syntax. It is derived from:
 *
 *     pic_width_in_luma_samples
 *     pic_height_in_luma_samples
 *     conformance_window
 *     chroma_format_idc
 */

struct SpsGeometry {
    /*
     * Coded dimensions.
     */
    std::uint32_t coded_width = 0;
    std::uint32_t coded_height = 0;

    /*
     * Display dimensions after conformance cropping.
     */
    std::uint32_t display_width = 0;
    std::uint32_t display_height = 0;

    /*
     * Minimum / maximum coding block sizes.
     */
    std::uint32_t min_cb_size = 0;
    std::uint32_t max_cb_size = 0;

    /*
     * Minimum / maximum transform block sizes.
     */
    std::uint32_t min_tb_size = 0;
    std::uint32_t max_tb_size = 0;
};

/*
 * -----------------------------------------------------------
 * Complete Sequence Parameter Set
 * -----------------------------------------------------------
 */

struct SequenceParameterSet {
    /*
     * =======================================================
     * Identification
     * =======================================================
     */

    /*
     * sps_video_parameter_set_id
     *
     * u(4)
     */
    std::uint8_t sps_video_parameter_set_id = 0;

    /*
     * sps_max_sub_layers_minus1
     *
     * u(3)
     */
    std::uint8_t sps_max_sub_layers_minus1 = 0;

    /*
     * sps_temporal_id_nesting_flag
     */
    bool sps_temporal_id_nesting_flag = false;

    /*
     * =======================================================
     * Profile / tier / level
     * =======================================================
     */

    ProfileTierLevel profile_tier_level{};

    /*
     * =======================================================
     * SPS identification
     * =======================================================
     */

    /*
     * sps_seq_parameter_set_id
     *
     * ue(v)
     */
    std::uint32_t sps_seq_parameter_set_id = 0;

    /*
     * =======================================================
     * Chroma format
     * =======================================================
     */

    /*
     * chroma_format_idc
     *
     * ue(v)
     */
    ChromaFormat chroma_format = ChromaFormat::YUV420;

    /*
     * separate_colour_plane_flag
     *
     * Present only when chroma_format_idc == 3.
     */
    bool separate_colour_plane_flag = false;

    /*
     * =======================================================
     * Picture dimensions
     * =======================================================
     */

    std::uint32_t pic_width_in_luma_samples = 0;

    std::uint32_t pic_height_in_luma_samples = 0;

    /*
     * conformance_window_flag
     */
    bool conformance_window_flag = false;

    Window conformance_window{};

    /*
     * =======================================================
     * Bit depth
     * =======================================================
     */

    /*
     * Raw syntax values.
     */
    std::uint32_t bit_depth_luma_minus8 = 0;

    std::uint32_t bit_depth_chroma_minus8 = 0;

    /*
     * Convenient decoded representation.
     */
    BitDepth bit_depth{};

    /*
     * =======================================================
     * Picture order count
     * =======================================================
     */

    /*
     * log2_max_pic_order_cnt_lsb_minus4
     */
    std::uint32_t log2_max_pic_order_cnt_lsb_minus4 = 0;

    /*
     * =======================================================
     * Sub-layer ordering
     * =======================================================
     */

    bool sps_sub_layer_ordering_info_present_flag = false;

    std::array<SpsSubLayerOrderingInfo, 8> sub_layer_ordering_info{};

    /*
     * =======================================================
     * Coding block configuration
     * =======================================================
     */

    SpsCodingBlockParameters coding_blocks{};

    /*
     * =======================================================
     * Transform / coding tools
     * =======================================================
     */

    /*
     * scaling_list_enabled_flag
     */
    bool scaling_list_enabled_flag = false;

    /*
     * sps_scaling_list_data_present_flag
     */
    bool sps_scaling_list_data_present_flag = false;

    ScalingListData scaling_list{};

    /*
     * amp_enabled_flag
     */
    bool amp_enabled_flag = false;

    /*
     * sample_adaptive_offset_enabled_flag
     */
    bool sample_adaptive_offset_enabled_flag = false;

    /*
     * =======================================================
     * PCM
     * =======================================================
     */

    SpsPcmParameters pcm{};

    /*
     * =======================================================
     * Short-term reference pictures
     * =======================================================
     */

    SpsReferencePictureSetParameters reference_picture_sets{};

    /*
     * =======================================================
     * Temporal MVP / intra tools
     * =======================================================
     */

    /*
     * sps_temporal_mvp_enabled_flag
     */
    bool sps_temporal_mvp_enabled_flag = false;

    /*
     * strong_intra_smoothing_enabled_flag
     */
    bool strong_intra_smoothing_enabled_flag = false;

    /*
     * =======================================================
     * VUI
     * =======================================================
     */

    /*
     * vui_parameters_present_flag
     */
    bool vui_parameters_present_flag = false;

    VuiParameters vui{};

    /*
     * =======================================================
     * Extensions
     * =======================================================
     */

    SpsExtension extension{};

    SpsRangeExtension range_extension{};

    SpsMultilayerExtension multilayer_extension{};

    Sps3dExtension three_d_extension{};

    SpsSccExtension scc_extension{};

    /*
     * =======================================================
     * Derived information
     * =======================================================
     */

    SpsGeometry geometry{};

    /*
     * =======================================================
     * Helpers
     * =======================================================
     */

    [[nodiscard]]
    constexpr std::size_t max_sub_layers() const noexcept {
        return static_cast<std::size_t>(sps_max_sub_layers_minus1) + 1;
    }

    [[nodiscard]]
    constexpr std::uint32_t max_pic_order_cnt_lsb() const noexcept {
        return std::uint32_t{1} << (log2_max_pic_order_cnt_lsb_minus4 + 4);
    }

    [[nodiscard]]
    constexpr std::uint8_t luma_bit_depth() const noexcept {
        return static_cast<std::uint8_t>(bit_depth_luma_minus8 + 8);
    }

    [[nodiscard]]
    constexpr std::uint8_t chroma_bit_depth() const noexcept {
        return static_cast<std::uint8_t>(bit_depth_chroma_minus8 + 8);
    }

    [[nodiscard]]
    constexpr bool has_scaling_list() const noexcept {
        return scaling_list_enabled_flag && sps_scaling_list_data_present_flag;
    }

    [[nodiscard]]
    constexpr bool has_vui() const noexcept {
        return vui_parameters_present_flag;
    }

    [[nodiscard]]
    constexpr bool has_short_term_rps() const noexcept {
        return reference_picture_sets.num_short_term_ref_pic_sets != 0;
    }

    [[nodiscard]]
    constexpr bool has_long_term_rps() const noexcept {
        return reference_picture_sets.long_term_ref_pics_present_flag;
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
        return sps_video_parameter_set_id < 16 && sps_max_sub_layers_minus1 < 8 &&
               sps_seq_parameter_set_id < 16 && pic_width_in_luma_samples != 0 &&
               pic_height_in_luma_samples != 0;
    }
};

/*
 * -----------------------------------------------------------
 * SPS initialization
 * -----------------------------------------------------------
 */

inline void initialize_sps(SequenceParameterSet& sps) {
    sps = {};

    sps.chroma_format = ChromaFormat::YUV420;

    sps.bit_depth = {8, 8};

    for (auto& entry : sps.sub_layer_ordering_info) {
        entry = {};
    }

    initialize_scaling_list_data(sps.scaling_list);
}

/*
 * -----------------------------------------------------------
 * Bit-depth derivation
 * -----------------------------------------------------------
 */

inline void derive_sps_bit_depth(SequenceParameterSet& sps) {
    sps.bit_depth.luma = static_cast<std::uint8_t>(sps.bit_depth_luma_minus8 + 8);

    sps.bit_depth.chroma = static_cast<std::uint8_t>(sps.bit_depth_chroma_minus8 + 8);
}

/*
 * -----------------------------------------------------------
 * Geometry derivation
 * -----------------------------------------------------------
 */

inline void derive_sps_geometry(SequenceParameterSet& sps) {
    sps.geometry.coded_width = sps.pic_width_in_luma_samples;

    sps.geometry.coded_height = sps.pic_height_in_luma_samples;

    const auto display = apply_window(
        sps.pic_width_in_luma_samples,
        sps.pic_height_in_luma_samples,
        sps.chroma_format,
        sps.conformance_window
    );

    sps.geometry.display_width = display.width;

    sps.geometry.display_height = display.height;

    sps.geometry.min_cb_size = sps.coding_blocks.min_luma_coding_block_size();

    sps.geometry.max_cb_size = sps.coding_blocks.max_luma_coding_block_size();

    sps.geometry.min_tb_size = sps.coding_blocks.min_luma_transform_block_size();

    sps.geometry.max_tb_size = sps.coding_blocks.max_luma_transform_block_size();
}

/*
 * -----------------------------------------------------------
 * RPS initialization
 * -----------------------------------------------------------
 */

inline void initialize_sps_rps(SequenceParameterSet& sps) {
    const auto count = sps.reference_picture_sets.num_short_term_ref_pic_sets;

    sps.reference_picture_sets.short_term_ref_pic_sets.clear();

    sps.reference_picture_sets.short_term_ref_pic_sets.resize(count);

    for (std::uint32_t i = 0; i < count; ++i) {
        sps.reference_picture_sets.short_term_ref_pic_sets[i].index = i;
    }
}

/*
 * -----------------------------------------------------------
 * Long-term reference picture initialization
 * -----------------------------------------------------------
 */

inline void initialize_sps_long_term_rps(SequenceParameterSet& sps) {
    auto& lt = sps.reference_picture_sets;

    if (!lt.long_term_ref_pics_present_flag) {
        lt.num_long_term_ref_pics_sps = 0;
        lt.lt_ref_pic_poc_lsb_sps.clear();
        lt.used_by_curr_pic_lt_sps_flag.clear();
        return;
    }

    lt.lt_ref_pic_poc_lsb_sps.resize(lt.num_long_term_ref_pics_sps);

    lt.used_by_curr_pic_lt_sps_flag.resize(lt.num_long_term_ref_pics_sps, false);
}

/*
 * -----------------------------------------------------------
 * SPS validation
 * -----------------------------------------------------------
 */

[[nodiscard]]
constexpr bool validate_sps_base(const SequenceParameterSet& sps) noexcept {
    if (sps.sps_video_parameter_set_id >= 16) {
        return false;
    }

    if (sps.sps_max_sub_layers_minus1 >= 8) {
        return false;
    }

    if (sps.sps_seq_parameter_set_id >= 16) {
        return false;
    }

    if (sps.pic_width_in_luma_samples == 0 || sps.pic_height_in_luma_samples == 0) {
        return false;
    }

    if (sps.bit_depth_luma_minus8 > 8 || sps.bit_depth_chroma_minus8 > 8) {
        return false;
    }

    if (sps.log2_max_pic_order_cnt_lsb_minus4 > 12) {
        return false;
    }

    return true;
}

/*
 * Validate chroma-specific SPS syntax.
 */
[[nodiscard]]
constexpr bool validate_sps_chroma(const SequenceParameterSet& sps) noexcept {
    if (!is_valid_chroma_format(static_cast<std::uint32_t>(sps.chroma_format))) {
        return false;
    }

    /*
     * separate_colour_plane_flag is only legal for
     * 4:4:4.
     */
    if (sps.separate_colour_plane_flag && sps.chroma_format != ChromaFormat::YUV444) {
        return false;
    }

    return true;
}

/*
 * Validate SPS RPS container dimensions.
 */
[[nodiscard]]
inline bool validate_sps_rps(const SequenceParameterSet& sps) noexcept {
    const auto& rps = sps.reference_picture_sets;

    if (rps.short_term_ref_pic_sets.size() != rps.num_short_term_ref_pic_sets) {
        return false;
    }

    if (rps.long_term_ref_pics_present_flag) {
        if (rps.lt_ref_pic_poc_lsb_sps.size() != rps.num_long_term_ref_pics_sps) {
            return false;
        }

        if (rps.used_by_curr_pic_lt_sps_flag.size() != rps.num_long_term_ref_pics_sps) {
            return false;
        }
    }

    return true;
}

/*
 * -----------------------------------------------------------
 * Convenience accessors
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline const ShortTermRefPicSet* sps_short_term_rps(
    const SequenceParameterSet& sps, std::size_t index
) noexcept {
    const auto& sets = sps.reference_picture_sets.short_term_ref_pic_sets;

    if (index >= sets.size()) {
        return nullptr;
    }

    return &sets[index];
}

[[nodiscard]]
inline ShortTermRefPicSet* sps_short_term_rps(
    SequenceParameterSet& sps, std::size_t index
) noexcept {
    auto& sets = sps.reference_picture_sets.short_term_ref_pic_sets;

    if (index >= sets.size()) {
        return nullptr;
    }

    return &sets[index];
}

/*
 * -----------------------------------------------------------
 * SPS constants
 * -----------------------------------------------------------
 */

inline constexpr std::uint8_t kMaxSpsSubLayersMinus1 = 7;

inline constexpr std::uint8_t kMaxSpsVideoParameterSetId = 15;

inline constexpr std::uint8_t kMaxSpsSequenceParameterSetId = 15;

}  // namespace bs