// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <array>
#include <cstdint>

namespace bs {

/*
 * H.265 chroma_format_idc.
 *
 * 0 = monochrome
 * 1 = 4:2:0
 * 2 = 4:2:2
 * 3 = 4:4:4
 */
enum class ChromaFormat : std::uint8_t { Monochrome = 0, YUV420 = 1, YUV422 = 2, YUV444 = 3 };

/*
 * H.265 screen-content-coding extension limit.
 *
 * A.3.7 constrains the number of palette predictor
 * initializers to 128 (HEVC_MAX_PALETTE_PREDICTOR_SIZE).
 */
inline constexpr std::size_t kMaxPalettePredictorSize = 128;

[[nodiscard]]
constexpr bool is_valid_chroma_format(std::uint32_t value) noexcept {
    return value <= 3;
}

[[nodiscard]]
constexpr ChromaFormat chroma_format_from_idc(std::uint32_t value) {
    return static_cast<ChromaFormat>(value);
}

/*
 * H.265 bit depth.
 *
 * The actual syntax stores:
 *
 *     bit_depth_luma_minus8
 *     bit_depth_chroma_minus8
 *
 * Keeping the decoded values as actual bit depths makes
 * downstream code considerably easier to read.
 */
struct BitDepth {
    std::uint8_t luma = 8;
    std::uint8_t chroma = 8;

    [[nodiscard]]
    constexpr bool valid() const noexcept {
        return luma >= 8 && chroma >= 8;
    }
};

/*
 * H.265 conformance_window / default_display_window.
 *
 * The syntax uses offsets rather than actual pixel
 * coordinates.
 *
 * Actual dimensions depend on chroma_format_idc.
 */
struct Window {
    std::uint32_t left_offset = 0;
    std::uint32_t right_offset = 0;
    std::uint32_t top_offset = 0;
    std::uint32_t bottom_offset = 0;

    [[nodiscard]]
    constexpr bool empty() const noexcept {
        return left_offset == 0 && right_offset == 0 && top_offset == 0 && bottom_offset == 0;
    }
};

/*
 * H.265 sub_layer_ordering_info().
 *
 * These values control decoded picture buffer and reorder
 * limits for a temporal sub-layer.
 */
struct SubLayerOrderingInfo {
    std::uint32_t max_dec_pic_buffering_minus1 = 0;
    std::uint32_t max_num_reorder_pics = 0;
    std::uint32_t max_latency_increase_plus1 = 0;
};

/*
 * H.265 scaling list identifiers.
 *
 * scaling_list_pred_mode_flag and related syntax are
 * represented in hevc_scaling_list.hpp, but the dimensions
 * are useful throughout the parser.
 */
enum class ScalingListSize : std::uint8_t {
    Size4x4 = 0,
    Size8x8 = 1,
    Size16x16 = 2,
    Size32x32 = 3
};

/*
 * H.265 slice types.
 *
 * slice_type:
 *
 *     0 = B
 *     1 = P
 *     2 = I
 */
enum class SliceType : std::uint8_t { B = 0, P = 1, I = 2 };

[[nodiscard]]
constexpr bool is_valid_slice_type(std::uint32_t value) noexcept {
    return value <= 2;
}

/*
 * H.265 reference picture list.
 *
 * Used later by slice header parsing.
 */
enum class RefPicList : std::uint8_t { L0 = 0, L1 = 1 };

/*
 * Picture structure / field information.
 *
 * H.265 uses flags in several places to distinguish
 * progressive/field coding. Keep the representation
 * explicit instead of exposing raw syntax bits everywhere.
 */
struct PictureStructure {
    bool field_seq = false;
    bool bottom_field = false;
};

/*
 * H.265 VUI timing information.
 *
 * This is kept here because it is useful to both VUI and
 * HRD-related structures.
 */
struct TimingInfo {
    std::uint32_t num_units_in_tick = 0;
    std::uint32_t time_scale = 0;

    bool poc_proportional_to_timing_flag = false;

    std::uint32_t num_ticks_poc_diff_one_minus1 = 0;

    [[nodiscard]]
    constexpr bool valid() const noexcept {
        return num_units_in_tick != 0 && time_scale != 0;
    }
};

/*
 * H.265 color description.
 *
 * Values correspond directly to the H.265 syntax values.
 */
struct ColourDescription {
    std::uint8_t colour_primaries = 2;
    std::uint8_t transfer_characteristics = 2;
    std::uint8_t matrix_coefficients = 2;

    bool present = false;
};

/*
 * H.265 aspect ratio information.
 *
 * aspect_ratio_idc values 1..16 are predefined.
 *
 * Extended_SAR uses:
 *
 *     sar_width
 *     sar_height
 */
struct AspectRatio {
    bool present = false;

    std::uint8_t aspect_ratio_idc = 0;

    std::uint16_t sar_width = 0;
    std::uint16_t sar_height = 0;
};

/*
 * H.265 video signal type.
 */
struct VideoSignalType {
    bool present = false;

    std::uint8_t video_format = 5;

    bool video_full_range_flag = false;

