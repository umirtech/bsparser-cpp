/*
 * ---------------------------------------------------------------------------
 * AVC / H.264 parser validation against FFmpeg-derived reference data
 * ---------------------------------------------------------------------------
 *
 * Usage:
 *
 *     ffmpeg_avc_test <stream.h264> <reference.txt>
 *
 * The stream is parsed with the bsparser AVC library.  A set of decoded
 * SPS / VUI / SEI values is then compared against the reference file,
 * which is produced by the generation script from ffprobe / trace_headers
 * output (FFmpeg being the source of truth).
 *
 * See tests/ffmpeg/generate.ps1 for the field set.  Keys not present in
 * the reference file are skipped.  The HDR flags are always emitted by the
 * generator (0 when absent) so the validator can also assert their
 * *absence* on SDR streams.
 *
 * Exit status:
 *
 *     0   every reference key matched the parsed value
 *     1   one or more mismatches, or the stream could not be parsed
 */

#include "avc_nal_parser.hpp"
#include "avc_parameter_set_manager.hpp"
#include "avc_sei_parser.hpp"
#include "avc_sps_parser.hpp"
#include "rbsp_bitstream_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

bs::avc::ParameterSetManager g_parameter_sets;

bool g_mastering_display_present = false;
std::uint32_t g_max_content_light_level = 0;
std::uint32_t g_max_pic_average_light_level = 0;


std::unordered_map<std::string, std::string>
read_reference(const std::string& path)
{
    std::unordered_map<std::string, std::string> result;

    std::ifstream in(path);

    if (!in) {
        return result;
    }

    std::string line;

    while (std::getline(in, line)) {

        if (line.empty() || line[0] == '#') {
            continue;
        }

        const auto eq = line.find('=');

        if (eq == std::string::npos) {
            continue;
        }

        result[line.substr(0, eq)] =
            line.substr(eq + 1);
    }

    return result;
}


const std::string*
ref_value(
    const std::unordered_map<std::string, std::string>& ref,
    const std::string& key)
{
    const auto it = ref.find(key);

    if (it == ref.end()) {
        return nullptr;
    }

    return &it->second;
}


bool check_int(
    const std::string& key,
    unsigned long long parsed,
    const std::unordered_map<std::string, std::string>& ref,
    std::vector<std::string>& failures)
{
    const std::string* expected = ref_value(ref, key);

    if (expected == nullptr) {
        return true;
    }

    try {
        const auto want =
            std::stoull(*expected);

        if (parsed == want) {
            return true;
        }

        failures.push_back(
            key + ": parser=" +
            std::to_string(parsed) +
            " reference=" + *expected);
        return false;
    }
    catch (...) {
        failures.push_back(
            key + ": reference value not an integer: " + *expected);
        return false;
    }
}


/*
 * Parse the AVC Content Light Level SEI payload (type 144).
 *
 * Layout: max_content_light_level (u(16)),
 *         max_pic_average_light_level (u(16)) -- big endian.
 */
void inspect_cll(
    std::span<const std::uint8_t> payload)
{
    if (payload.size() < 4) {
        return;
    }

    g_max_content_light_level =
        (static_cast<std::uint32_t>(payload[0]) << 8) |
         static_cast<std::uint32_t>(payload[1]);

    g_max_pic_average_light_level =
        (static_cast<std::uint32_t>(payload[2]) << 8) |
         static_cast<std::uint32_t>(payload[3]);
}


