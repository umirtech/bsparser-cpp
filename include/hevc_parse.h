#pragma once
#include <cstddef>
#include <bitreader.h>

namespace bsparser {
   
   struct HevcShortTermRps
   {
      bool inter_ref_pic_set_prediction_flag = false;

      uint32_t delta_idx_minus1 = 0;
      bool delta_rps_sign = false;
      uint32_t abs_delta_rps_minus1 = 0;

      uint32_t num_negative_pics = 0;
      uint32_t num_positive_pics = 0;

      std::vector<uint32_t> delta_poc_s0_minus1;
      std::vector<uint8_t> used_by_curr_pic_s0_flag;

      std::vector<uint32_t> delta_poc_s1_minus1;
      std::vector<uint8_t> used_by_curr_pic_s1_flag;

      std::vector<uint8_t> used_by_curr_pic_flag;
      std::vector<uint8_t> use_delta_flag;

      // Derived number of delta POCs.
      uint32_t num_delta_pocs = 0;
   };


   struct HevcVps
   {
      uint32_t id = 0;

      uint32_t max_layers_minus1 = 0;
      uint32_t max_sub_layers_minus1 = 0;
      uint32_t num_layer_sets_minus1 = 0;

      bool base_layer_internal_flag = false;
      bool base_layer_available_flag = false;
      bool temporal_id_nesting_flag = false;
   };


   struct HevcSps
   {
      uint32_t id = 0;
      uint32_t vps_id = 0;

      uint32_t max_sub_layers_minus1 = 0;
      bool temporal_id_nesting_flag = false;

      uint32_t chroma_format_idc = 0;
      bool separate_colour_plane_flag = false;

      uint32_t pic_width_in_luma_samples = 0;
      uint32_t pic_height_in_luma_samples = 0;

      uint32_t conf_win_left_offset = 0;
      uint32_t conf_win_right_offset = 0;
      uint32_t conf_win_top_offset = 0;
      uint32_t conf_win_bottom_offset = 0;

      uint32_t bit_depth_luma_minus8 = 0;
      uint32_t bit_depth_chroma_minus8 = 0;

      uint32_t log2_max_pic_order_cnt_lsb_minus4 = 0;

      uint32_t log2_min_luma_coding_block_size_minus3 = 0;
      uint32_t log2_diff_max_min_luma_coding_block_size = 0;

      uint32_t log2_min_luma_transform_block_size_minus2 = 0;
      uint32_t log2_diff_max_min_luma_transform_block_size = 0;

      uint32_t max_transform_hierarchy_depth_inter = 0;
      uint32_t max_transform_hierarchy_depth_intra = 0;

      bool scaling_list_enabled_flag = false;
      bool sps_scaling_list_data_present_flag = false;

      bool amp_enabled_flag = false;
      bool sample_adaptive_offset_enabled_flag = false;

      bool pcm_enabled_flag = false;
      uint32_t pcm_sample_bit_depth_luma_minus1 = 0;
      uint32_t pcm_sample_bit_depth_chroma_minus1 = 0;
      uint32_t log2_min_pcm_luma_coding_block_size_minus3 = 0;
      uint32_t log2_diff_max_min_pcm_luma_coding_block_size = 0;
      bool pcm_loop_filter_disabled_flag = false;

      std::vector<HevcShortTermRps> short_term_ref_pic_sets;

      bool long_term_ref_pics_present_flag = false;
      uint32_t num_long_term_ref_pics_sps = 0;

      std::vector<uint32_t> lt_ref_pic_poc_lsb_sps;
      std::vector<uint8_t> used_by_curr_pic_lt_sps_flag;

      bool temporal_mvp_enabled_flag = false;
      bool strong_intra_smoothing_enabled_flag = false;

      // Derived values used by slice parsing.

      uint32_t width = 0;
      uint32_t height = 0;

      uint32_t min_cb_size = 0;
      uint32_t max_cb_size = 0;

      uint32_t log2_ctb_size = 0;
      uint32_t ctb_size = 0;

      uint32_t pic_width_in_ctbs = 0;
      uint32_t pic_height_in_ctbs = 0;
      uint32_t pic_size_in_ctbs = 0;
   };


   struct HevcPps
   {
      uint32_t id = 0;
      uint32_t sps_id = 0;

