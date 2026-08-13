#include "rbsp_bitstream_reader.hpp"
#include "hevc_sps_parser.hpp"
#include "hevc_pps_parser.hpp"
#include "hevc_vps_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <vector>

namespace {

struct RawNal {
    std::uint8_t type = 0;
    std::vector<std::uint8_t> header_and_payload;
};

std::vector<RawNal> split_annex_b(const std::vector<std::uint8_t>& data)
{
    std::vector<RawNal> nals;
    std::vector<std::size_t> starts;
    std::size_t i = 0;
    while (i + 3 < data.size()) {
        if (data[i] == 0x00 && data[i + 1] == 0x00) {
            if (i + 3 < data.size() && data[i + 2] == 0x00 &&
                data[i + 3] == 0x01) {
                starts.push_back(i);
                i += 4;
            } else if (data[i + 2] == 0x01) {
                starts.push_back(i);
                i += 3;
            } else {
                ++i;
            }
        } else {
            ++i;
        }
    }
    for (std::size_t k = 0; k < starts.size(); ++k) {
        const std::size_t begin = starts[k];
        const std::size_t end =
            k + 1 < starts.size() ? starts[k + 1] : data.size();
        if (end - begin < 6) {
            continue;
        }
        std::size_t h = begin;
        while (h < end && data[h] == 0x00) {
            ++h;
        }
        if (h >= end || data[h] != 0x01) {
            continue;
        }
        ++h;
        if (h + 2 > end) {
            continue;
        }
        RawNal nal;
        nal.type = static_cast<std::uint8_t>((data[h] >> 1) & 0x3F);
        nal.header_and_payload.assign(data.begin() + h, data.begin() + end);
        nals.push_back(std::move(nal));
    }
    return nals;
}

std::span<const std::byte> payload_span(const RawNal& nal)
{
    const std::uint8_t* base = nal.header_and_payload.data() + 2;
    return std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(base),
        nal.header_and_payload.size() - 2);
}

void dump_sps(bs::RbspBitstreamReader& reader)
{
    using namespace bs;
    const auto sps = parse_sequence_parameter_set(reader);
    std::cout << "[SPS id=" << sps.sps_seq_parameter_set_id << "]\n";
    const auto& ext = sps.extension;
    std::cout << "  ext flags: range=" << ext.range_extension_flag
              << " multi=" << ext.multilayer_extension_flag
              << " 3d=" << ext.extension_3d_flag
              << " scc=" << ext.scc_extension_flag << "\n";
    if (sps.has_range_extension()) {
        const auto& r = sps.range_extension;
        std::cout << "  range: trskip_rot=" << r.transform_skip_rotation_enabled_flag
                  << " trskip_ctx=" << r.transform_skip_context_enabled_flag
                  << " impl_rdpcm=" << r.implicit_rdpcm_enabled_flag
                  << " expl_rdpcm=" << r.explicit_rdpcm_enabled_flag
                  << " ext_prec=" << r.extended_precision_processing_flag
                  << " intra_sm_disable=" << r.intra_smoothing_disabled_flag
                  << " high_prec=" << r.high_precision_offsets_enabled_flag
                  << " persist_rice=" << r.persistent_rice_adaptation_enabled_flag
                  << " cabac_bypass=" << r.cabac_bypass_alignment_enabled_flag << "\n";
    }
    if (sps.has_scc_extension()) {
        const auto& s = sps.scc_extension;
        std::cout << "  scc: curr_pic_ref=" << s.sps_curr_pic_ref_enabled_flag
                  << " palette_mode=" << s.palette_mode_enabled_flag << "\n";
        if (s.palette_mode_enabled_flag) {
            std::cout << "  scc: palette_max_size=" << s.palette_max_size
                      << " delta_max_predictor=" << s.delta_palette_max_predictor_size
                      << " init_present=" << s.sps_palette_predictor_initializers_present_flag
                      << " num_minus1=" << s.sps_num_palette_predictor_initializers_minus1 << "\n";
            const std::size_t n = static_cast<std::size_t>(
                s.sps_num_palette_predictor_initializers_minus1) + 1;
            const std::size_t comps =
                sps.chroma_format == bs::ChromaFormat::Monochrome ? 1 : 3;
            for (std::size_t c = 0; c < comps; ++c) {
                for (std::size_t k = 0; k < n; ++k) {
                    std::cout << "    init[" << c << "][" << k << "]="
                              << s.sps_palette_predictor_initializer[c][k] << "\n";
                }
            }
        }
        std::cout << "  scc: mv_res_control_idc=" << s.motion_vector_resolution_control_idc
                  << " intra_boundary_disable=" << s.intra_boundary_filtering_disabled_flag << "\n";
    }
    if (ext.multilayer_extension_flag) {
        std::cout << "  multilayer: inter_view_mv_vert_constraint="
                  << sps.multilayer_extension.inter_view_mv_vert_constraint_flag << "\n";
    }
    if (ext.extension_3d_flag) {
        for (std::size_t v = 0; v < 2; ++v) {
            const auto& view = sps.three_d_extension.views[v];
            std::cout << "  3d view" << v << ": iv_di_mc=" << view.iv_di_mc_enabled_flag
                      << " iv_mv_scal=" << view.iv_mv_scal_enabled_flag
                      << " tex_mc=" << view.tex_mc_enabled_flag
                      << " log2_ivmc_sub_pb=" << view.log2_ivmc_sub_pb_size_minus3
                      << " intra_contour=" << view.intra_contour_enabled_flag
                      << " intra_dc_only_wedge=" << view.intra_dc_only_wedge_enabled_flag
                      << " cqt_cu_part_pred=" << view.cqt_cu_part_pred_enabled_flag
                      << " inter_dc_only=" << view.inter_dc_only_enabled_flag
                      << " skip_intra=" << view.skip_intra_enabled_flag << "\n";
        }
    }
}

