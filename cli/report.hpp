#pragma once

/*
 * ===========================================================================
 * CLI report model + exporters
 * ===========================================================================
 *
 * Collects a structured report while a stream is parsed through the unified
 * bs::parse() API, and serialises it to JSON or to a self-contained HTML
 * viewer (with inline filter controls).
 *
 * This is a tooling helper, not part of the core library: it pulls in the
 * syntax parsers so each handler can attach a short human summary to the NAL
 * it receives.
 */

#include <bsparser.hpp>

#include <parser/avc_sei_parser.hpp>
#include <parser/avc_slice_parser.hpp>
#include <parser/hevc_sei_parser.hpp>
#include <parser/hevc_slice_parser.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace bs {
namespace cli {

/*
 * One parsed NAL unit.
 */
struct NalEntry {
    std::size_t index = 0;
    std::size_t offset = 0;
    std::string type;
    unsigned type_id = 0;
    bool vcl = false;
    std::size_t size = 0;
    std::string summary;
    std::vector<std::pair<std::string, std::string>> fields;
};

/*
 * Whole-stream report.
 */
struct Report {
    std::string codec;
    std::string framing;
    std::size_t parsed = 0;
    std::vector<NalEntry> entries;
};

/*
 * ---------------------------------------------------------------------------
 * Internal: shared capture state
 * ---------------------------------------------------------------------------
 *
 * The unified dispatcher callbacks are plain function pointers, so they cannot
 * capture.  A single build_report() call sets these process-local pointers and
 * clears them afterwards.
 */
namespace detail {

inline Report* g_report = nullptr;
inline const std::uint8_t* g_data_start = nullptr;
inline State* g_state = nullptr;

[[nodiscard]]
inline std::span<const std::byte> to_byte_span(std::span<const std::uint8_t> data) noexcept {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()), data.size());
}

[[nodiscard]]
inline std::size_t offset_of(const std::uint8_t* p) noexcept {
    return static_cast<std::size_t>(p - g_data_start);
}

[[nodiscard]]
inline std::string hevc_type_name(NalUnitType t) noexcept {
    switch (t) {
        case NalUnitType::VPS_NUT:
            return "VPS_NUT";
        case NalUnitType::SPS_NUT:
            return "SPS_NUT";
        case NalUnitType::PPS_NUT:
            return "PPS_NUT";
        case NalUnitType::PREFIX_SEI_NUT:
            return "PREFIX_SEI_NUT";
        case NalUnitType::SUFFIX_SEI_NUT:
            return "SUFFIX_SEI_NUT";
        case NalUnitType::AUD_NUT:
            return "AUD_NUT";
        case NalUnitType::EOS_NUT:
            return "EOS_NUT";
        case NalUnitType::EOB_NUT:
            return "EOB_NUT";
        case NalUnitType::FD_NUT:
            return "FD_NUT";
        default:
            if (is_vcl_nal_unit(t)) {
                return "VCL";
            }
            return "NAL_" + std::to_string(static_cast<unsigned>(t));
    }
}

[[nodiscard]]
inline std::string avc_type_name(avc::NalUnitType t) noexcept {
    switch (t) {
        case avc::NalUnitType::SliceNonIdr:
            return "SliceNonIdr";
        case avc::NalUnitType::SliceDataPartitionA:
            return "SliceDataPartitionA";
        case avc::NalUnitType::SliceDataPartitionB:
            return "SliceDataPartitionB";
        case avc::NalUnitType::SliceDataPartitionC:
            return "SliceDataPartitionC";
        case avc::NalUnitType::SliceIdr:
            return "SliceIdr";
        case avc::NalUnitType::Sei:
            return "SEI";
        case avc::NalUnitType::Sps:
            return "SPS";
        case avc::NalUnitType::Pps:
            return "PPS";
        case avc::NalUnitType::AccessUnitDelimiter:
            return "AUD";
        case avc::NalUnitType::EndOfSequence:
            return "EndOfSequence";
        case avc::NalUnitType::EndOfStream:
            return "EndOfStream";
        case avc::NalUnitType::FillerData:
            return "FillerData";
        case avc::NalUnitType::SpsExtension:
            return "SpsExtension";
        default:
            if (avc::is_vcl_nal_unit(t)) {
                return "VCL";
            }
            return "NAL_" + std::to_string(static_cast<unsigned>(t));
    }
}

[[nodiscard]]
inline std::string vvc_type_name(vvc::NalUnitType t) noexcept {
    switch (t) {
        case vvc::NalUnitType::OpiNut:
            return "OPI_NUT";
        case vvc::NalUnitType::DciNut:
            return "DCI_NUT";
        case vvc::NalUnitType::VpsNut:
            return "VPS_NUT";
        case vvc::NalUnitType::SpsNut:
            return "SPS_NUT";
        case vvc::NalUnitType::PpsNut:
            return "PPS_NUT";
        case vvc::NalUnitType::PrefixApsNut:
            return "PREFIX_APS_NUT";
        case vvc::NalUnitType::SuffixApsNut:
            return "SUFFIX_APS_NUT";
        case vvc::NalUnitType::PhNut:
            return "PH_NUT";
        case vvc::NalUnitType::AudNut:
            return "AUD_NUT";
        case vvc::NalUnitType::EosNut:
            return "EOS_NUT";
        case vvc::NalUnitType::EobNut:
            return "EOB_NUT";
        case vvc::NalUnitType::SeiPrefixNut:
            return "PREFIX_SEI_NUT";
        case vvc::NalUnitType::SeiSuffixNut:
            return "SUFFIX_SEI_NUT";
        case vvc::NalUnitType::FdNut:
            return "FD_NUT";
        default:
            if (vvc::is_vcl_nal_unit(static_cast<std::uint8_t>(t))) {
                return "VCL";
            }
            return "NAL_" + std::to_string(static_cast<unsigned>(t));
    }
}

