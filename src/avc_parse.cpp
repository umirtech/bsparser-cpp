#include "avc_parse.h"
#include <iomanip>


namespace bsparser{
    
    Header parse_avc(const Bytes& d, uint64_t off)
    {
        if (d.empty()) throw std::out_of_range("empty AVC NAL");
        uint8_t t = d[0] & 0x1f;
        uint8_t ref = (d[0] >> 5) & 3;


        static const char* names[] = {
                "Unknown",
                "Slice",
                "DP A",
                "DP B",
                "DP C",
                "IDR",
                "SEI",
                "SPS",
                "PPS",
                "AUD",
                "End of sequence",
                "End of stream",
                "Filler data",
                "SPS extension",
                "Prefix NAL unit",
                "Subset SPS"
        };

        Header h{off, d.size(), t < 16 ? names[t] : "AVC NAL", t == 5, {}};
        field(h, "nal_unit_type", t);
        field(h, "nal_ref_idc", ref);

        Bytes r = rbsp(d, 1);
        BitReader b(r);

        try
        {
            if (t == 7)
            {
                auto profile = b.u(8);
                field(h, "profile_idc", profile);
                static const char* prof_names[] = {
                        "Baseline", "Main", "Extended", "High", "High 10", "High 4:2:2", "High 4:4:4 Predictive"
                };
                if (profile == 66)
                    field(h, "profile_name", prof_names[0]);
                else if (profile == 77)
                    field(h, "profile_name", prof_names[1]);
                else if (profile == 88)
                    field(h, "profile_name", prof_names[2]);
                else if (profile == 100)
                    field(h, "profile_name", prof_names[3]);
                else if (profile == 110)
                    field(h, "profile_name", prof_names[4]);
                else if (profile == 122)
                    field(h, "profile_name", prof_names[5]);
                else if (profile == 244)
                    field(h, "profile_name", prof_names[6]);

                auto constraints = b.u(8);
                field(h, "constraint_set_flags", constraints);
                auto level = b.u(8);
                field(h, "level_idc", level);
                std::ostringstream lvl_s;
                lvl_s << (level / 10) << "." << (level % 10);
                field(h, "level", lvl_s.str());
                field(h, "seq_parameter_set_id", b.ue());

                unsigned chroma = 1;
                unsigned bit_depth_luma = 8;
                unsigned bit_depth_chroma = 8;
                if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
                    profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 ||
                    profile == 139 || profile == 134)
                {
                    chroma = b.ue();
                    field(h, "chroma_format_idc", chroma);
                    if (chroma == 3)
                    {
                        field(h, "separate_colour_plane_flag", b.u(1));
                    }
                    bit_depth_luma = b.ue() + 8;
                    bit_depth_chroma = b.ue() + 8;
                    field(h, "bit_depth_luma_minus8", bit_depth_luma - 8);
                    field(h, "bit_depth_chroma_minus8", bit_depth_chroma - 8);
                    field(h, "bit_depth", bit_depth_luma);
                    b.u(1);
                    if (b.u(1))
                    {
                        unsigned n = chroma != 3 ? 8 : 12;
                        for (unsigned i = 0; i < n; ++i)
                        {
                            if (b.u(1))
                            {
                                int last = 8, next = 8;
                                unsigned size = i < 6 ? 16 : 64;
                                for (unsigned j = 0; j < size; ++j)
                                {
                                    if (next) next = (last + b.se() + 256) % 256;
                                    last = next ? next : last;
                                }
                            }
                        }
                    }
                }
                else
                {
                    field(h, "chroma_format_idc", chroma);
                    field(h, "bit_depth", 8);
                }

                auto log2_max_frame_num = b.ue() + 4;
                field(h, "log2_max_frame_num_minus4", log2_max_frame_num - 4);
                auto poc_type = b.ue();
                field(h, "pic_order_cnt_type", poc_type);
                if (poc_type == 0)
                {
                    field(h, "log2_max_pic_order_cnt_lsb_minus4", b.ue());
                }
                else if (poc_type == 1)
                {
                    b.u(1);
                    b.se();
                    b.se();
                    auto num_ref_frames_in_poc_cycle = b.ue();
                    for (uint32_t i = 0; i < num_ref_frames_in_poc_cycle; ++i)
                        b.se();
                }
                auto max_num_ref_frames = b.ue();
                field(h, "num_ref_frames", max_num_ref_frames);
                field(h, "gaps_in_frame_num_value_allowed_flag", b.u(1));

                auto pic_width_in_mbs = b.ue() + 1;
                auto pic_height_in_map_units = b.ue() + 1;
                field(h, "pic_width_in_mbs_minus1", pic_width_in_mbs - 1);
                field(h, "pic_height_in_map_units_minus1", pic_height_in_map_units - 1);

                auto frame_mbs_only = b.u(1);
                field(h, "frame_mbs_only_flag", frame_mbs_only);
                if (!frame_mbs_only)
                {
                    field(h, "mb_adaptive_frame_field_flag", b.u(1));
                }
                field(h, "direct_8x8_inference_flag", b.u(1));

                auto crop = b.u(1);
                field(h, "frame_cropping_flag", crop);
                uint32_t l = 0, rgt = 0, tp = 0, bt = 0;
                if (crop)
                {
                    l = b.ue();
                    rgt = b.ue();
                    tp = b.ue();
                    bt = b.ue();
                    field(h, "frame_crop_left_offset", l);
                    field(h, "frame_crop_right_offset", rgt);
                    field(h, "frame_crop_top_offset", tp);
                    field(h, "frame_crop_bottom_offset", bt);
                }
                const uint32_t crop_x = (chroma == 1 || chroma == 2) ? 2 : 1;
                const uint32_t crop_y = (2 - frame_mbs_only) * (chroma == 1 ? 2 : 1);
                uint32_t w = pic_width_in_mbs * 16 - (l + rgt) * crop_x;
                uint32_t ht = pic_height_in_map_units * 16 * (2 - frame_mbs_only) - (tp + bt) * crop_y;
                field(h, "width", w);
                field(h, "height", ht);

                if (b.u(1))
                {
                    field(h, "vui_parameters_present_flag", 1);
                    if (b.u(1))
                    {
                        auto aspect_ratio_idc = b.u(8);
                        field(h, "aspect_ratio_idc", aspect_ratio_idc);
                        if (aspect_ratio_idc == 255)
                        {
                            field(h, "sar_width", b.u(16));
                            field(h, "sar_height", b.u(16));
                        }
                    }
                    if (b.u(1)) b.u(1);
                    if (b.u(1))
                    {
                        b.u(3);
                        field(h, "video_full_range_flag", b.u(1));
                        if (b.u(1))
                        {
                            field(h, "colour_primaries", b.u(8));
                            field(h, "transfer_characteristics", b.u(8));
                            field(h, "matrix_coefficients", b.u(8));
                        }
                    }
                    if (b.u(1))
                    {
                        b.ue();
                        b.ue();
                    }
                    if (b.u(1))
                    {
                        auto num_units = b.u(32);
                        auto time_scale = b.u(32);
                        auto fixed_fps = b.u(1);
                        field(h, "num_units_in_tick", num_units);
                        field(h, "time_scale", time_scale);
                        field(h, "fixed_frame_rate_flag", fixed_fps);
                        if (num_units > 0)
                        {
                            double fps = double(time_scale) / (2.0 * num_units);
                            std::ostringstream fps_s;
                            fps_s << std::fixed << std::setprecision(2) << fps;
                            field(h, "fps", fps_s.str());
                        }
                    }
                }
            }
            else if (t == 8)
            {
                auto pps_id = b.ue();
                auto sps_id = b.ue();
                field(h, "pic_parameter_set_id", pps_id);
                field(h, "seq_parameter_set_id", sps_id);
                auto entropy_coding = b.u(1);
                field(h, "entropy_coding_mode_flag", entropy_coding);
                field(h, "entropy_coding_mode", entropy_coding ? "CABAC" : "CAVLC");
                field(h, "bottom_field_pic_order_in_frame_present_flag", b.u(1));
                auto num_sg = b.ue();
                field(h, "num_slice_groups_minus1", num_sg);
                if (num_sg > 0)
                {
                    auto sg_map_type = b.ue();
                    if (sg_map_type == 0)
                    {
                        for (uint32_t i = 0; i <= num_sg; ++i)
                            b.ue();
                    }
                    else if (sg_map_type == 2)
                    {
                        for (uint32_t i = 0; i < num_sg; ++i)
                        {
                            b.ue();
                            b.ue();
                        }
                    }
                    else if (sg_map_type == 3 || sg_map_type == 4 || sg_map_type == 5)
                    {
                        b.u(1);
                        b.ue();
                    }
                    else if (sg_map_type == 6)
                    {
                        auto pic_size_in_map_units = b.ue() + 1;
                        for (uint32_t i = 0; i < pic_size_in_map_units; ++i)
                            b.u(1);
                    }
                }
                field(h, "num_ref_idx_l0_default_active_minus1", b.ue());
                field(h, "num_ref_idx_l1_default_active_minus1", b.ue());
                field(h, "weighted_pred_flag", b.u(1));
                field(h, "weighted_bipred_idc", b.u(2));
                field(h, "pic_init_qp_minus26", b.se());
                field(h, "pic_init_qs_minus26", b.se());
                field(h, "chroma_qp_index_offset", b.se());
                field(h, "deblocking_filter_control_present_flag", b.u(1));
                field(h, "constrained_intra_pred_flag", b.u(1));
                field(h, "redundant_pic_cnt_present_flag", b.u(1));
                if (b.bits_left() > 0)
                {
                    field(h, "transform_8x8_mode_flag", b.u(1));
                }
            }
            else if (t == 1 || t == 5 || t == 2 || t == 3 || t == 4)
            {
                auto first_mb = b.ue();
                auto slice_type = b.ue();
                auto pps_id = b.ue();
                field(h, "first_mb_in_slice", first_mb);
                field(h, "slice_type", slice_type);

                static const char* st_names[] = {
                        "P",
                        "B",
                        "I",
                        "SP",
                        "SI",
                        "P",
                        "B",
                        "I",
                        "SP",
                        "SI"
                };

                field(h, "slice_type_name", slice_type < 10 ? st_names[slice_type] : "Unknown");
                if (t != 5 && slice_type < 10) h.type = st_names[slice_type];
                field(h, "pic_parameter_set_id", pps_id);
                if (b.bits_left() >= 4)
                {
                    auto frame_num = b.u(4);
                    field(h, "frame_num", frame_num);
                }
                if (t == 5 && b.bits_left() > 0)
                {
                    auto idr_pic_id = b.ue();
                    field(h, "idr_pic_id", idr_pic_id);
                    field(h, "poc", 0);
                }
                else if (b.bits_left() >= 4)
                {
                    auto poc_lsb = b.u(4);
                    field(h, "pic_order_cnt_lsb", poc_lsb);
                    field(h, "poc", poc_lsb);
                }
            }
            else if (t == 9)
            {
                field(h, "primary_pic_type", b.u(3));
            }
        }
        catch (const std::exception&)
        {
        }
        return h;
    }

}