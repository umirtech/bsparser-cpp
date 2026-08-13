
#include "hevc_nal_parser.hpp"
#include "hevc_sps_parser.hpp"
#include "hevc_pps_parser.hpp"
#include "hevc_vps_parser.hpp"
#include "hevc_sei_parser.hpp"
#include "hevc_slice_parser.hpp"
#include "rbsp_bitstream_reader.hpp"
#include "hevc_parameter_set_manager.hpp"
#include "log.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <vector>



/*
* -------------------------------------------------------
* Parameter-set storage
* -------------------------------------------------------
*/
static bs::ParameterSetManager parameter_sets;

int main(int argc, char** argv)
{
    using namespace bs;

    /*
     * Route all log output to stdout so the report stays on
     * the primary stream.
     */
    bs::log::set_stream(&std::cout);

    /*
     * -------------------------------------------------------
     * Load the raw Annex-B stream from disk
     * -------------------------------------------------------
     *
     * The sample bytes are read from a .hevc file at runtime
     * instead of being embedded in the binary, keeping the
     * test executable small.  The same files also serve as the
     * fuzz seed corpus.
     */
    const std::string path =
        (argc > 1) ? std::string(argv[1])
                   : std::string("tests/fuzz/corpus/stream.hevc");

    std::ifstream in(path, std::ios::binary);

    if (!in) {

        BS_LOG_ERROR("cannot open stream: "
            << path << '\n'
            << "usage: hevc_test [file.hevc]\n");

        return 1;
    }

    std::vector<std::uint8_t> annex_b{
        std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>()
    };

    in.close();

    if (annex_b.size() < 4) {

        BS_LOG_ERROR("stream too small: "
            << path << '\n');

        return 1;
    }

    BS_LOG_INFO("loaded "
        << annex_b.size()
        << " bytes from "
        << path
        << '\n');


    /*
     * -------------------------------------------------------
     * Handlers
     * -------------------------------------------------------
     */

    BsNalHandlers handlers;

    handlers.pps =
    [](const NalUnit& nalUnit)
    {

        BS_LOG_INFO("NAL type = "
            << static_cast<unsigned>(nalUnit.nal_type())
            << '\n');

        BS_LOG_INFO("[PPS]\n");

        const auto payload =
            nalUnit.payload_bytes();

        const auto byte_span =
            std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(
                    payload.data()),
                payload.size());

        bs::RbspBitstreamReader reader(byte_span);

        auto pps =
            bs::parse_picture_parameter_set(reader);

        BS_LOG_INFO("  PPS ID: "
            << pps.pps_pic_parameter_set_id
            << '\n');

        BS_LOG_INFO("  SPS ID: "
            << pps.pps_seq_parameter_set_id
            << '\n');

        BS_LOG_INFO("  initial QP: "
            << pps.initial_qp()
            << '\n');

        BS_LOG_INFO("  tiles: "
            << std::boolalpha
            << pps.tiles.tiles_enabled_flag
            << '\n');

        BS_LOG_INFO("  weighted_pred_flag: "
            << std::boolalpha
            << pps.weighted_pred_flag
            << '\n');

        BS_LOG_INFO("  weighted_bipred_flag: "
            << std::boolalpha
            << pps.weighted_bipred_flag
            << '\n');

        BS_LOG_INFO("  lists_modification_present_flag: "
            << pps.lists_modification_present_flag
            << '\n');

        BS_LOG_INFO("  cabac_init_present_flag: "
            << pps.cabac_init_present_flag
            << '\n');

        BS_LOG_INFO("  slice_chroma_qp_offsets_present_flag: "
            << pps.slice_chroma_qp_offsets_present_flag
            << '\n');

        (void)parameter_sets.store_pps(std::move(pps));
    };


    handlers.sps =
    [](const NalUnit& nalUnit)
    {
        BS_LOG_INFO("NAL type = "
            << static_cast<unsigned>(nalUnit.nal_type())
            << '\n');

        BS_LOG_INFO("[SPS]\n");

        const auto payload =
            nalUnit.payload_bytes();

        const auto byte_span =
            std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(
                    payload.data()),
                payload.size());

        bs::RbspBitstreamReader reader(byte_span);

        auto sps =
            bs::parse_sequence_parameter_set(reader);

        BS_LOG_INFO("  SPS ID: "
            << sps.sps_seq_parameter_set_id
            << '\n');

        BS_LOG_INFO("  VPS ID: "
            << static_cast<unsigned>(
                sps.sps_video_parameter_set_id)
            << '\n');

        BS_LOG_INFO("  dimensions: "
            << sps.pic_width_in_luma_samples
            << " x "
            << sps.pic_height_in_luma_samples
            << '\n');

        BS_LOG_INFO("  chroma format: "
            << static_cast<unsigned>(
                sps.chroma_format)
            << '\n');

        BS_LOG_INFO("  luma bit depth: "
            << static_cast<unsigned>(
                sps.luma_bit_depth())
            << '\n');

        BS_LOG_INFO("  chroma bit depth: "
            << static_cast<unsigned>(
                sps.chroma_bit_depth())
            << '\n');

        BS_LOG_INFO("  sps_temporal_mvp_enabled_flag: "
            << std::boolalpha
            << sps.sps_temporal_mvp_enabled_flag
            << '\n');

        BS_LOG_INFO("  long_term_ref_pics_present_flag: "
            << sps.reference_picture_sets
                .long_term_ref_pics_present_flag
            << '\n');

        BS_LOG_INFO("  num_short_term_ref_pic_sets: "
            << sps.reference_picture_sets
                .num_short_term_ref_pic_sets
            << '\n');

        BS_LOG_INFO("  sample_adaptive_offset_enabled_flag: "
            << sps.sample_adaptive_offset_enabled_flag
            << '\n');

        (void)parameter_sets.store_sps(std::move(sps));
    };

    handlers.vps =
    [](const NalUnit& nalUnit)
    {
        BS_LOG_INFO("NAL type = "
            << static_cast<unsigned>(nalUnit.nal_type())
            << '\n');

        BS_LOG_INFO("[VPS]\n");

        const auto payload =
            nalUnit.payload_bytes();

        const auto byte_span =
            std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(
                    payload.data()),
                payload.size());

        bs::RbspBitstreamReader reader(byte_span);

        auto vps =
            bs::parse_video_parameter_set(reader);

        BS_LOG_INFO("  VPS ID: "
            << static_cast<unsigned>(
                vps.vps_video_parameter_set_id)
            << '\n');

        BS_LOG_INFO("  max layers: "
            << vps.max_layers()
            << '\n');

        BS_LOG_INFO("  max sub-layers: "
            << vps.max_sub_layers()
            << '\n');

        (void)parameter_sets.store_vps(std::move(vps));
    };

    handlers.slice =
        [](const NalUnit& nalUnit)
        {
            BS_LOG_INFO("\n[SLICE]\n");

            BS_LOG_INFO("  NAL type: "
                << static_cast<unsigned>(
                    nalUnit.nal_type())
                << '\n');

            try {

                /*
                * ------------------------------------------------
                * Resolve PPS
                * ------------------------------------------------
                *
                * Your sample has PPS ID 0.
                *
                * We first need to parse the beginning of the
                * slice header to know the PPS ID, but your parser
                * intentionally requires the PPS beforehand.
                *
                * Since this sample contains PPS #0, use it for
                * this first end-to-end test.
                */

                const auto* pps =
                    parameter_sets.find_pps(0);

                if (pps == nullptr) {

                    BS_LOG_ERROR("  ERROR: PPS #0 not found\n");

                    return;
                }


                /*
                * ------------------------------------------------
                * Resolve SPS
                * ------------------------------------------------
                */

                const auto* sps =
                    parameter_sets.find_sps(
                        pps->pps_seq_parameter_set_id);

                if (sps == nullptr) {

                    BS_LOG_ERROR("  ERROR: SPS not found\n");

                    return;
                }


                /*
                * ------------------------------------------------
                * EBSP → RBSP
                * ------------------------------------------------
                */

                const auto payload =
                    nalUnit.payload_bytes();

                const auto byte_span =
                    std::span<const std::byte>(
                        reinterpret_cast<const std::byte*>(
                            payload.data()),
                        payload.size());


                RbspBitstreamReader reader(
                    byte_span);


                /*
                * ------------------------------------------------
                * Parse slice header
                * ------------------------------------------------
                */

                auto header =
                    parse_slice_segment_header(
                        reader,
                        *sps,
                        *pps,
                        nalUnit.nal_type(),
                        0);


                /*
                * ------------------------------------------------
                * Print decoded information
                * ------------------------------------------------
                */

                BS_LOG_INFO("  first_slice_segment_in_pic_flag: "
                    << std::boolalpha
                    << header.first_slice_segment_in_pic_flag
                    << '\n');

                BS_LOG_INFO("  dependent_slice_segment_flag: "
                    << header.dependent_slice_segment_flag
                    << '\n');

                BS_LOG_INFO("  PPS ID: "
                    << header.slice_pic_parameter_set_id
                    << '\n');

                BS_LOG_INFO("  slice type: "
                    << static_cast<unsigned>(
                        header.slice_type)
                    << '\n');

                BS_LOG_INFO("  pic_order_cnt_lsb: "
                    << header.slice_pic_order_cnt_lsb
                    << '\n');

                BS_LOG_INFO("  slice_qp_delta: "
                    << header.slice_qp_delta
                    << '\n');

                BS_LOG_INFO("  effective L0 refs: "
                    << header.effective_num_ref_idx_l0
                    << '\n');

                BS_LOG_INFO("  effective L1 refs: "
                    << header.effective_num_ref_idx_l1
                    << '\n');

                BS_LOG_INFO("  NumPocTotalCurr: "
                    << header.reference_pictures
                        .num_poc_total_curr
                    << '\n');

            }
            catch (const SliceSegmentHeaderParseError& e) {

                BS_LOG_ERROR("  SLICE PARSE ERROR: "
                    << e.what()
                    << '\n');

            }
            catch (const std::exception& e) {

                BS_LOG_ERROR("  SLICE EXCEPTION: "
                    << e.what()
                    << '\n');
            }
        };


    handlers.prefix_sei =
        [](const NalUnit& nalUnit)
        {
            BS_LOG_INFO("\n[PREFIX SEI]\n");

            try {

                const auto sei =
                    bs::parse_sei_nal(nalUnit);

                BS_LOG_INFO("  RBSP size: "
                    << sei.rbsp_storage.size()
                    << '\n');

                BS_LOG_INFO("  message count: "
                    << sei.size()
                    << '\n');

                std::size_t index = 0;

                for (const auto& message :
                    sei.view.messages) {

                    BS_LOG_INFO("  message["
                        << index++
                        << "]"
                        << " payload_type="
                        << message.payload_type
                        << " payload_size="
                        << message.payload_size
                        << " actual_size="
                        << message.payload.size()
                        << '\n');
                }

                if (!bs::validate_sei(sei.view)) {

                    BS_LOG_WARN("  WARNING: SEI validation failed\n");
                }

            } catch (const bs::SeiParseError& e) {

                BS_LOG_ERROR("  SEI parse error: "
                    << e.what()
                    << '\n');

            } catch (const std::exception& e) {

                BS_LOG_ERROR("  SEI exception: "
                    << e.what()
                    << '\n');
            }
        };


    handlers.suffix_sei =
        [](const NalUnit& nalUnit)
        {
            BS_LOG_INFO("\n[SUFFIX SEI]\n");

            try {

                const auto sei =
                    bs::parse_sei_nal(nalUnit);

                BS_LOG_INFO("  RBSP size: "
                    << sei.rbsp_storage.size()
                    << '\n');

                BS_LOG_INFO("  message count: "
                    << sei.size()
                    << '\n');

                std::size_t index = 0;

                for (const auto& message :
                    sei.view.messages) {

                    BS_LOG_INFO("  message["
                        << index++
                        << "]"
                        << " payload_type="
                        << message.payload_type
                        << " payload_size="
                        << message.payload_size
                        << " actual_size="
                        << message.payload.size()
                        << '\n');
                }

                if (!bs::validate_sei(sei.view)) {

                    BS_LOG_WARN("  WARNING: SEI validation failed\n");
                }

            } catch (const bs::SeiParseError& e) {

                BS_LOG_ERROR("  SEI parse error: "
                    << e.what()
                    << '\n');
            }
        };


    handlers.unsupported =
    [](const NalUnit& nalUnit)
    {
        BS_LOG_INFO("[UNSUPPORTED] "
            << "type="
            << static_cast<unsigned>(
                nalUnit.nal_type())
            << " payload_size="
            << nalUnit.payload_bytes().size()
            << '\n');
    };


    /*
     * -------------------------------------------------------
     * Dispatch
     * -------------------------------------------------------
     */

    std::span<const std::uint8_t> bytes{
        annex_b.data(),
        annex_b.size()
    };


    const bool ok =
        dispatch_nals(
            bytes,
            NalFramingMode::AnnexB,
            handlers);


    BS_LOG_INFO("\n--------------------------------\n"
        << "dispatch result: "
        << std::boolalpha
        << ok
        << '\n'
        << "VPS count: "
        << parameter_sets.vps_count()
        << '\n'
        << "SPS count: "
        << parameter_sets.sps_count()
        << '\n'
        << "PPS count: "
        << parameter_sets.pps_count()
        << '\n'
        << "--------------------------------\n");


    return ok ? 0 : 1;
}