[[nodiscard]]
inline std::string av1_type_name(unsigned type) noexcept {
    switch (type) {
        case 1:
            return "SEQUENCE_HEADER";
        case 2:
            return "TEMPORAL_DELIMITER";
        case 3:
            return "FRAME_HEADER";
        case 4:
            return "TILE_GROUP";
        case 5:
            return "METADATA";
        case 6:
            return "FRAME";
        case 7:
            return "REDUNDANT_FRAME_HEADER";
        case 8:
            return "TILE_LIST";
        case 15:
            return "PADDING";
        default:
            return "OBU_" + std::to_string(type);
    }
}

/*
 * Full field dumps for the parsed parameter-set structs.
 */
inline std::vector<std::pair<std::string, std::string>> hevc_vps_fields(
    const VideoParameterSet& v
) {
    return {
        {"vps_id", std::to_string(v.vps_video_parameter_set_id)},
        {"max_layers", std::to_string(v.max_layers())},
        {"max_sub_layers", std::to_string(v.max_sub_layers())},
        {"max_layer_id", std::to_string(v.vps_max_layer_id)},
        {"num_layer_sets", std::to_string(v.vps_num_layer_sets_minus1 + 1)},
    };
}

inline std::vector<std::pair<std::string, std::string>> hevc_sps_fields(
    const SequenceParameterSet& s
) {
    return {
        {"sps_id", std::to_string(s.sps_seq_parameter_set_id)},
        {"vps_id", std::to_string(s.sps_video_parameter_set_id)},
        {"max_sub_layers", std::to_string(s.sps_max_sub_layers_minus1 + 1)},
        {"temporal_id_nesting", s.sps_temporal_id_nesting_flag ? "1" : "0"},
        {"profile_idc", std::to_string(s.profile_tier_level.general_profile_idc)},
        {"tier", s.profile_tier_level.general_tier_flag ? "1" : "0"},
        {"level_idc", std::to_string(s.profile_tier_level.general_level_idc)},
        {"width", std::to_string(s.pic_width_in_luma_samples)},
        {"height", std::to_string(s.pic_height_in_luma_samples)},
        {"chroma_format", std::to_string(static_cast<unsigned>(s.chroma_format))},
        {"bit_depth_luma", std::to_string(s.bit_depth_luma_minus8 + 8)},
        {"bit_depth_chroma", std::to_string(s.bit_depth_chroma_minus8 + 8)},
        {"log2_max_poc_lsb", std::to_string(s.log2_max_pic_order_cnt_lsb_minus4 + 4)},
        {"min_cb_size", std::to_string(s.coding_blocks.min_luma_coding_block_size())},
        {"max_cb_size", std::to_string(s.coding_blocks.max_luma_coding_block_size())},
        {"min_tb_size", std::to_string(s.coding_blocks.min_luma_transform_block_size())},
        {"max_tb_size", std::to_string(s.coding_blocks.max_luma_transform_block_size())},
        {"max_transform_hierarchy_inter",
         std::to_string(s.coding_blocks.max_transform_hierarchy_depth_inter)},
        {"max_transform_hierarchy_intra",
         std::to_string(s.coding_blocks.max_transform_hierarchy_depth_intra)},
        {"num_short_term_rps",
         std::to_string(s.reference_picture_sets.num_short_term_ref_pic_sets)},
        {"long_term_refs_present",
         s.reference_picture_sets.long_term_ref_pics_present_flag ? "1" : "0"},
        {"num_long_term_refs", std::to_string(s.reference_picture_sets.num_long_term_ref_pics_sps)},
    };
}

inline std::vector<std::pair<std::string, std::string>> hevc_pps_fields(
    const PictureParameterSet& p
) {
    return {
        {"pps_id", std::to_string(p.pps_pic_parameter_set_id)},
        {"sps_id", std::to_string(p.pps_seq_parameter_set_id)},
        {"dependent_slice_segments", p.dependent_slice_segments_enabled_flag ? "1" : "0"},
        {"output_flag_present", p.output_flag_present_flag ? "1" : "0"},
        {"num_extra_slice_header_bits", std::to_string(p.num_extra_slice_header_bits)},
        {"sign_data_hiding", p.sign_data_hiding_enabled_flag ? "1" : "0"},
        {"cabac_init_present", p.cabac_init_present_flag ? "1" : "0"},
        {"num_ref_idx_l0_default", std::to_string(p.num_ref_idx_l0_default_active_minus1 + 1)},
        {"num_ref_idx_l1_default", std::to_string(p.num_ref_idx_l1_default_active_minus1 + 1)},
        {"init_qp_minus26", std::to_string(p.init_qp_minus26)},
        {"constrained_intra_pred", p.constrained_intra_pred_flag ? "1" : "0"},
        {"transform_skip_enabled", p.transform_skip_enabled_flag ? "1" : "0"},
        {"cu_qp_delta_enabled", p.cu_qp_delta_enabled_flag ? "1" : "0"},
        {"cb_qp_offset", std::to_string(p.pps_cb_qp_offset)},
        {"cr_qp_offset", std::to_string(p.pps_cr_qp_offset)},
        {"deblocking_filter_control",
         p.deblocking.deblocking_filter_control_present_flag ? "1" : "0"},
        {"tiles_enabled", p.tiles.tiles_enabled_flag ? "1" : "0"},
        {"loop_filter_across_tiles", p.tiles.loop_filter_across_tiles_enabled_flag ? "1" : "0"},
    };
}

