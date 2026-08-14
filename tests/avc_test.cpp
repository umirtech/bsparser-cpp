/*
 * ---------------------------------------------------------------------------
 * AVC / H.264 parser demo and validation driver
 * ---------------------------------------------------------------------------
 *
 * Usage:
 *
 *     avc_test <file.h264>
 *
 * Parses an Annex-B H.264 stream: SPS, PPS, SEI and slice segment
 * headers, then prints the decoded syntax values.
 */

#include "avc_nal_parser.hpp"
#include "avc_parameter_set_manager.hpp"
#include "avc_pps_parser.hpp"
#include "avc_sei_parser.hpp"
#include "avc_slice_parser.hpp"
#include "avc_sps_parser.hpp"
#include "log.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

namespace {

bs::avc::ParameterSetManager parameter_sets;

std::span<const std::byte> as_bytes(std::span<const std::uint8_t> input) noexcept {
    return std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(input.data()), input.size()
    );
}

void print_sps(const bs::avc::SequenceParameterSet& sps) {
    BS_LOG_INFO("  profile_idc: " << static_cast<unsigned>(sps.profile_idc) << '\n');

    BS_LOG_INFO("  level_idc: " << static_cast<unsigned>(sps.level_idc) << '\n');

    BS_LOG_INFO("  SPS ID: " << static_cast<unsigned>(sps.seq_parameter_set_id) << '\n');

    BS_LOG_INFO("  chroma_format_idc: " << static_cast<unsigned>(sps.chroma_format_idc) << '\n');

    BS_LOG_INFO(
        "  bit_depth_luma: " << static_cast<unsigned>(sps.bit_depth_luma_minus8 + 8) << '\n'
    );

    BS_LOG_INFO("  log2_max_frame_num: " << sps.log2_max_frame_num() << '\n');

    BS_LOG_INFO("  pic_order_cnt_type: " << static_cast<unsigned>(sps.pic_order_cnt_type) << '\n');

    BS_LOG_INFO("  max_num_ref_frames: " << static_cast<unsigned>(sps.max_num_ref_frames) << '\n');

    BS_LOG_INFO(
        "  dimensions: " << sps.pic_width_in_luma_samples() << " x "
                         << sps.pic_height_in_luma_samples() << '\n'
    );

    BS_LOG_INFO("  frame_mbs_only_flag: " << std::boolalpha << sps.frame_mbs_only_flag << '\n');

    BS_LOG_INFO(
        "  direct_8x8_inference_flag: " << std::boolalpha << sps.direct_8x8_inference_flag << '\n'
    );

    BS_LOG_INFO("  frame_cropping_flag: " << std::boolalpha << sps.frame_cropping_flag << '\n');

    BS_LOG_INFO(
        "  vui_parameters_present_flag: " << std::boolalpha << sps.vui_parameters_present_flag
                                          << '\n'
    );

    if (sps.vui_parameters_present_flag && sps.vui.timing_info_present_flag) {
        BS_LOG_INFO(
            "  vui.timing: num_units_in_tick=" << sps.vui.num_units_in_tick
                                               << " time_scale=" << sps.vui.time_scale << '\n'
        );
    }
}

void print_pps(const bs::avc::PictureParameterSet& pps) {
    BS_LOG_INFO("  PPS ID: " << static_cast<unsigned>(pps.pic_parameter_set_id) << '\n');

    BS_LOG_INFO("  SPS ID: " << static_cast<unsigned>(pps.seq_parameter_set_id) << '\n');

    BS_LOG_INFO(
        "  entropy_coding_mode_flag: " << std::boolalpha << pps.entropy_coding_mode_flag << '\n'
    );

    BS_LOG_INFO("  weighted_pred_flag: " << std::boolalpha << pps.weighted_pred_flag << '\n');

    BS_LOG_INFO(
        "  weighted_bipred_idc: " << static_cast<unsigned>(pps.weighted_bipred_idc) << '\n'
    );

    BS_LOG_INFO("  pic_init_qp_minus26: " << pps.pic_init_qp_minus26 << '\n');

    BS_LOG_INFO(
        "  deblocking_filter_control_present_flag: "
        << std::boolalpha << pps.deblocking_filter_control_present_flag << '\n'
    );

    BS_LOG_INFO(
        "  constrained_intra_pred_flag: " << std::boolalpha << pps.constrained_intra_pred_flag
                                          << '\n'
    );
}