void dump_pps(bs::RbspBitstreamReader& reader)
{
    using namespace bs;
    const auto pps = parse_picture_parameter_set(reader);
    std::cout << "[PPS id=" << pps.pps_pic_parameter_set_id << "]\n";
    const auto& ext = pps.extension;
    std::cout << "  ext flags: range=" << ext.range_extension_flag
              << " multi=" << ext.multilayer_extension_flag
              << " 3d=" << ext.extension_3d_flag
              << " scc=" << ext.scc_extension_flag << "\n";
    if (pps.has_range_extension()) {
        const auto& r = pps.range_extension;
        std::cout << "  range: log2_max_tr_skip_minus2=" << r.log2_max_transform_skip_block_size_minus2
                  << " cross_comp_pred=" << r.cross_component_prediction_enabled_flag
                  << " chroma_qp_offset_list_en=" << r.chroma_qp_offset_list_enabled_flag
                  << " log2_sao_luma=" << r.log2_sao_offset_scale_luma
                  << " log2_sao_chroma=" << r.log2_sao_offset_scale_chroma << "\n";
    }
    if (ext.multilayer_extension_flag) {
        const auto& m = pps.multilayer_extension;
        std::cout << "  multilayer: poc_reset_info=" << m.poc_reset_info_present_flag
                  << " infer_scaling_list=" << m.pps_infer_scaling_list_flag
                  << " num_ref_loc_offsets=" << m.ref_location_offsets.size()
                  << " colour_mapping_en=" << m.colour_mapping_enabled_flag
                  << " cm_ref_layers=" << m.cm_ref_layer_id.size()
                  << " cm_octants=" << m.colour_mapping_octants.size() << "\n";
    }
    if (ext.extension_3d_flag) {
        const auto& t = pps.three_d_extension;
        std::cout << "  3d: dlts_present=" << t.dlts_present_flag
                  << " depth_layers_minus1=" << t.pps_depth_layers_minus1
                  << " bit_depth_for_depth_minus8=" << t.pps_bit_depth_for_depth_layers_minus8
                  << " dlt_flags=" << t.depth_layer_transforms.size() << "\n";
        for (const auto& dlt : t.depth_layer_transforms) {
            std::cout << "    dlt: flag=" << dlt.dlt_flag
                      << " pred=" << dlt.dlt_pred_flag
                      << " val_flags_present=" << dlt.dlt_val_flags_present_flag
                      << " value_flag_count=" << dlt.dlt_value_flag.size() << "\n";
        }
    }
    if (pps.has_scc_extension()) {
        const auto& s = pps.scc_extension;
        std::cout << "  scc: curr_pic_ref=" << s.pps_curr_pic_ref_enabled_flag
                  << " act_en=" << s.residual_adaptive_colour_transform_enabled_flag
                  << " slice_act_offsets_present=" << s.pps_slice_act_qp_offsets_present_flag
                  << " act_y_qp_plus5=" << s.pps_act_y_qp_offset_plus5
                  << " act_cb_qp_plus5=" << s.pps_act_cb_qp_offset_plus5
                  << " act_cr_qp_plus3=" << s.pps_act_cr_qp_offset_plus3
                  << " init_present=" << s.pps_palette_predictor_initializers_present_flag
                  << " num_initializers=" << s.pps_num_palette_predictor_initializers
                  << " mono=" << s.monochrome_palette_flag
                  << " luma_entry_minus8=" << s.luma_bit_depth_entry_minus8
                  << " chroma_entry_minus8=" << s.chroma_bit_depth_entry_minus8 << "\n";
        for (std::size_t c = 0; c < 3; ++c) {
            for (std::size_t k = 0; k < s.pps_num_palette_predictor_initializers; ++k) {
                std::cout << "    pps_init[" << c << "][" << k << "]="
                          << s.pps_palette_predictor_initializer[c][k] << "\n";
            }
        }
    }
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: ext_test <file.hevc>\n";
        return 1;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::cerr << "cannot open " << argv[1] << "\n";
        return 1;
    }
    std::vector<std::uint8_t> data(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());

    for (const auto& nal : split_annex_b(data)) {
        switch (nal.type) {
            case 32: {
                bs::RbspBitstreamReader reader(payload_span(nal));
                const auto vps = bs::parse_video_parameter_set(reader);
                std::cout << "[VPS id=" << static_cast<unsigned>(vps.vps_video_parameter_set_id) << "]\n";
                break;
            }
            case 33: {
                bs::RbspBitstreamReader reader(payload_span(nal));
                dump_sps(reader);
                break;
            }
            case 34: {
                bs::RbspBitstreamReader reader(payload_span(nal));
                dump_pps(reader);
                break;
            }
            default:
                break;
        }
    }
    return 0;
}