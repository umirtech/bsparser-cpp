// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

/*
 * ---------------------------------------------------------------------------
 * HEVC parser validation against pre-generated reference data
 * ---------------------------------------------------------------------------
 *
 * Usage:
 *
 *     hevc_hdr_test <stream.hevc> <reference.txt>
 *
 * The stream is parsed with the bsparser library.  A set of decoded
 * SPS / VUI / SEI values is then compared against the reference file,
 * which contains the expected values for the stream.
 *
 * The reference file is a set of "key=value" lines.  Lines beginning
 * with '#' are ignored.
 *
 * Exit status:
 *
 *     0   every reference key matched the parsed value
 *     1   one or more mismatches, or the stream could not be parsed
 *
 * The values compared (all in raw HEVC syntax units):
 *
 *     profile_idc               general_profile_idc
 *     level_idc                 general_level_idc
 *     coded_width               pic_width_in_luma_samples
 *     coded_height              pic_height_in_luma_samples
 *     display_width             coded width minus conformance window
 *     display_height            coded height minus conformance window
 *     chroma_format_idc         0..3
 *     bit_depth_luma            bit_depth_luma_minus8 + 8
 *     bit_depth_chroma          bit_depth_chroma_minus8 + 8
 *     colour_primaries          VUI colour_primaries
 *     transfer_characteristics  VUI transfer_characteristics
 *     matrix_coefficients       VUI matrix_coefficients
 *     video_full_range_flag     VUI video_full_range_flag
 *     mastering_display_present whether SEI payload 137 was seen
 *     max_content_light_level   SEI payload 144 MaxCLL
 *     max_pic_average_light_level  SEI payload 144 MaxFALL
 */

#include "hevc_nal_parser.hpp"
#include "hevc_sps_parser.hpp"
#include "hevc_sei_parser.hpp"
#include "hevc_parameter_set_manager.hpp"
#include "rbsp_bitstream_reader.hpp"
#include "log.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

bs::ParameterSetManager g_parameter_sets;

bool g_mastering_display_present = false;
std::uint32_t g_max_content_light_level = 0;
std::uint32_t g_max_pic_average_light_level = 0;

/*
 * Read a "key=value" reference file.
 */
std::unordered_map<std::string, std::string> read_reference(const std::string& path) {
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

        result[line.substr(0, eq)] = line.substr(eq + 1);
    }

    return result;
}

/*
 * Find a reference value by key.
 */
const std::string* ref_value(
    const std::unordered_map<std::string, std::string>& ref, const std::string& key
) {
    const auto it = ref.find(key);

    if (it == ref.end()) {
        return nullptr;
    }

    return &it->second;
}

/*
 * Compare a parsed integer against the reference.
 */
bool check_int(
    const std::string& key,
    unsigned long long parsed,
    const std::unordered_map<std::string, std::string>& ref,
    std::vector<std::string>& failures
) {
    const std::string* expected = ref_value(ref, key);

    if (expected == nullptr) {
        return true;
    }

    try {
        const auto want = std::stoull(*expected);

        if (parsed == want) {
            return true;
        }

        failures.push_back(key + ": parser=" + std::to_string(parsed) + " reference=" + *expected);
        return false;
    } catch (...) {
        failures.push_back(key + ": reference value not an integer: " + *expected);
        return false;
    }
}

/*
 * Parse a stream and run the checks.
 */