inline std::vector<std::pair<std::string, std::string>> avc_sps_fields(
    const avc::SequenceParameterSet& s
) {
    return {
        {"sps_id", std::to_string(s.seq_parameter_set_id)},
        {"profile_idc", std::to_string(s.profile_idc)},
        {"level_idc", std::to_string(s.level_idc)},
        {"chroma_format_idc", std::to_string(s.chroma_format_idc)},
        {"bit_depth_luma", std::to_string(s.bit_depth_luma_minus8 + 8)},
        {"bit_depth_chroma", std::to_string(s.bit_depth_chroma_minus8 + 8)},
        {"max_num_ref_frames", std::to_string(s.max_num_ref_frames)},
        {"pic_order_cnt_type", std::to_string(s.pic_order_cnt_type)},
        {"width", std::to_string(s.pic_width_in_luma_samples())},
        {"height", std::to_string(s.pic_height_in_luma_samples())},
        {"frame_mbs_only", s.frame_mbs_only_flag ? "1" : "0"},
        {"direct_8x8_inference", s.direct_8x8_inference_flag ? "1" : "0"},
    };
}

inline std::vector<std::pair<std::string, std::string>> avc_pps_fields(
    const avc::PictureParameterSet& p
) {
    return {
        {"pps_id", std::to_string(p.pic_parameter_set_id)},
        {"sps_id", std::to_string(p.seq_parameter_set_id)},
        {"entropy_coding_mode", p.entropy_coding_mode_flag ? "1" : "0"},
        {"num_ref_idx_l0_default", std::to_string(p.num_ref_idx_l0_default_active_minus1 + 1)},
        {"num_ref_idx_l1_default", std::to_string(p.num_ref_idx_l1_default_active_minus1 + 1)},
        {"weighted_pred", p.weighted_pred_flag ? "1" : "0"},
        {"weighted_bipred_idc", std::to_string(p.weighted_bipred_idc)},
        {"pic_init_qp_minus26", std::to_string(p.pic_init_qp_minus26)},
        {"deblocking_filter_control", p.deblocking_filter_control_present_flag ? "1" : "0"},
        {"redundant_pic_cnt_present", p.redundant_pic_cnt_present_flag ? "1" : "0"},
    };
}

inline void add_entry(
    std::string type,
    unsigned type_id,
    bool vcl,
    const std::uint8_t* payload,
    std::size_t size,
    std::string summary,
    std::vector<std::pair<std::string, std::string>> fields = {}
) {
    if (g_report == nullptr) {
        return;
    }

    NalEntry entry;
    entry.index = g_report->entries.size();
    entry.offset = offset_of(payload);
    entry.type = std::move(type);
    entry.type_id = type_id;
    entry.vcl = vcl;
    entry.size = size;
    entry.summary = std::move(summary);
    entry.fields = std::move(fields);

    g_report->entries.push_back(std::move(entry));
}

}  // namespace detail

/*
 * ---------------------------------------------------------------------------
 * Report builder
 * ---------------------------------------------------------------------------
 */