int run_stream(
    const std::string& stream_path,
    const std::unordered_map<std::string, std::string>& ref)
{
    std::ifstream in(stream_path, std::ios::binary);

    if (!in) {
        std::cerr << "cannot open stream: " << stream_path << "\n";
        return 1;
    }

    std::vector<std::uint8_t> annex_b{
        std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>()
    };

    in.close();

    std::vector<std::string> failures;

    bs::avc::NalHandlers handlers;

    handlers.sps =
        [](const bs::avc::NalUnit& nal) {

            const auto payload = nal.payload_bytes();

            const auto byte_span =
                std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(
                        payload.data()),
                    payload.size());

            bs::RbspBitstreamReader reader(byte_span);

            auto sps =
                bs::avc::parse_sequence_parameter_set(reader);

            (void)g_parameter_sets.store_sps(std::move(sps));
        };

    handlers.sei =
        [](const bs::avc::NalUnit& nal) {

            const auto sei =
                bs::avc::parse_sei_nal(nal);

            for (const auto& message : sei.messages) {

                if (message.payload_type == 137) {
                    g_mastering_display_present = true;
                }
                else if (message.payload_type == 144) {
                    inspect_cll(message.payload);
                }
            }
        };

    std::span<const std::uint8_t> bytes{
        annex_b.data(),
        annex_b.size()
    };

    try {
        (void)bs::avc::dispatch_nals(
            bytes,
            bs::NalFramingMode::AnnexB,
            handlers);
    }
    catch (const std::exception& e) {
        failures.push_back(
            std::string("dispatch threw: ") + e.what());
    }

    if (g_parameter_sets.sps_count() == 0) {
        failures.push_back("no SPS parsed");
    }

    for (std::uint8_t id = 0; id < 32; ++id) {

        const auto* sps =
            g_parameter_sets.find_sps(id);

        if (sps == nullptr) {
            continue;
        }

        check_int("profile_idc",
            sps->profile_idc, ref, failures);

        check_int("level_idc",
            sps->level_idc, ref, failures);

        const std::uint32_t coded_width =
            (sps->pic_width_in_mbs_minus1 + 1) * 16;

        const std::uint32_t coded_height =
            (sps->pic_height_in_map_units_minus1 + 1) * 16 *
            (sps->frame_mbs_only_flag ? 1u : 2u);

        check_int("coded_width", coded_width, ref, failures);
        check_int("coded_height", coded_height, ref, failures);

        check_int("display_width",
            sps->pic_width_in_luma_samples(), ref, failures);

        check_int("display_height",
            sps->pic_height_in_luma_samples(), ref, failures);

        check_int("chroma_format_idc",
            sps->chroma_format_idc, ref, failures);

        check_int("bit_depth_luma",
            static_cast<unsigned>(
                sps->bit_depth_luma_minus8) + 8,
            ref, failures);

        check_int("bit_depth_chroma",
            static_cast<unsigned>(
                sps->bit_depth_chroma_minus8) + 8,
            ref, failures);

        if (sps->vui_parameters_present_flag &&
            sps->vui.video_signal_type_present_flag) {

            check_int("video_full_range_flag",
                sps->vui.video_full_range_flag ? 1u : 0u,
                ref, failures);

            if (sps->vui.colour_description_present_flag) {

                check_int("colour_primaries",
                    sps->vui.colour_primaries, ref, failures);

                check_int("transfer_characteristics",
                    sps->vui.transfer_characteristics,
                    ref, failures);

                check_int("matrix_coefficients",
                    sps->vui.matrix_coefficients,
                    ref, failures);
            }
        }
    }

    check_int("mastering_display_present",
        g_mastering_display_present ? 1u : 0u,
        ref, failures);

    check_int("max_content_light_level",
        g_max_content_light_level, ref, failures);

    check_int("max_pic_average_light_level",
        g_max_pic_average_light_level, ref, failures);

    if (failures.empty()) {
        std::cout << "PASS " << stream_path << "\n";
        return 0;
    }

    std::cout << "FAIL " << stream_path << "\n";

    for (const auto& failure : failures) {
        std::cout << "  " << failure << "\n";
    }

    return 1;
}

} // namespace


int main(int argc, char** argv)
{
    if (argc != 3) {
        std::cerr
            << "usage: ffmpeg_avc_test <stream.h264> <reference.txt>\n";
        return 2;
    }

    const auto ref = read_reference(argv[2]);

    if (ref.empty()) {
        std::cerr << "no reference data in " << argv[2] << "\n";
        return 2;
    }

    return run_stream(argv[1], ref);
}
