#pragma once

#include "av1_sequence_header.hpp"

#include <bitstream/boolean_decoder.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace bs {
namespace av1 {

/*
 * -----------------------------------------------------------
 * AV1 sequence header parser (AV1 §5.5, Annex A)
 * -----------------------------------------------------------
 * Full syntax per AV1 spec: timing, decoder model, operating
 * points, dimensions, frame-id, feature flags, superres/cdef/
 * restoration, color_config, film grain.  Mirrors ffmpeg
 * cbs_av1 sequence_header_obu.
 */

namespace detail_seq {

inline void parse_color_config(BooleanDecoder& bd, SequenceHeader& sh) {
    sh.color_config.high_bitdepth = bd.read_bool(128);
    if (sh.seq_profile == 2 && sh.color_config.high_bitdepth) {
        sh.color_config.twelve_bit = bd.read_bool(128);
    } else {
        sh.color_config.twelve_bit = false;
    }
    const std::uint8_t bit_depth =
        sh.color_config.twelve_bit ? 12 : (sh.color_config.high_bitdepth ? 10 : 8);

    if (sh.seq_profile == 1) {
        sh.color_config.mono_chrome = false;
    } else {
        sh.color_config.mono_chrome = bd.read_bool(128);
    }

    sh.color_config.color_description_present_flag = bd.read_bool(128);
    if (sh.color_config.color_description_present_flag) {
        sh.color_config.color_primaries = static_cast<std::uint8_t>(bd.read_literal(8));
        sh.color_config.transfer_characteristics = static_cast<std::uint8_t>(bd.read_literal(8));
        sh.color_config.matrix_coefficients = static_cast<std::uint8_t>(bd.read_literal(8));
    } else {
        sh.color_config.color_primaries = 0;
        sh.color_config.transfer_characteristics = 0;
        sh.color_config.matrix_coefficients = 0;
    }

    if (sh.color_config.mono_chrome) {
        sh.color_config.color_range = bd.read_bool(128);
        sh.color_config.subsampling_x = 1;
        sh.color_config.subsampling_y = 1;
        sh.color_config.chroma_sample_position = 0;
        sh.color_config.separate_uv_delta_q = false;
        (void)bit_depth;
    } else if (sh.color_config.color_description_present_flag &&
               sh.color_config.color_primaries == 1 &&
               sh.color_config.transfer_characteristics == 13 &&
               sh.color_config.matrix_coefficients == 0) {
        // RGB
        sh.color_config.color_range = true;
        sh.color_config.subsampling_x = 0;
        sh.color_config.subsampling_y = 0;
        sh.color_config.chroma_sample_position = 0;
        sh.color_config.separate_uv_delta_q = bd.read_bool(128);
    } else {
        sh.color_config.color_range = bd.read_bool(128);
        if (sh.seq_profile == 0) {
            sh.color_config.subsampling_x = 1;
            sh.color_config.subsampling_y = 1;
        } else if (sh.seq_profile == 1) {
            sh.color_config.subsampling_x = 0;
            sh.color_config.subsampling_y = 0;
        } else {
            if (bit_depth == 12) {
                sh.color_config.subsampling_x = bd.read_bool(128) ? 1 : 0;
                if (sh.color_config.subsampling_x) {
                    sh.color_config.subsampling_y = bd.read_bool(128) ? 1 : 0;
                } else {
                    sh.color_config.subsampling_y = 0;
                }
            } else {
                sh.color_config.subsampling_x = 1;
                sh.color_config.subsampling_y = 0;
            }
        }
        if (sh.color_config.subsampling_x && sh.color_config.subsampling_y) {
            sh.color_config.chroma_sample_position = static_cast<std::uint8_t>(bd.read_literal(2));
        } else {
            sh.color_config.chroma_sample_position = 0;
        }
        sh.color_config.separate_uv_delta_q = bd.read_bool(128);
    }
}

}  // namespace detail_seq

[[nodiscard]]
inline SequenceHeader parse_sequence_header(std::span<const std::uint8_t> payload) {
    BooleanDecoder bd{payload};

    SequenceHeader sh;

    sh.seq_profile = static_cast<std::uint8_t>(bd.read_literal(3));
    sh.still_picture = bd.read_bool(128);
    sh.reduced_still_picture_header = bd.read_bool(128);

    if (sh.reduced_still_picture_header) {
        // inferred timing
        sh.timing_info_present_flag = false;
        sh.decoder_model_info_present_flag = false;
        sh.initial_display_delay_present_flag = false;
        sh.operating_points_cnt_minus_1 = 0;
        sh.operating_point_idc[0] = 0;

        sh.seq_level_idx[0] = static_cast<std::uint8_t>(bd.read_literal(5));
        sh.seq_level_idx_0 = sh.seq_level_idx[0];
        if (sh.seq_level_idx[0] > 7) {
            sh.seq_tier[0] = bd.read_bool(128);
            sh.seq_tier_0 = sh.seq_tier[0];
        } else {
            sh.seq_tier[0] = false;
            sh.seq_tier_0 = false;
        }
        // decoder_model_present_for_this_op inferred 0
        sh.decoder_model_present_for_this_op[0] = false;
        sh.initial_display_delay_present_for_this_op[0] = false;

        sh.frame_width_bits_minus_1 = static_cast<std::uint8_t>(bd.read_literal(4));
        sh.frame_height_bits_minus_1 = static_cast<std::uint8_t>(bd.read_literal(4));
        sh.max_frame_width = bd.read_literal(sh.frame_width_bits_minus_1 + 1) + 1;
        sh.max_frame_height = bd.read_literal(sh.frame_height_bits_minus_1 + 1) + 1;
        sh.dimensions_present = true;

        sh.frame_id_numbers_present_flag = false;

        sh.use_128x128_superblock = bd.read_bool(128);
        sh.enable_filter_intra = bd.read_bool(128);
        sh.enable_intra_edge_filter = bd.read_bool(128);

        // inferred
        sh.enable_interintra_compound = false;
        sh.enable_masked_compound = false;
        sh.enable_warped_motion = false;
        sh.enable_dual_filter = false;
        sh.enable_order_hint = false;
        sh.enable_jnt_comp = false;
        sh.enable_ref_frame_mvs = false;
        sh.seq_choose_screen_content_tools = true;
        sh.seq_force_screen_content_tools = 2;
        sh.seq_choose_integer_mv = true;
        sh.seq_force_integer_mv = 2;
        sh.order_hint_bits_minus_1 = 0;

        // legacy aliases
        sh.equal_picture_interval = false;
        sh.buffer_delay_length_minus_1 = 0;
        sh.buffer_removal_time_length_minus_1 = 0;
        sh.frame_presentation_time_length_minus_1 = 0;
    } else {
        // timing + decoder model
        sh.timing_info_present_flag = bd.read_bool(128);
        if (sh.timing_info_present_flag) {
            sh.timing_info.num_units_in_display_tick = bd.read_literal(32);
            sh.timing_info.time_scale = bd.read_literal(32);
            sh.timing_info.equal_picture_interval = bd.read_bool(128);
            sh.equal_picture_interval = sh.timing_info.equal_picture_interval;
            if (sh.timing_info.equal_picture_interval) {
                sh.timing_info.num_ticks_per_picture_minus_1 = bd.read_uvlc();
            }
            sh.decoder_model_info_present_flag = bd.read_bool(128);
            if (sh.decoder_model_info_present_flag) {
                sh.decoder_model_info.buffer_delay_length_minus_1 =
                    static_cast<std::uint8_t>(bd.read_literal(5));
                sh.buffer_delay_length_minus_1 = sh.decoder_model_info.buffer_delay_length_minus_1;
                sh.decoder_model_info.num_units_in_decoding_tick = bd.read_literal(32);
                sh.decoder_model_info.buffer_removal_time_length_minus_1 =
                    static_cast<std::uint8_t>(bd.read_literal(5));
                sh.buffer_removal_time_length_minus_1 =
                    sh.decoder_model_info.buffer_removal_time_length_minus_1;
                sh.decoder_model_info.frame_presentation_time_length_minus_1 =
                    static_cast<std::uint8_t>(bd.read_literal(5));
                sh.frame_presentation_time_length_minus_1 =
                    sh.decoder_model_info.frame_presentation_time_length_minus_1;
            } else {
                sh.buffer_delay_length_minus_1 = 0;
                sh.buffer_removal_time_length_minus_1 = 0;
                sh.frame_presentation_time_length_minus_1 = 0;
            }
        } else {
            sh.decoder_model_info_present_flag = false;
            sh.equal_picture_interval = false;
            sh.buffer_delay_length_minus_1 = 0;
            sh.buffer_removal_time_length_minus_1 = 0;
            sh.frame_presentation_time_length_minus_1 = 0;
        }

        sh.initial_display_delay_present_flag = bd.read_bool(128);
        sh.operating_points_cnt_minus_1 = static_cast<std::uint8_t>(bd.read_literal(5));
        for (std::uint8_t i = 0; i <= sh.operating_points_cnt_minus_1; ++i) {
            sh.operating_point_idc[i] = static_cast<std::uint16_t>(bd.read_literal(12));
            sh.seq_level_idx[i] = static_cast<std::uint8_t>(bd.read_literal(5));
            if (sh.seq_level_idx[i] > 7) {
                sh.seq_tier[i] = bd.read_bool(128);
            } else {
                sh.seq_tier[i] = false;
            }
            if (sh.decoder_model_info_present_flag) {
                sh.decoder_model_present_for_this_op[i] = bd.read_bool(128);
                if (sh.decoder_model_present_for_this_op[i]) {
                    const std::uint8_t n =
                        static_cast<std::uint8_t>(sh.buffer_delay_length_minus_1 + 1);
                    sh.decoder_buffer_delay[i] = bd.read_literal(n);
                    sh.encoder_buffer_delay[i] = bd.read_literal(n);
                    sh.low_delay_mode_flag[i] = bd.read_bool(128);
                }
            } else {
                sh.decoder_model_present_for_this_op[i] = false;
            }
            if (sh.initial_display_delay_present_flag) {
                sh.initial_display_delay_present_for_this_op[i] = bd.read_bool(128);
                if (sh.initial_display_delay_present_for_this_op[i]) {
                    sh.initial_display_delay_minus_1[i] =
                        static_cast<std::uint8_t>(bd.read_literal(4));
                }
            } else {
                sh.initial_display_delay_present_for_this_op[i] = false;
            }
        }

        sh.frame_width_bits_minus_1 = static_cast<std::uint8_t>(bd.read_literal(4));
        sh.frame_height_bits_minus_1 = static_cast<std::uint8_t>(bd.read_literal(4));
        sh.max_frame_width = bd.read_literal(sh.frame_width_bits_minus_1 + 1) + 1;
        sh.max_frame_height = bd.read_literal(sh.frame_height_bits_minus_1 + 1) + 1;
        sh.dimensions_present = true;

        sh.frame_id_numbers_present_flag = bd.read_bool(128);
        if (sh.frame_id_numbers_present_flag) {
            sh.delta_frame_id_length_minus_2 = static_cast<std::uint8_t>(bd.read_literal(4));
            sh.additional_frame_id_length_minus_1 = static_cast<std::uint8_t>(bd.read_literal(3));
        }

        sh.use_128x128_superblock = bd.read_bool(128);
        sh.enable_filter_intra = bd.read_bool(128);
        sh.enable_intra_edge_filter = bd.read_bool(128);

        sh.enable_interintra_compound = bd.read_bool(128);
        sh.enable_masked_compound = bd.read_bool(128);
        sh.enable_warped_motion = bd.read_bool(128);
        sh.enable_dual_filter = bd.read_bool(128);

        sh.enable_order_hint = bd.read_bool(128);
        if (sh.enable_order_hint) {
            sh.enable_jnt_comp = bd.read_bool(128);
            sh.enable_ref_frame_mvs = bd.read_bool(128);
        } else {
            sh.enable_jnt_comp = false;
            sh.enable_ref_frame_mvs = false;
        }

        sh.seq_choose_screen_content_tools = bd.read_bool(128);
        if (sh.seq_choose_screen_content_tools) {
            sh.seq_force_screen_content_tools = 2;
        } else {
            sh.seq_force_screen_content_tools =
                static_cast<std::uint8_t>(bd.read_bool(128) ? 1u : 0u);
        }
        if (sh.seq_force_screen_content_tools > 0) {
            sh.seq_choose_integer_mv = bd.read_bool(128);
            if (sh.seq_choose_integer_mv) {
                sh.seq_force_integer_mv = 2;
            } else {
                sh.seq_force_integer_mv = static_cast<std::uint8_t>(bd.read_bool(128) ? 1u : 0u);
            }
        } else {
            sh.seq_choose_integer_mv = false;
            sh.seq_force_integer_mv = 2;  // SELECT per spec when screen tools off
        }

        if (sh.enable_order_hint) {
            sh.order_hint_bits_minus_1 = static_cast<std::uint8_t>(bd.read_literal(3));
        } else {
            sh.order_hint_bits_minus_1 = 0;
        }
    }

    // common tail: superres / cdef / restoration / color_config / film grain
    sh.enable_superres = bd.read_bool(128);
    sh.enable_cdef = bd.read_bool(128);
    sh.enable_restoration = bd.read_bool(128);

    detail_seq::parse_color_config(bd, sh);

    sh.film_grain_params_present = bd.read_bool(128);

    return sh;
}

}  // namespace av1
}  // namespace bs
