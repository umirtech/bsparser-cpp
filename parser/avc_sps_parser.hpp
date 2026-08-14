#pragma once

#include "avc_parse_common.hpp"
#include "avc_sps.hpp"
#include "avc_vui.hpp"
#include "rbsp_bitstream_reader.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace bs {
namespace avc {

/*
 * -----------------------------------------------------------
 * AVC sequence parameter set parser (7.3.2.1.1)
 * -----------------------------------------------------------
 *
 * Reuses bs::RbspBitstreamReader for bit-level access,
 * Exp-Golomb decoding and emulation-prevention handling.
 */

namespace detail {

/*
 * Default scaling matrices (Annex A.2.5.1).
 */
inline constexpr std::uint8_t kDefault4x4Intra[16] = {
    6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 37, 37, 42
};

inline constexpr std::uint8_t kDefault4x4Inter[16] = {
    10, 14, 14, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 37, 37, 42
};

inline constexpr std::uint8_t kDefault8x8Intra[64] = {
    6,  10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18, 18, 18, 18, 23, 23, 23, 23, 23, 23, 25,
    25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25,
    25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25
};

inline constexpr std::uint8_t kDefault8x8Inter[64] = {
    9,  13, 13, 16, 13, 16, 22, 22, 22, 22, 25, 25, 25, 25, 25, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29
};

/*
 * AVC scaling_list() (7.3.2.1.1.1).
 *
 * The list is decoded differentially from the previous matrix
 * (or a default matrix), matching the H.264 semantics.
 */
inline void parse_scaling_list(
    RbspBitstreamReader& reader,
    std::uint8_t size_of_scaling_list,
    std::uint8_t matrix_id,
    std::uint8_t* list_out
) {
    bool use_default = false;

    if (matrix_id < 6) {
        const auto& default_matrix = (matrix_id < 3) ? kDefault4x4Intra : kDefault4x4Inter;

        use_default = reader.read_bit();

        if (use_default) {
            for (unsigned j = 0; j < 16; ++j) {
                list_out[j] = default_matrix[j];
            }
            return;
        }

    } else {
        /*
         * matrix_id 6 (intra 8x8) / 7 (inter 8x8).
         *
         * Predicted from the previously parsed 8x8 matrix
         * when scaling_list_pred_matrix_id_delta != 0.
         * This parser processes matrices in order, so the
         * previous 8x8 matrix is the one at index
         * (matrix_id - 1).
         */
        const auto& default_matrix = (matrix_id == 6) ? kDefault8x8Intra : kDefault8x8Inter;

        use_default = reader.read_bit();

        if (use_default) {
            for (unsigned j = 0; j < 64; ++j) {
                list_out[j] = default_matrix[j];
            }
            return;
        }
    }

    /*
     * Differential coding from nextCoef.
     */
    std::int32_t next_coef = 8;

    for (unsigned j = 0; j < size_of_scaling_list; ++j) {
        std::int32_t delta_scale = 0;

        if (next_coef != 0) {
            delta_scale = read_se_bounded(reader, "delta_scale", 255);
        }

        next_coef = (next_coef + delta_scale + 256) % 256;

        list_out[j] = static_cast<std::uint8_t>(next_coef);
    }
}

/*
 * AVC hrd_parameters() (E.1.2).
 */
inline HrdParameters parse_hrd_parameters(RbspBitstreamReader& reader) {
    HrdParameters hrd;

    hrd.cpb_cnt_minus1 = static_cast<std::uint8_t>(read_ue_max(reader, "cpb_cnt_minus1", 31));

    hrd.bit_rate_scale = static_cast<std::uint8_t>(reader.read_bits(4));

    hrd.cpb_size_scale = static_cast<std::uint8_t>(reader.read_bits(4));

    for (unsigned i = 0; i <= hrd.cpb_cnt_minus1; ++i) {
        hrd.bit_rate_value_minus1[i] = read_ue_max(reader, "bit_rate_value_minus1", 0xFFFFFFFFu);

        hrd.cpb_size_value_minus1[i] = read_ue_max(reader, "cpb_size_value_minus1", 0xFFFFFFFFu);

        hrd.cbr_flag[i] = reader.read_bit();
    }

    hrd.initial_cpb_removal_delay_length_minus1 = static_cast<std::uint8_t>(reader.read_bits(5));

    hrd.cpb_removal_delay_length_minus1 = static_cast<std::uint8_t>(reader.read_bits(5));

    hrd.dpb_output_delay_length_minus1 = static_cast<std::uint8_t>(reader.read_bits(5));

    hrd.time_offset_length = static_cast<std::uint8_t>(reader.read_bits(5));

    return hrd;
}

/*
 * AVC vui_parameters() (E.1.1).
 */
inline VuiParameters parse_vui_parameters(RbspBitstreamReader& reader) {
    VuiParameters vui;

    vui.aspect_ratio_info_present_flag = reader.read_bit();

    if (vui.aspect_ratio_info_present_flag) {
        vui.aspect_ratio_idc = static_cast<std::uint8_t>(reader.read_bits(8));

        if (vui.aspect_ratio_idc == 255) {  // Extended_SAR

            vui.sar_width = static_cast<std::uint16_t>(reader.read_bits(16));

            vui.sar_height = static_cast<std::uint16_t>(reader.read_bits(16));
        }
    }

    vui.overscan_info_present_flag = reader.read_bit();

    if (vui.overscan_info_present_flag) {
        vui.overscan_appropriate_flag = reader.read_bit();
    }

    vui.video_signal_type_present_flag = reader.read_bit();

    if (vui.video_signal_type_present_flag) {
        vui.video_format = static_cast<std::uint8_t>(reader.read_bits(3));

        vui.video_full_range_flag = reader.read_bit();

        vui.colour_description_present_flag = reader.read_bit();

        if (vui.colour_description_present_flag) {
            vui.colour_primaries = static_cast<std::uint8_t>(reader.read_bits(8));

            vui.transfer_characteristics = static_cast<std::uint8_t>(reader.read_bits(8));

            vui.matrix_coefficients = static_cast<std::uint8_t>(reader.read_bits(8));
        }
    }

    vui.chroma_loc_info_present_flag = reader.read_bit();

    if (vui.chroma_loc_info_present_flag) {
        vui.chroma_sample_loc_type_top_field =
            read_ue_max(reader, "chroma_sample_loc_type_top_field", 5);

        vui.chroma_sample_loc_type_bottom_field =
            read_ue_max(reader, "chroma_sample_loc_type_bottom_field", 5);
    }

    vui.timing_info_present_flag = reader.read_bit();

    if (vui.timing_info_present_flag) {
        vui.num_units_in_tick = static_cast<std::uint32_t>(reader.read_bits(32));

        vui.time_scale = static_cast<std::uint32_t>(reader.read_bits(32));

        vui.fixed_frame_rate_flag = reader.read_bit();
    }

    vui.nal_hrd_parameters_present_flag = reader.read_bit();

    if (vui.nal_hrd_parameters_present_flag) {
        vui.nal_hrd = parse_hrd_parameters(reader);
    }

    vui.vcl_hrd_parameters_present_flag = reader.read_bit();

    if (vui.vcl_hrd_parameters_present_flag) {
        vui.vcl_hrd = parse_hrd_parameters(reader);
    }

    if (vui.nal_hrd_parameters_present_flag || vui.vcl_hrd_parameters_present_flag) {
        vui.low_delay_hrd_flag = reader.read_bit();
    }

    vui.pic_struct_present_flag = reader.read_bit();

    vui.bitstream_restriction_flag = reader.read_bit();

    if (vui.bitstream_restriction_flag) {
        vui.motion_vectors_over_pic_boundaries_flag = reader.read_bit();

        vui.max_bytes_per_pic_denom = read_ue_max(reader, "max_bytes_per_pic_denom", 16);

        vui.max_bits_per_mb_denom = read_ue_max(reader, "max_bits_per_mb_denom", 16);

        vui.log2_max_mv_length_horizontal =
            read_ue_max(reader, "log2_max_mv_length_horizontal", 16);

        vui.log2_max_mv_length_vertical = read_ue_max(reader, "log2_max_mv_length_vertical", 16);

        vui.max_num_reorder_frames = read_ue_max(reader, "max_num_reorder_frames", 16);

        vui.max_dec_frame_buffering = read_ue_max(reader, "max_dec_frame_buffering", 16);
    }

    return vui;
}

}  // namespace detail

/*
 * -----------------------------------------------------------
 * Parse a complete AVC SPS RBSP
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline SequenceParameterSet parse_sequence_parameter_set(RbspBitstreamReader& reader) {
    SequenceParameterSet sps;

    sps.profile_idc = reader.read_u8();

    for (unsigned i = 0; i < 6; ++i) {
        sps.constraint_set_flag[i] = reader.read_bit();
    }

    // reserved_zero_2bits
    (void)reader.read_bits(2);

    sps.level_idc = reader.read_u8();

    sps.seq_parameter_set_id =
        static_cast<std::uint8_t>(detail::read_ue_max(reader, "seq_parameter_set_id", 31));

    if (is_high_profile(sps.profile_idc)) {
        sps.chroma_format_idc =
            static_cast<std::uint8_t>(detail::read_ue_max(reader, "chroma_format_idc", 3));

        if (sps.chroma_format_idc == 3) {
            sps.separate_colour_plane_flag = reader.read_bit();
        }

        sps.bit_depth_luma_minus8 =
            static_cast<std::uint8_t>(detail::read_ue_max(reader, "bit_depth_luma_minus8", 6));

        sps.bit_depth_chroma_minus8 =
            static_cast<std::uint8_t>(detail::read_ue_max(reader, "bit_depth_chroma_minus8", 6));

        sps.qpprime_y_zero_transform_bypass_flag = reader.read_bit();

        const bool seq_scaling_matrix_present_flag = reader.read_bit();

        if (seq_scaling_matrix_present_flag) {
            for (unsigned i = 0; i < 8; ++i) {
                const bool present = reader.read_bit();

                if (!present) {
                    continue;
                }

                if (i < 6) {
                    sps.scaling_lists.present_4x4[i] = true;
                    detail::parse_scaling_list(
                        reader,
                        16,
                        static_cast<std::uint8_t>(i),
                        sps.scaling_lists.list_4x4[i].data()
                    );
                } else {
                    const unsigned idx = i - 6;
                    sps.scaling_lists.present_8x8[idx] = true;
                    detail::parse_scaling_list(
                        reader,
                        64,
                        static_cast<std::uint8_t>(i),
                        sps.scaling_lists.list_8x8[idx].data()
                    );
                }
            }
        }
    }

    sps.log2_max_frame_num_minus4 =
        static_cast<std::uint8_t>(detail::read_ue_max(reader, "log2_max_frame_num_minus4", 12));

    sps.pic_order_cnt_type =
        static_cast<std::uint8_t>(detail::read_ue_max(reader, "pic_order_cnt_type", 2));

    if (sps.pic_order_cnt_type == 0) {
        sps.log2_max_pic_order_cnt_lsb_minus4 = static_cast<std::uint8_t>(
            detail::read_ue_max(reader, "log2_max_pic_order_cnt_lsb_minus4", 12)
        );

    } else if (sps.pic_order_cnt_type == 1) {
        sps.delta_pic_order_always_zero_flag = reader.read_bit();

        sps.offset_for_non_ref_pic =
            detail::read_se_bounded(reader, "offset_for_non_ref_pic", 0x7FFFFFFF);

        sps.offset_for_top_to_bottom_field =
            detail::read_se_bounded(reader, "offset_for_top_to_bottom_field", 0x7FFFFFFF);

        sps.num_ref_frames_in_pic_order_cnt_cycle = static_cast<std::uint8_t>(
            detail::read_ue_max(reader, "num_ref_frames_in_pic_order_cnt_cycle", 255)
        );

        for (unsigned i = 0; i < sps.num_ref_frames_in_pic_order_cnt_cycle; ++i) {
            sps.offset_for_ref_frame[i] =
                detail::read_se_bounded(reader, "offset_for_ref_frame", 0x7FFFFFFF);
        }
    }

    sps.max_num_ref_frames =
        static_cast<std::uint8_t>(detail::read_ue_max(reader, "max_num_ref_frames", 255));

    sps.gaps_in_frame_num_value_allowed_flag = reader.read_bit();

    sps.pic_width_in_mbs_minus1 = detail::read_ue_max(reader, "pic_width_in_mbs_minus1", 0xFFFFFu);

    sps.pic_height_in_map_units_minus1 =
        detail::read_ue_max(reader, "pic_height_in_map_units_minus1", 0xFFFFFu);

    sps.frame_mbs_only_flag = reader.read_bit();

    if (!sps.frame_mbs_only_flag) {
        sps.mb_adaptive_frame_field_flag = reader.read_bit();
    }

    sps.direct_8x8_inference_flag = reader.read_bit();

    sps.frame_cropping_flag = reader.read_bit();

    if (sps.frame_cropping_flag) {
        sps.frame_crop_left_offset =
            detail::read_ue_max(reader, "frame_crop_left_offset", 0xFFFFFu);

        sps.frame_crop_right_offset =
            detail::read_ue_max(reader, "frame_crop_right_offset", 0xFFFFFu);

        sps.frame_crop_top_offset = detail::read_ue_max(reader, "frame_crop_top_offset", 0xFFFFFu);

        sps.frame_crop_bottom_offset =
            detail::read_ue_max(reader, "frame_crop_bottom_offset", 0xFFFFFu);
    }

    sps.vui_parameters_present_flag = reader.read_bit();

    if (sps.vui_parameters_present_flag) {
        sps.vui = detail::parse_vui_parameters(reader);
    }

    reader.read_rbsp_trailing_bits();

    return sps;
}

/*
 * -----------------------------------------------------------
 * Try variant + validation
 * -----------------------------------------------------------
 */

struct AvcSpsParseResult {
    SequenceParameterSet sps{};
    bool ok = false;
};

[[nodiscard]]
inline AvcSpsParseResult try_parse_sequence_parameter_set(RbspBitstreamReader& reader) {
    try {
        return AvcSpsParseResult{parse_sequence_parameter_set(reader), true};
    } catch (const std::exception&) {
        return AvcSpsParseResult{};
    }
}

[[nodiscard]]
inline bool validate_sequence_parameter_set(const SequenceParameterSet& sps) noexcept {
    if (sps.seq_parameter_set_id >= kMaxSpsCount) {
        return false;
    }

    if (sps.pic_order_cnt_type > 2) {
        return false;
    }

    if (sps.log2_max_frame_num() > 16) {
        return false;
    }

    if (sps.pic_order_cnt_type == 0 && sps.log2_max_pic_order_cnt_lsb() > 16) {
        return false;
    }

    return true;
}

}  // namespace avc
}  // namespace bs