// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "avc_scaling_list.hpp"
#include "avc_common.hpp"

#include <array>
#include <cstdint>

namespace bs {
namespace avc {

/*
 * AVC HRD parameters (E.1.2).
 */
struct HrdParameters {
    std::uint8_t cpb_cnt_minus1 = 0;
    std::uint8_t bit_rate_scale = 0;
    std::uint8_t cpb_size_scale = 0;

    std::array<std::uint32_t, 32> bit_rate_value_minus1{};
    std::array<std::uint32_t, 32> cpb_size_value_minus1{};
    std::array<bool, 32> cbr_flag{};

    std::uint8_t initial_cpb_removal_delay_length_minus1 = 0;
    std::uint8_t cpb_removal_delay_length_minus1 = 0;
    std::uint8_t dpb_output_delay_length_minus1 = 0;
    std::uint8_t time_offset_length = 0;
};

/*
 * AVC VUI parameters (E.1.1).
 */
struct VuiParameters {
    bool aspect_ratio_info_present_flag = false;
    std::uint8_t aspect_ratio_idc = 0;
    std::uint16_t sar_width = 0;
    std::uint16_t sar_height = 0;

    bool overscan_info_present_flag = false;
    bool overscan_appropriate_flag = false;

    bool video_signal_type_present_flag = false;
    std::uint8_t video_format = 5;
    bool video_full_range_flag = false;
    bool colour_description_present_flag = false;
    std::uint8_t colour_primaries = 0;
    std::uint8_t transfer_characteristics = 0;
    std::uint8_t matrix_coefficients = 0;

    bool chroma_loc_info_present_flag = false;
    std::uint32_t chroma_sample_loc_type_top_field = 0;
    std::uint32_t chroma_sample_loc_type_bottom_field = 0;

    bool timing_info_present_flag = false;
    std::uint32_t num_units_in_tick = 0;
    std::uint32_t time_scale = 0;
    bool fixed_frame_rate_flag = false;

    bool nal_hrd_parameters_present_flag = false;
    HrdParameters nal_hrd{};

    bool vcl_hrd_parameters_present_flag = false;
    HrdParameters vcl_hrd{};

    bool low_delay_hrd_flag = false;

    bool pic_struct_present_flag = false;

    bool bitstream_restriction_flag = false;
    bool motion_vectors_over_pic_boundaries_flag = false;
    std::uint32_t max_bytes_per_pic_denom = 0;
    std::uint32_t max_bits_per_mb_denom = 0;
    std::uint32_t log2_max_mv_length_horizontal = 0;
    std::uint32_t log2_max_mv_length_vertical = 0;
    std::uint32_t max_num_reorder_frames = 0;
    std::uint32_t max_dec_frame_buffering = 0;
};

}  // namespace avc
}  // namespace bs