[[nodiscard]]
inline Report build_report(
    Codec codec, std::span<const std::uint8_t> data, NalFramingMode mode, unsigned length_size = 4
) {
    Report report;
    report.codec = (codec == Codec::Hevc) ? "HEVC" : "AVC";
    report.framing = (mode == NalFramingMode::AnnexB) ? "Annex-B" : "Length-prefixed";

    auto state = create_state(codec);

    detail::g_report = &report;
    detail::g_data_start = data.data();
    detail::g_state = state.get();

    if (codec == Codec::Hevc) {
        BsNalHandlers handlers{};

        handlers.vps = [](const NalUnit& nal) {
            std::string summary = "VPS";
            std::vector<std::pair<std::string, std::string>> fields;
            try {
                RbspBitstreamReader reader(detail::to_byte_span(nal.payload_bytes()));
                auto vps = parse_video_parameter_set(reader);
                fields = detail::hevc_vps_fields(vps);
                summary = "VPS id=" +
                          std::to_string(static_cast<unsigned>(vps.vps_video_parameter_set_id));
            } catch (...) {
                summary = "VPS (unparsable)";
            }
            detail::add_entry(
                detail::hevc_type_name(nal.type()),
                static_cast<unsigned>(nal.type()),
                false,
                nal.payload_bytes().data(),
                nal.payload_bytes().size(),
                summary,
                std::move(fields)
            );
        };

        handlers.sps = [](const NalUnit& nal) {
            std::string summary = "SPS";
            std::vector<std::pair<std::string, std::string>> fields;
            try {
                RbspBitstreamReader reader(detail::to_byte_span(nal.payload_bytes()));
                auto sps = parse_sequence_parameter_set(reader);
                fields = detail::hevc_sps_fields(sps);
                summary = std::to_string(sps.pic_width_in_luma_samples) + "x" +
                          std::to_string(sps.pic_height_in_luma_samples) +
                          " chroma=" + std::to_string(static_cast<unsigned>(sps.chroma_format));
            } catch (...) {
                summary = "SPS (unparsable)";
            }
            detail::add_entry(
                detail::hevc_type_name(nal.type()),
                static_cast<unsigned>(nal.type()),
                false,
                nal.payload_bytes().data(),
                nal.payload_bytes().size(),
                summary,
                std::move(fields)
            );
        };

        handlers.pps = [](const NalUnit& nal) {
            std::string summary = "PPS";
            std::vector<std::pair<std::string, std::string>> fields;
            try {
                RbspBitstreamReader reader(detail::to_byte_span(nal.payload_bytes()));
                auto pps = parse_picture_parameter_set(reader);
                fields = detail::hevc_pps_fields(pps);
                summary =
                    "PPS id=" + std::to_string(static_cast<unsigned>(pps.pps_pic_parameter_set_id));
            } catch (...) {
                summary = "PPS (unparsable)";
            }
            detail::add_entry(
                detail::hevc_type_name(nal.type()),
                static_cast<unsigned>(nal.type()),
                false,
                nal.payload_bytes().data(),
                nal.payload_bytes().size(),
                summary,
                std::move(fields)
            );
        };

        handlers.prefix_sei = [](const NalUnit& nal) {
            std::string summary = "PREFIX_SEI";
            std::size_t count = 0;
            try {
                auto sei = parse_sei_nal(nal);
                count = sei.size();
                summary = "PREFIX_SEI messages=" + std::to_string(count);
            } catch (...) {
                summary = "PREFIX_SEI (unparsable)";
            }
            detail::add_entry(
                detail::hevc_type_name(nal.type()),
                static_cast<unsigned>(nal.type()),
                false,
                nal.payload_bytes().data(),
                nal.payload_bytes().size(),
                summary,
                {{"messages", std::to_string(count)}}
            );
        };

        handlers.suffix_sei = [](const NalUnit& nal) {
            std::string summary = "SUFFIX_SEI";
            std::size_t count = 0;
            try {
                auto sei = parse_sei_nal(nal);
                count = sei.size();
                summary = "SUFFIX_SEI messages=" + std::to_string(count);
            } catch (...) {
                summary = "SUFFIX_SEI (unparsable)";
            }
            detail::add_entry(
                detail::hevc_type_name(nal.type()),
                static_cast<unsigned>(nal.type()),
                false,
                nal.payload_bytes().data(),
                nal.payload_bytes().size(),
                summary,
                {{"messages", std::to_string(count)}}
            );
        };

        handlers.slice = [](const NalUnit& nal) {
            const std::string name = detail::hevc_type_name(nal.type());
            std::string summary = "Slice (" + name + ")";
            std::vector<std::pair<std::string, std::string>> fields;
            try {
                /* RbspReader: no per-slice logical-map build over the payload */
                RbspReader reader(nal.payload_bytes());
                auto sh =
                    parse_slice_segment_header(reader, {}, {}, nal.nal_type(), nal.temporal_id());
                static const char* st[] = {"B", "P", "I"};
                const char* stn = static_cast<unsigned>(sh.slice_type) <= 2u
                                      ? st[static_cast<unsigned>(sh.slice_type)]
                                      : "?";
                summary = "Slice " + std::string(stn) +
                          " pps=" + std::to_string(sh.slice_pic_parameter_set_id);
                fields = {
                    {"slice_type", stn},
                    {"pps_id", std::to_string(sh.slice_pic_parameter_set_id)},
                    {"first_slice", sh.first_slice_segment_in_pic_flag ? "1" : "0"},
                    {"dependent_slice", sh.dependent_slice_segment_flag ? "1" : "0"},
                    {"slice_qp_delta", std::to_string(sh.slice_qp_delta)},
                    {"poc_lsb", std::to_string(sh.slice_pic_order_cnt_lsb)},
                    {"sao_luma", sh.slice_sao_luma_flag ? "1" : "0"},
                    {"sao_chroma", sh.slice_sao_chroma_flag ? "1" : "0"},
                };
            } catch (...) {
                summary = "Slice (" + name + ")";
            }
            detail::add_entry(
                name,
                static_cast<unsigned>(nal.type()),
                true,
                nal.payload_bytes().data(),
                nal.payload_bytes().size(),
                summary,
                std::move(fields)
            );
        };

        handlers.unsupported = [](const NalUnit& nal) {
            const std::string name = detail::hevc_type_name(nal.type());
            detail::add_entry(
                name,
                static_cast<unsigned>(nal.type()),
                false,
                nal.payload_bytes().data(),
                nal.payload_bytes().size(),
                name,
                {}
            );
        };

        report.parsed = parse(*state, data, mode, handlers, length_size);

    } else if (codec == Codec::Avc) {
        avc::NalHandlers handlers{};

        handlers.sps = [](const avc::NalUnit& nal) {
            std::string summary = "SPS";
            std::vector<std::pair<std::string, std::string>> fields;
            try {
                RbspBitstreamReader reader(detail::to_byte_span(nal.payload_bytes()));
                auto sps = avc::parse_sequence_parameter_set(reader);
                fields = detail::avc_sps_fields(sps);
                summary = std::to_string(sps.pic_width_in_luma_samples()) + "x" +
                          std::to_string(sps.pic_height_in_luma_samples()) +
                          " profile=" + std::to_string(static_cast<unsigned>(sps.profile_idc));
            } catch (...) {
                summary = "SPS (unparsable)";
            }
            detail::add_entry(
                detail::avc_type_name(nal.type()),
                static_cast<unsigned>(nal.type()),
                false,
                nal.payload_bytes().data(),
                nal.payload_bytes().size(),
                summary,
                std::move(fields)
            );
        };

        handlers.pps = [](const avc::NalUnit& nal) {
            std::string summary = "PPS";
            std::vector<std::pair<std::string, std::string>> fields;
            try {
                RbspBitstreamReader reader(detail::to_byte_span(nal.payload_bytes()));
                auto pps = avc::parse_picture_parameter_set(reader);
                fields = detail::avc_pps_fields(pps);
                summary =
                    "PPS id=" + std::to_string(static_cast<unsigned>(pps.pic_parameter_set_id));
            } catch (...) {
                summary = "PPS (unparsable)";
            }
            detail::add_entry(
                detail::avc_type_name(nal.type()),
                static_cast<unsigned>(nal.type()),
                false,
                nal.payload_bytes().data(),
                nal.payload_bytes().size(),
                summary,
                std::move(fields)
            );
        };

        handlers.sei = [](const avc::NalUnit& nal) {
            std::string summary = "SEI";
            std::size_t count = 0;
            try {
                auto sei = avc::parse_sei_nal(nal);
                count = sei.messages.size();
                summary = "SEI messages=" + std::to_string(count);
            } catch (...) {
                summary = "SEI (unparsable)";
            }
            detail::add_entry(
                detail::avc_type_name(nal.type()),
                static_cast<unsigned>(nal.type()),
                false,
                nal.payload_bytes().data(),
                nal.payload_bytes().size(),
                summary,
                {{"messages", std::to_string(count)}}
            );
        };

        handlers.slice = [](const avc::NalUnit& nal) {
            const std::string name = detail::avc_type_name(nal.type());
            std::string summary = "Slice (" + name + ")";
            std::vector<std::pair<std::string, std::string>> fields;
            try {
                RbspReader reader(nal.payload_bytes());
                auto sh =
                    avc::parse_slice_header(reader, {}, {}, nal.type(), nal.header.nal_ref_idc);
                static const char* st[] = {"P", "B", "I", "SP", "SI"};
                const char* stn = static_cast<unsigned>(sh.slice_type) <= 4u
                                      ? st[static_cast<unsigned>(sh.slice_type)]
                                      : "?";
                summary =
                    "Slice " + std::string(stn) + " pps=" + std::to_string(sh.pic_parameter_set_id);
                fields = {
                    {"slice_type", stn},
                    {"pps_id", std::to_string(sh.pic_parameter_set_id)},
                    {"first_mb", std::to_string(sh.first_mb_in_slice)},
                    {"frame_num", std::to_string(sh.frame_num)},
                    {"idr_pic_id", std::to_string(sh.idr_pic_id)},
                    {"pic_order_cnt_lsb", std::to_string(sh.pic_order_cnt_lsb)},
                    {"slice_qp_delta", std::to_string(sh.slice_qp_delta)},
                };
            } catch (...) {
                summary = "Slice (" + name + ")";
            }
            detail::add_entry(
                name,
                static_cast<unsigned>(nal.type()),
                true,
                nal.payload_bytes().data(),
                nal.payload_bytes().size(),
                summary,
                std::move(fields)
            );
        };

        handlers.unsupported = [](const avc::NalUnit& nal) {
            const std::string name = detail::avc_type_name(nal.type());
            detail::add_entry(
                name,
                static_cast<unsigned>(nal.type()),
                false,
                nal.payload_bytes().data(),
                nal.payload_bytes().size(),
                name,
                {}
            );
        };

        report.parsed = parse(*state, data, mode, handlers, length_size);
    } else if (
        codec == Codec::Vvc || codec == Codec::Av1 || codec == Codec::Vp9 || codec == Codec::Vp8
    ) {
        report.codec = (codec == Codec::Vvc)   ? "VVC"
                       : (codec == Codec::Av1) ? "AV1"
                       : (codec == Codec::Vp9) ? "VP9"
                                               : "VP8";
        report.framing = (mode == NalFramingMode::AnnexB) ? "Annex-B"
                         : (mode == NalFramingMode::Obu)  ? "OBU"
                         : (mode == NalFramingMode::Ivf)  ? "IVF"
                                                          : "Length-prefixed";

        if (codec == Codec::Vvc) {
            auto add_vvc = [&](auto framer) {
                std::size_t i = 0;
                while (framer.valid()) {
                    const auto span = framer.nal();
                    try {
                        auto nal = vvc::parse_nal_unit(span);
                        const std::string name = detail::vvc_type_name(nal.type());
                        std::string summary = name;
                        std::vector<std::pair<std::string, std::string>> fields;

                        if (nal.is_vcl()) {
                            try {
                                RbspReader r(nal.payload_bytes());
                                auto sh = vvc::parse_slice_header(r);
                                static const char* st[] = {"B", "P", "I"};
                                const char* stn = static_cast<unsigned>(sh.slice_type) <= 2u
                                                      ? st[static_cast<unsigned>(sh.slice_type)]
                                                      : "?";
                                summary = "Slice pps=" + std::to_string(sh.pps_id) + " type=" + stn;
                                fields = {
                                    {"pps_id", std::to_string(sh.pps_id)},
                                    {"slice_type", stn},
                                };
                            } catch (...) {
                                summary = "Slice (unparsable)";
                            }
                        } else {
                            try {
                                RbspReader r(nal.payload_bytes());
                                switch (nal.type()) {
                                    case vvc::NalUnitType::VpsNut: {
                                        auto vps = vvc::parse_vps(r);
                                        summary = "VPS id=" + std::to_string(vps.vps_id);
                                        fields = {
                                            {"vps_id", std::to_string(vps.vps_id)},
                                            {"max_layers",
                                             std::to_string(vps.max_layers_minus1 + 1)},
                                            {"max_sublayers",
                                             std::to_string(vps.max_sublayers_minus1 + 1)},
                                            {"num_ptls", std::to_string(vps.num_ptls_minus1 + 1)},
                                        };
                                        break;
                                    }
                                    case vvc::NalUnitType::SpsNut: {
                                        auto sps = vvc::parse_sps(r);
                                        summary = "SPS id=" + std::to_string(sps.sps_id);
                                        fields = {
                                            {"sps_id", std::to_string(sps.sps_id)},
                                            {"vps_id", std::to_string(sps.vps_id)},
                                            {"max_sublayers",
                                             std::to_string(sps.max_sublayers_minus1 + 1)},
                                            {"chroma_format",
                                             std::to_string(sps.chroma_format_idc)},
                                            {"log2_ctu_size",
                                             std::to_string(sps.log2_ctu_size_minus5 + 5)},
                                        };
                                        break;
                                    }
                                    case vvc::NalUnitType::PpsNut: {
                                        auto pps = vvc::parse_pps(r);
                                        summary = "PPS id=" + std::to_string(pps.pps_id) + " " +
                                                  std::to_string(pps.pic_width_in_luma_samples) +
                                                  "x" +
                                                  std::to_string(pps.pic_height_in_luma_samples);
                                        fields = {
                                            {"pps_id", std::to_string(pps.pps_id)},
                                            {"sps_id", std::to_string(pps.sps_id)},
                                            {"width",
                                             std::to_string(pps.pic_width_in_luma_samples)},
                                            {"height",
                                             std::to_string(pps.pic_height_in_luma_samples)},
                                        };
                                        break;
                                    }
                                    case vvc::NalUnitType::PhNut: {
                                        auto ph = vvc::parse_ph(r);
                                        summary =
                                            "Picture Header pps_id=" + std::to_string(ph.pps_id);
                                        fields = {{"pps_id", std::to_string(ph.pps_id)}};
                                        break;
                                    }
                                    case vvc::NalUnitType::DciNut: {
                                        auto dci = vvc::parse_dci(r);
                                        summary = "DCI sps=" + std::to_string(dci.num_sps + 1);
                                        fields = {{"num_sps", std::to_string(dci.num_sps + 1)}};
                                        break;
                                    }
                                    case vvc::NalUnitType::OpiNut: {
                                        auto opi = vvc::parse_opi(r);
                                        summary = "OPI";
                                        fields = {
                                            {"ols_info_present", opi.ols_info_present ? "1" : "0"},
                                            {"ptl_present", opi.ptl_present ? "1" : "0"},
                                        };
                                        break;
                                    }
                                    default:
                                        break;
                                }
                            } catch (...) {
                                summary = name + " (unparsable)";
                            }
                        }

                        detail::add_entry(
                            name,
                            nal.nal_type(),
                            nal.is_vcl(),
                            nal.payload_bytes().data(),
                            nal.payload_bytes().size(),
                            summary,
                            std::move(fields)
                        );
                    } catch (...) {
                        detail::add_entry(
                            "bad", 0, false, span.data(), span.size(), "unparsable", {}
                        );
                    }
                    framer.next();
                    ++i;
                }
                return i;
            };

            if (mode == NalFramingMode::AnnexB) {
                report.parsed = add_vvc(AnnexBNalIterator{data});
            } else {
                report.parsed = add_vvc(LengthPrefixedNalIterator{data, length_size});
            }

        } else if (codec == Codec::Av1) {
            av1::ObuFramer framer{data};
            std::size_t i = 0;
            while (framer.valid()) {
                const auto span = framer.obu();
                try {
                    auto obu = av1::parse_obu(span);
                    const std::string name = detail::av1_type_name(obu.type());
                    std::string summary = name;
                    std::vector<std::pair<std::string, std::string>> fields;

                    if (obu.type() == static_cast<std::uint8_t>(av1::ObuType::SequenceHeader)) {
                        auto sh = av1::parse_sequence_header(obu.payload_bytes());
                        summary = "Sequence Header profile=" + std::to_string(sh.seq_profile);
                        fields = {
                            {"seq_profile", std::to_string(sh.seq_profile)},
                            {"still_picture", sh.still_picture ? "1" : "0"},
                            {"reduced_still_picture_header",
                             sh.reduced_still_picture_header ? "1" : "0"},
                        };
                        if (sh.dimensions_present) {
                            fields.emplace_back(
                                "max_frame_width", std::to_string(sh.max_frame_width)
                            );
                            fields.emplace_back(
                                "max_frame_height", std::to_string(sh.max_frame_height)
                            );
                            summary += " " + std::to_string(sh.max_frame_width) + "x" +
                                       std::to_string(sh.max_frame_height);
                        }
                    } else if (
                        obu.type() == static_cast<std::uint8_t>(av1::ObuType::FrameHeader) ||
                        obu.type() ==
                            static_cast<std::uint8_t>(av1::ObuType::RedundantFrameHeader) ||
                        obu.type() == static_cast<std::uint8_t>(av1::ObuType::Frame)
                    ) {
                        auto fh = av1::parse_frame_header(obu.payload_bytes());
                        static const char* ft[] = {"KEY", "INTER", "INTRA_ONLY", "SWITCH"};
                        const char* ftn = static_cast<unsigned>(fh.frame_type) <= 3u
                                              ? ft[static_cast<unsigned>(fh.frame_type)]
                                              : "?";
                        summary = "Frame Header " + std::string(ftn);
                        fields = {
                            {"frame_type", ftn},
                            {"show_frame", fh.show_frame ? "1" : "0"},
                            {"error_resilient_mode", fh.error_resilient_mode ? "1" : "0"},
                            {"disable_cdf_update", fh.disable_cdf_update ? "1" : "0"},
                            {"allow_screen_content_tools",
                             fh.allow_screen_content_tools ? "1" : "0"},
                        };
                    }

                    detail::add_entry(
                        name,
                        obu.type(),
                        false,
                        obu.payload_bytes().data(),
                        obu.payload_bytes().size(),
                        summary,
                        std::move(fields)
                    );
                } catch (...) {
                    detail::add_entry("bad", 0, false, span.data(), span.size(), "unparsable", {});
                }
                framer.next();
                ++i;
            }
            report.parsed = i;

        } else if (codec == Codec::Vp9) {
            IvfFramer framer{data};
            std::size_t i = 0;
            while (framer.valid()) {
                const auto frame = framer.frame();
                try {
                    auto fh = vp9::parse_frame_header(frame);
                    const bool key = fh.frame_type == vp9::FrameType::KeyFrame;
                    const std::string summary = std::string(key ? "Key" : "Inter") + " frame " +
                                                std::to_string(fh.width) + "x" +
                                                std::to_string(fh.height);
                    detail::add_entry(
                        "frame",
                        0,
                        false,
                        frame.data(),
                        frame.size(),
                        summary,
                        {
                            {"frame_marker", std::to_string(fh.frame_marker)},
                            {"profile", std::to_string(fh.profile)},
                            {"frame_type", key ? "KEY" : "INTER"},
                            {"show_frame", fh.show_frame ? "1" : "0"},
                            {"error_resilient_mode", fh.error_resilient_mode ? "1" : "0"},
                            {"width", std::to_string(fh.width)},
                            {"height", std::to_string(fh.height)},
                        }
                    );
                } catch (...) {
                    detail::add_entry(
                        "frame", 0, false, frame.data(), frame.size(), "unparsable", {}
                    );
                }
                framer.next();
                ++i;
            }
            report.parsed = i;

        } else {
            IvfFramer framer{data};
            std::size_t i = 0;
            while (framer.valid()) {
                const auto frame = framer.frame();
                try {
                    auto fh = vp8::parse_frame_header(frame);
                    const std::string summary = std::string(fh.key_frame ? "Key" : "Inter") +
                                                " frame " + std::to_string(fh.width) + "x" +
                                                std::to_string(fh.height);
                    detail::add_entry(
                        "frame",
                        0,
                        false,
                        frame.data(),
                        frame.size(),
                        summary,
                        {
                            {"key_frame", fh.key_frame ? "1" : "0"},
                            {"version", std::to_string(fh.version)},
                            {"show_frame", fh.show_frame ? "1" : "0"},
                            {"first_part_size", std::to_string(fh.first_part_size)},
                            {"width", std::to_string(fh.width)},
                            {"height", std::to_string(fh.height)},
                        }
                    );
                } catch (...) {
                    detail::add_entry(
                        "frame", 0, false, frame.data(), frame.size(), "unparsable", {}
                    );
                }
                framer.next();
                ++i;
            }
            report.parsed = i;
        }
    }

    detail::g_report = nullptr;
    detail::g_data_start = nullptr;
    detail::g_state = nullptr;

    return report;
}