    ColourDescription colour;
};

/*
 * H.265 chroma location information.
 */
struct ChromaLocationInfo {
    bool present = false;

    std::uint32_t chroma_sample_loc_type_top_field = 0;
    std::uint32_t chroma_sample_loc_type_bottom_field = 0;
};

/*
 * H.265 decoded picture dimension information.
 *
 * This is derived from SPS syntax.
 *
 * pic_width_in_luma_samples
 * pic_height_in_luma_samples
 *
 * are retained as coded dimensions.
 */
struct PictureDimensions {
    std::uint32_t width_luma_samples = 0;
    std::uint32_t height_luma_samples = 0;

    Window conformance_window{};

    bool conformance_window_flag = false;

    [[nodiscard]]
    constexpr std::uint32_t coded_width() const noexcept {
        return width_luma_samples;
    }

    [[nodiscard]]
    constexpr std::uint32_t coded_height() const noexcept {
        return height_luma_samples;
    }
};

/*
 * Useful derived geometry.
 *
 * This is NOT directly an H.265 syntax structure.
 *
 * It belongs in the semantic layer and can be populated
 * after SPS parsing.
 */
struct CodingBlockGeometry {
    std::uint32_t min_cb_size = 0;
    std::uint32_t max_cb_size = 0;

    std::uint32_t min_tb_size = 0;
    std::uint32_t max_tb_size = 0;

    std::uint8_t max_transform_hierarchy_depth_inter = 0;
    std::uint8_t max_transform_hierarchy_depth_intra = 0;
};

/*
 * H.265 reference picture set identifiers.
 *
 * This is intentionally small for now. The complete
 * short_term_ref_pic_set syntax will live in:
 *
 *     hevc_short_term_ref_pic_set.hpp
 */
struct ReferencePictureSetInfo {
    std::uint32_t num_negative_pics = 0;
    std::uint32_t num_positive_pics = 0;
    std::uint32_t num_delta_pocs = 0;
};

/*
 * Generic flag collection used by profile/constraint
 * structures.
 */
using ProfileCompatibilityFlags = std::array<bool, 32>;

/*
 * H.265 profile space.
 *
 * general_profile_space:
 *
 *     0 = unspecified
 *     1 = profile space 1
 *     2 = profile space 2
 *     3 = profile space 3
 */
enum class ProfileSpace : std::uint8_t { Unspecified = 0, Space1 = 1, Space2 = 2, Space3 = 3 };

/*
 * H.265 tier.
 */
enum class Tier : std::uint8_t { Main = 0, High = 1 };

/*
 * Generic H.265 level.
 *
 * level_idc is stored exactly as signaled.
 *
 * Examples:
 *
 *     30 = Level 3.0
 *     31 = Level 3.1
 *     40 = Level 4.0
 *     41 = Level 4.1
 *     50 = Level 5.0
 *     ...
 */
struct Level {
    std::uint8_t level_idc = 0;

    [[nodiscard]]
    constexpr float value() const noexcept {
        return static_cast<float>(level_idc) / 30.0F;
    }

    [[nodiscard]]
    constexpr bool valid() const noexcept {
        return level_idc != 0;
    }
};

/*
 * H.265 profile/tier/level identity.
 *
 * The complete syntax lives in hevc_profile_tier_level.hpp.
 */
struct ProfileTierLevelId {
    ProfileSpace profile_space = ProfileSpace::Unspecified;

    Tier tier = Tier::Main;

    std::uint8_t profile_idc = 0;

    ProfileCompatibilityFlags compatibility_flags{};

    Level level{};
};

/*
 * H.265 temporal layer description.
 *
 * Used by VPS/SPS and profile_tier_level().
 */
struct TemporalLayer {
    std::uint8_t id = 0;

    bool profile_present = false;
    bool level_present = false;
};

/*
 * H.265 POC configuration.
 *
 * This is derived from:
 *
 *     log2_max_pic_order_cnt_lsb_minus4
 */
struct PocConfiguration {
    std::uint8_t log2_max_pic_order_cnt_lsb_minus4 = 0;

    [[nodiscard]]
    constexpr std::uint32_t max_pic_order_cnt_lsb() const noexcept {
        return std::uint32_t{1} << (log2_max_pic_order_cnt_lsb_minus4 + 4);
    }
};

/*
 * H.265 sample aspect ratio.
 */
struct SampleAspectRatio {
    std::uint16_t width = 1;
    std::uint16_t height = 1;

    [[nodiscard]]
    constexpr bool valid() const noexcept {
        return width != 0 && height != 0;
    }
};

/*
 * Utility for safely converting an unsigned syntax value
 * to uint8_t.
 *
 * Parser code can use this after validating the syntax
 * range.
 */
[[nodiscard]]
constexpr std::uint8_t to_u8(std::uint32_t value) noexcept {
    return static_cast<std::uint8_t>(value);
}

/*
 * Utility for safely converting an unsigned syntax value
 * to uint16_t.
 */
[[nodiscard]]
constexpr std::uint16_t to_u16(std::uint32_t value) noexcept {
    return static_cast<std::uint16_t>(value);
}

}  // namespace bs