int run_stream(
    const std::string& stream_path, const std::unordered_map<std::string, std::string>& ref
) {
    std::ifstream in(stream_path, std::ios::binary);

    if (!in) {
        std::cerr << "cannot open stream: " << stream_path << "\n";
        return 1;
    }

    std::vector<std::uint8_t> annex_b{
        std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()
    };

    in.close();

    std::vector<std::string> failures;

    bs::BsNalHandlers handlers;

    handlers.sps = [](const bs::NalUnit& nal) {
        const auto payload = nal.payload_bytes();

        const auto span = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(payload.data()), payload.size()
        );

        bs::RbspBitstreamReader reader(span);

        auto sps = bs::parse_sequence_parameter_set(reader);

        (void)g_parameter_sets.store_sps(std::move(sps));
    };

    handlers.prefix_sei = [](const bs::NalUnit& nal) {
        const auto sei = bs::parse_sei_nal(nal);

        bs::MasteringDisplayColourVolume md{};

        if (bs::get_mastering_display_colour_volume(sei.view, md)) {
            g_mastering_display_present = true;
        }

        bs::ContentLightLevelInfo cll{};

        if (bs::get_content_light_level_info(sei.view, cll)) {
            g_max_content_light_level = cll.max_content_light_level;

            g_max_pic_average_light_level = cll.max_pic_average_light_level;
        }
    };

    handlers.suffix_sei = [](const bs::NalUnit& nal) {
        const auto sei = bs::parse_sei_nal(nal);

        bs::MasteringDisplayColourVolume md{};

        if (bs::get_mastering_display_colour_volume(sei.view, md)) {
            g_mastering_display_present = true;
        }

        bs::ContentLightLevelInfo cll{};

        if (bs::get_content_light_level_info(sei.view, cll)) {
            g_max_content_light_level = cll.max_content_light_level;

            g_max_pic_average_light_level = cll.max_pic_average_light_level;
        }
    };

    std::span<const std::uint8_t> bytes{annex_b.data(), annex_b.size()};

    try {
        (void)bs::dispatch_nals(bytes, bs::NalFramingMode::AnnexB, handlers);
    } catch (const std::exception& e) {
        failures.push_back(std::string("dispatch threw: ") + e.what());
    }

    if (g_parameter_sets.sps_count() == 0) {
        failures.push_back("no SPS parsed");
    }

    for (std::uint8_t id = 0; id < 16; ++id) {
        const auto* sps = g_parameter_sets.find_sps(id);

        if (sps == nullptr) {
            continue;
        }

        const auto& ptl = sps->profile_tier_level;

        check_int("profile_idc", ptl.general_profile_idc, ref, failures);

        check_int("level_idc", ptl.general_level_idc, ref, failures);

        check_int("coded_width", sps->pic_width_in_luma_samples, ref, failures);

        check_int("coded_height", sps->pic_height_in_luma_samples, ref, failures);

        check_int("display_width", sps->geometry.display_width, ref, failures);

        check_int("display_height", sps->geometry.display_height, ref, failures);

        check_int(
            "chroma_format_idc",
            static_cast<unsigned>(static_cast<std::uint8_t>(sps->chroma_format)),
            ref,
            failures
        );

        check_int("bit_depth_luma", sps->luma_bit_depth(), ref, failures);

        check_int("bit_depth_chroma", sps->chroma_bit_depth(), ref, failures);

        if (sps->has_vui()) {
            const auto& signal = sps->vui.video_signal;

            if (signal.present) {
                check_int(
                    "video_full_range_flag", signal.video_full_range_flag ? 1 : 0, ref, failures
                );

                if (signal.colour.present) {
                    check_int("colour_primaries", signal.colour.colour_primaries, ref, failures);

                    check_int(
                        "transfer_characteristics",
                        signal.colour.transfer_characteristics,
                        ref,
                        failures
                    );

                    check_int(
                        "matrix_coefficients", signal.colour.matrix_coefficients, ref, failures
                    );
                }
            }
        }
    }

    check_int("mastering_display_present", g_mastering_display_present ? 1 : 0, ref, failures);

    check_int("max_content_light_level", g_max_content_light_level, ref, failures);

    check_int("max_pic_average_light_level", g_max_pic_average_light_level, ref, failures);

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

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: hevc_hdr_test <stream.hevc> <reference.txt>\n";
        return 2;
    }

    const auto ref = read_reference(argv[2]);

    if (ref.empty()) {
        std::cerr << "no reference data in " << argv[2] << "\n";
        return 2;
    }

    return run_stream(argv[1], ref);
}