/*
 * ---------------------------------------------------------------------------
 * JSON export
 * ---------------------------------------------------------------------------
 */
[[nodiscard]]
inline std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);

    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }

    return out;
}

[[nodiscard]]
inline std::string to_json(const Report& report) {
    std::ostringstream os;
    os << "{\n";
    os << "  \"codec\": \"" << json_escape(report.codec) << "\",\n";
    os << "  \"framing\": \"" << json_escape(report.framing) << "\",\n";
    os << "  \"parsed\": " << report.parsed << ",\n";
    os << "  \"nals\": [\n";

    for (std::size_t i = 0; i < report.entries.size(); ++i) {
        const auto& e = report.entries[i];

        os << "    {\n";
        os << "      \"index\": " << e.index << ",\n";
        os << "      \"offset\": " << e.offset << ",\n";
        os << "      \"type\": \"" << json_escape(e.type) << "\",\n";
        os << "      \"type_id\": " << e.type_id << ",\n";
        os << "      \"vcl\": " << (e.vcl ? "true" : "false") << ",\n";
        os << "      \"size\": " << e.size << ",\n";
        os << "      \"summary\": \"" << json_escape(e.summary) << "\",\n";
        os << "      \"fields\": {";

        for (std::size_t f = 0; f < e.fields.size(); ++f) {
            if (f) {
                os << ", ";
            }
            os << "\"" << json_escape(e.fields[f].first) << "\": \""
               << json_escape(e.fields[f].second) << "\"";
        }

        os << "}\n";
        os << "    }";
        os << (i + 1 < report.entries.size() ? ",\n" : "\n");
    }

    os << "  ]\n";
    os << "}\n";

    return os.str();
}