void print_slice(const bs::avc::SliceHeader& header) {
    BS_LOG_INFO("  first_mb_in_slice: " << header.first_mb_in_slice << '\n');

    BS_LOG_INFO("  slice_type: " << static_cast<unsigned>(header.slice_type) << '\n');

    BS_LOG_INFO("  PPS ID: " << static_cast<unsigned>(header.pic_parameter_set_id) << '\n');

    BS_LOG_INFO("  frame_num: " << header.frame_num << '\n');

    BS_LOG_INFO("  pic_order_cnt_lsb: " << header.pic_order_cnt_lsb << '\n');

    BS_LOG_INFO("  slice_qp_delta: " << header.slice_qp_delta << '\n');

    BS_LOG_INFO("  idr_pic_id: " << header.idr_pic_id << '\n');
}

}  // namespace

int main(int argc, char** argv) {
    using namespace bs;

    log::set_stream(&std::cout);

    if (argc < 2) {
        std::cerr << "usage: avc_test <file.h264>\n";
        return 1;
    }

    const std::string path = argv[1];

    std::ifstream in(path, std::ios::binary);

    if (!in) {
        std::cerr << "cannot open " << path << "\n";
        return 1;
    }

    std::vector<std::uint8_t> bytes{
        std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()
    };

    if (bytes.size() < 4) {
        std::cerr << "stream too small: " << path << "\n";
        return 1;
    }

    BS_LOG_INFO("loaded " << bytes.size() << " bytes from " << path << '\n');

    avc::NalHandlers handlers;

    handlers.sps = [](const avc::NalUnit& nal) {
        RbspBitstreamReader reader(as_bytes(nal.payload_bytes()));

        auto sps = avc::parse_sequence_parameter_set(reader);

        BS_LOG_INFO("[SPS]\n");

        print_sps(sps);

        (void)parameter_sets.store_sps(std::move(sps));
    };

    handlers.pps = [](const avc::NalUnit& nal) {
        RbspBitstreamReader reader(as_bytes(nal.payload_bytes()));

        auto pps = avc::parse_picture_parameter_set(reader);

        BS_LOG_INFO("[PPS]\n");

        print_pps(pps);

        (void)parameter_sets.store_pps(std::move(pps));
    };

    handlers.sei = [](const avc::NalUnit& nal) {
        const auto sei = avc::parse_sei_nal(nal);

        BS_LOG_INFO("[SEI]\n");

        BS_LOG_INFO("  message count: " << sei.messages.size() << '\n');

        for (const auto& message : sei.messages) {
            BS_LOG_INFO(
                "  payload_type=" << message.payload_type
                                  << " payload_size=" << message.payload_size << '\n'
            );
        }
    };

    handlers.slice = [](const avc::NalUnit& nal) {
        try {
            /*
             * Slice header carries the PPS id; resolve the
             * dependency chain to get SPS.
             */
            RbspBitstreamReader reader(as_bytes(nal.payload_bytes()));

            const std::uint8_t pps_id = 0;

            const auto resolved = parameter_sets.resolve(pps_id);

            if (!resolved.valid()) {
                BS_LOG_ERROR("  ERROR: parameter sets not found\n");
                return;
            }

            auto header = avc::parse_slice_header(
                reader, *resolved.sps, *resolved.pps, nal.type(), nal.header.nal_ref_idc
            );

            BS_LOG_INFO(
                "[SLICE] type=" << static_cast<unsigned>(nal.nal_type()) << " ref_idc="
                                << static_cast<unsigned>(nal.header.nal_ref_idc) << '\n'
            );

            print_slice(header);

        } catch (const std::exception& e) {
            BS_LOG_ERROR("  SLICE EXCEPTION: " << e.what() << '\n');
        }
    };

    handlers.unsupported = [](const avc::NalUnit& nal) {
        BS_LOG_INFO(
            "[UNSUPPORTED] type=" << static_cast<unsigned>(nal.nal_type())
                                  << " payload_size=" << nal.payload_size() << '\n'
        );
    };

    std::span<const std::uint8_t> input{bytes.data(), bytes.size()};

    const std::size_t parsed = avc::dispatch_nals(input, NalFramingMode::AnnexB, handlers);

    BS_LOG_INFO(
        "\n--------------------------------\n"
        << "parsed NAL count: " << parsed << '\n'
        << "SPS count: " << parameter_sets.sps_count() << '\n'
        << "PPS count: " << parameter_sets.pps_count() << '\n'
        << "--------------------------------\n"
    );

    return 0;
}