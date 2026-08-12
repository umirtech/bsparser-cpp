#include "vvc_parse.h"


namespace bsparser{
    
    Header parse_vvc(const Bytes& d, uint64_t off)
    {
        if (d.size() < 2) throw std::out_of_range("truncated NAL header");
        
        uint8_t type = (d[0] >> 3) & 0x1F;
        uint8_t layer_id = ((d[0] & 0x07) << 3) | (d[1] >> 5);
        uint8_t tid = d[1] & 0x07;

        Header h{off, d.size(),"VVC NAL" , false, {}};
        field(h, "nal_unit_type", type);
        field(h, "nuh_layer_id", layer_id);
        field(h, "nuh_temporal_id_plus1", tid);


        static const char* types[] = {
                "TRAIL",
                "STSA",
                "RADL",
                "RASL",
                "RSV_VCL_4",
                "RSV_VCL_5",
                "RSV_VCL_6",
                "IDR_W_RADL",
                "IDR_N_LP",
                "CRA",
                "GDR",
                "RSV_IRAP_11",
                "OPI",
                "DCI",
                "VPS",
                "SPS",
                "PPS",
                "PREFIX_APS",
                "SUFFIX_APS",
                "PH",
                "AUD",
                "EOS",
                "EOB",
                "PREFIX_SEI",
                "SUFFIX_SEI",
                "FD"
        };

        h.type = type < 26 ? types[type] : "VVC NAL";
        h.keyframe = type >= 7 && type <= 10;

        Bytes r = rbsp(d, 2);
        BitReader b(r);
        try
        {
            if (type == 14)
            {
                field(h, "vps_video_parameter_set_id", b.u(4));
                field(h, "vps_max_layers_minus1", b.u(6));
                field(h, "vps_max_sublayers_minus1", b.u(3));
            }
            else if (type == 15)
            {
                auto sps_id = b.u(4);
                auto vps_id = b.u(4);
                auto max_sublayers = b.u(3);
                auto chroma = b.u(2);
                auto ctu_log2 = b.u(2) + 5;
                field(h, "sps_seq_parameter_set_id", sps_id);
                field(h, "sps_video_parameter_set_id", vps_id);
                field(h, "sps_max_sublayers_minus1", max_sublayers);
                field(h, "sps_chroma_format_idc", chroma);
                field(h, "sps_log2_ctu_size_minus5", ctu_log2 - 5);
                field(h, "ctu_size", 1u << ctu_log2);
                b.u(1);
                if (max_sublayers > 0) b.u(1);
                b.u(1);
                auto rpr = b.u(1);
                field(h, "sps_ref_pic_resampling_enabled_flag", rpr);
                if (rpr) b.u(1);
                auto max_w = b.ue();
                auto max_h = b.ue();
                field(h, "sps_pic_width_max_in_luma_samples", max_w);
                field(h, "sps_pic_height_max_in_luma_samples", max_h);
                auto conf = b.u(1);
                field(h, "sps_conformance_window_flag", conf);
                uint32_t l = 0, rgt = 0, tp = 0, bt = 0;
                if (conf)
                {
                    l = b.ue();
                    rgt = b.ue();
                    tp = b.ue();
                    bt = b.ue();
                    field(h, "sps_conf_win_left_offset", l);
                    field(h, "sps_conf_win_right_offset", rgt);
                    field(h, "sps_conf_win_top_offset", tp);
                    field(h, "sps_conf_win_bottom_offset", bt);
                }
                uint32_t sx = chroma == 1 || chroma == 2 ? 2 : 1, sy = chroma == 1 ? 2 : 1;
                field(h, "width", max_w - (l + rgt) * sx);
                field(h, "height", max_h - (tp + bt) * sy);
                auto subpic = b.u(1);
                if (subpic)
                {
                    auto num_subpics = b.ue() + 1;
                    field(h, "sps_num_subpics_minus1", num_subpics - 1);
                }
                auto bitdepth = b.ue() + 8;
                field(h, "sps_bitdepth_minus8", bitdepth - 8);
                field(h, "bit_depth", bitdepth);
            }
            else if (type == 16)
            {
                auto pps_id = b.u(6);
                auto sps_id = b.u(4);
                field(h, "pps_pic_parameter_set_id", pps_id);
                field(h, "pps_seq_parameter_set_id", sps_id);
                b.u(1);
                auto w = b.ue();
                auto ht = b.ue();
                field(h, "pps_pic_width_in_luma_samples", w);
                field(h, "pps_pic_height_in_luma_samples", ht);
                field(h, "width", w);
                field(h, "height", ht);
            }
            else if (type == 19)
            {
                field(h, "ph_gdr_or_irap_pic_flag", b.u(1));
                field(h, "ph_non_ref_pic_flag", b.u(1));
                auto gdr = b.u(1);
                field(h, "ph_gdr_pic_flag", gdr);
                field(h, "ph_inter_slice_allowed_flag", b.u(1));
                field(h, "ph_intra_slice_allowed_flag", b.u(1));
                field(h, "ph_pic_parameter_set_id", b.ue());
                if (b.bits_left() >= 8)
                {
                    auto poc = b.u(8);
                    field(h, "ph_pic_order_cnt_lsb", poc);
                    field(h, "poc", poc);
                }
            }
            else if (type <= 11)
            {
                auto ph_in_sh = b.u(1);
                field(h, "sh_picture_header_in_slice_header_flag", ph_in_sh);
            }
        }
        catch (const std::exception&)
        {
        }
        return h;
        
    }

}