/*
 * ---------------------------------------------------------------------------
 * HTML export (self-contained viewer with filter controls)
 * ---------------------------------------------------------------------------
 */
[[nodiscard]]
inline std::string to_html(const Report& report) {
    /*
     * Embed the report as JSON.  Escape "</" so the data cannot
     * prematurely close the <script> element.
     */
    std::string json = to_json(report);
    std::string safe;
    safe.reserve(json.size());

    for (std::size_t i = 0; i < json.size(); ++i) {
        if (json.compare(i, 2, "</") == 0) {
            safe += "<\\/";
            ++i;
        } else {
            safe += json[i];
        }
    }

    std::ostringstream os;
    os << "<!DOCTYPE html>\n";
    os << "<html lang=\"en\">\n";
    os << "<head>\n";
    os << "<meta charset=\"utf-8\">\n";
    os << "<meta name=\"viewport\" "
          "content=\"width=device-width, initial-scale=1\">\n";
    os << "<title>bsparser report - " << json_escape(report.codec) << "</title>\n";
    os << "<style>\n";
    os << "body{font-family:system-ui,Segoe UI,Roboto,sans-serif;"
          "margin:0;background:#0f1115;color:#e6e6e6}\n";
    os << "header{padding:16px 20px;background:#171a21;"
          "border-bottom:1px solid #2a2f3a;position:sticky;top:0;z-index:1}\n";
    os << "h1{font-size:18px;margin:0 0 4px}\n";
    os << ".meta{font-size:13px;color:#9aa4b2}\n";
    os << ".controls{display:flex;flex-wrap:wrap;gap:10px;"
          "align-items:center;margin-top:12px}\n";
    os << "input,select{background:#0f1115;color:#e6e6e6;"
          "border:1px solid #2a2f3a;border-radius:6px;"
          "padding:6px 8px;font-size:13px}\n";
    os << "label{font-size:13px;color:#9aa4b2;display:flex;"
          "gap:6px;align-items:center}\n";
    os << "table{border-collapse:collapse;width:100%;"
          "font-size:13px;margin-top:14px}\n";
    os << "th,td{text-align:left;padding:8px 10px;border-bottom:"
          "1px solid #20242d;vertical-align:top}\n";
    os << "th{position:sticky;top:150px;background:#171a21;"
          "color:#9aa4b2;font-weight:600}\n";
    os << "tr:hover td{background:#1b1f27}\n";
    os << ".vcl{color:#7ee787}.nonvcl{color:#79c0ff}\n";
    os << ".summary{color:#d6c77e}\n";
    os << ".count{font-size:13px;color:#9aa4b2;margin-left:auto}\n";
    os << "details{font-size:12px;color:#9aa4b2}\n";
    os << "</style>\n";
    os << "</head>\n";
    os << "<body>\n";
    os << "<header>\n";
    os << "<h1>bsparser - " << json_escape(report.codec) << " bitstream report</h1>\n";
    os << "<div class=\"meta\">framing: " << json_escape(report.framing)
       << " &middot; NAL units: " << report.entries.size() << " &middot; parsed: " << report.parsed
       << "</div>\n";
    os << "<div class=\"controls\">\n";
    os << "<input id=\"search\" type=\"text\" "
          "placeholder=\"filter by type / summary...\" "
          "style=\"min-width:240px\">\n";
    os << "<select id=\"typeFilter\"><option value=\"\">"
          "All types</option></select>\n";
    os << "<label><input id=\"vclOnly\" type=\"checkbox\"> "
          "VCL only</label>\n";
    os << "<label><input id=\"nonVclOnly\" type=\"checkbox\"> "
          "Non-VCL only</label>\n";
    os << "<span class=\"count\" id=\"count\"></span>\n";
    os << "</div>\n";
    os << "</header>\n";
    os << "<table>\n";
    os << "<thead><tr><th>#</th><th>Offset</th><th>Type</th>"
          "<th>Size</th><th>Summary</th><th>Details</th>"
          "</tr></thead>\n";
    os << "<tbody id=\"rows\"></tbody>\n";
    os << "</table>\n";
    os << "<script>\n";
    os << "const REPORT = " << safe << ";\n";
    os << R"JS(
const tbody = document.getElementById('rows');
const search = document.getElementById('search');
const typeFilter = document.getElementById('typeFilter');
const vclOnly = document.getElementById('vclOnly');
const nonVclOnly = document.getElementById('nonVclOnly');
const count = document.getElementById('count');

function buildTypeOptions() {
  const seen = new Set();
  REPORT.nals.forEach(n => { seen.add(n.type); });
  [...seen].sort().forEach(t => {
    const o = document.createElement('option');
    o.value = t; o.textContent = t;
    typeFilter.appendChild(o);
  });
}

function esc(s) {
  return String(s).replace(/[&<>"']/g, c => ({
    '&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'
  }[c]));
}