      bool dependent_slice_segments_enabled_flag = false;
      bool output_flag_present_flag = false;

      uint32_t num_extra_slice_header_bits = 0;

      bool sign_data_hiding_enabled_flag = false;
      bool cabac_init_present_flag = false;

      uint32_t num_ref_idx_l0_default_active_minus1 = 0;
      uint32_t num_ref_idx_l1_default_active_minus1 = 0;

      int32_t init_qp_minus26 = 0;

      bool constrained_intra_pred_flag = false;
      bool transform_skip_enabled_flag = false;
      bool cu_qp_delta_enabled_flag = false;

      uint32_t diff_cu_qp_delta_depth = 0;

      int32_t pps_cb_qp_offset = 0;
      int32_t pps_cr_qp_offset = 0;

      bool pps_slice_chroma_qp_offsets_present_flag = false;

      bool weighted_pred_flag = false;
      bool weighted_bipred_flag = false;

      bool transquant_bypass_enabled_flag = false;

      bool tiles_enabled_flag = false;
      bool entropy_coding_sync_enabled_flag = false;

      uint32_t num_tile_columns_minus1 = 0;
      uint32_t num_tile_rows_minus1 = 0;
      bool uniform_spacing_flag = false;

      std::vector<uint32_t> column_width;
      std::vector<uint32_t> row_height;

      bool loop_filter_across_tiles_enabled_flag = false;
      bool pps_loop_filter_across_slices_enabled_flag = false;

      bool deblocking_filter_control_present_flag = false;
      bool deblocking_filter_override_enabled_flag = false;
      bool pps_deblocking_filter_disabled_flag = false;

      int32_t pps_beta_offset_div2 = 0;
      int32_t pps_tc_offset_div2 = 0;

      bool pps_scaling_list_data_present_flag = false;

      bool lists_modification_present_flag = false;

      uint32_t log2_parallel_merge_level_minus2 = 0;

      bool slice_segment_header_extension_present_flag = false;

      bool pps_extension_present_flag = false;
   };


   struct HevcSlice
   {
      bool first_slice_segment_in_pic_flag = false;
      bool no_output_of_prior_pics_flag = false;

      uint32_t pps_id = 0;

      bool dependent_slice_segment_flag = false;
      uint32_t slice_segment_address = 0;

      uint32_t slice_type = 0;

      bool pic_output_flag = true;

      uint32_t colour_plane_id = 0;

      uint32_t slice_pic_order_cnt_lsb = 0;

      bool short_term_ref_pic_set_sps_flag = true;
      uint32_t short_term_ref_pic_set_idx = 0;

      bool slice_temporal_mvp_enabled_flag = false;

      bool slice_sao_luma_flag = false;
      bool slice_sao_chroma_flag = false;

      bool deblocking_filter_override_flag = false;
      bool slice_deblocking_filter_disabled_flag = false;

      int32_t slice_beta_offset_div2 = 0;
      int32_t slice_tc_offset_div2 = 0;

      uint32_t num_ref_idx_l0_active_minus1 = 0;
      uint32_t num_ref_idx_l1_active_minus1 = 0;

      bool num_ref_idx_active_override_flag = false;

      bool collocated_from_l0_flag = true;
      uint32_t collocated_ref_idx = 0;

      bool ref_pic_list_modification_flag_l0 = false;
      bool ref_pic_list_modification_flag_l1 = false;

      bool slice_loop_filter_across_slices_enabled_flag = false;

      uint32_t five_minus_max_num_merge_cand = 0;

      std::vector<uint32_t> slice_reserved_flag;

      // Long-term reference picture syntax.
      uint32_t num_long_term_sps = 0;
      uint32_t num_long_term_pics = 0;

      std::vector<uint32_t> lt_idx_sps;
      std::vector<uint32_t> poc_lsb_lt;
      std::vector<uint8_t> used_by_curr_pic_lt_flag;
      std::vector<uint8_t> delta_poc_msb_present_flag;
      std::vector<uint32_t> delta_poc_msb_cycle_lt;
   };


   struct HevcParseState
   {
      std::unordered_map<uint32_t, HevcVps> vps;
      std::unordered_map<uint32_t, HevcSps> sps;
      std::unordered_map<uint32_t, HevcPps> pps;
   };
    
   Header parse_hevc(const Bytes& d, uint64_t off,HevcParseState& parseState);

}