function matches(n, q, type, vc, nv) {
  if (type && n.type !== type) return false;
  if (vc && !n.vcl) return false;
  if (nv && n.vcl) return false;
  if (q) {
    const hay = (n.type + ' ' + n.summary + ' ' + JSON.stringify(n.fields)).toLowerCase();
    if (!hay.includes(q)) return false;
  }
  return true;
}

function render() {
  const q = search.value.trim().toLowerCase();
  const type = typeFilter.value;
  const vc = vclOnly.checked;
  const nv = nonVclOnly.checked;
  tbody.innerHTML = '';
  let shown = 0;
  REPORT.nals.forEach(n => {
    if (!matches(n, q, type, vc, nv)) return;
    shown++;
    const tr = document.createElement('tr');
    const cls = n.vcl ? 'vcl' : 'nonvcl';
    let details = '';
    for (const [k, v] of Object.entries(n.fields)) {
      details += esc(k) + ': ' + esc(v) + '<br>';
    }
    tr.innerHTML =
      '<td>' + n.index + '</td>' +
      '<td>0x' + n.offset.toString(16) + '</td>' +
      '<td class="' + cls + '">' + esc(n.type) + '</td>' +
      '<td>' + n.size + '</td>' +
      '<td class="summary">' + esc(n.summary) + '</td>' +
      '<td><details><summary>fields</summary>' + details + '</details></td>';
    tbody.appendChild(tr);
  });
  count.textContent = shown + ' / ' + REPORT.nals.length + ' shown';
}

[search, typeFilter, vclOnly, nonVclOnly].forEach(el =>
  el.addEventListener('input', render));
buildTypeOptions();
render();
)JS";
    os << "\n</script>\n";
    os << "</body>\n";
    os << "</html>\n";

    return os.str();
}

}  // namespace cli
}  // namespace bs
