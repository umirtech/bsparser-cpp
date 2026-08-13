#pragma once

#include "avc_parse_common.hpp"
#include "avc_pps.hpp"
#include "avc_sps_parser.hpp"
#include "rbsp_bitstream_reader.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace bs {
namespace avc {

/*
 * -----------------------------------------------------------
 * AVC picture parameter set parser (7.3.2.2)
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline PictureParameterSet
parse_picture_parameter_set(
    RbspBitstreamReader& reader)
{
    PictureParameterSet pps;

    pps.pic_parameter_set_id = static_cast<std::uint8_t>(
        detail::read_ue_max(
            reader,
            "pic_parameter_set_id",
            255));

    pps.seq_parameter_set_id = static_cast<std::uint8_t>(
        detail::read_ue_max(
            reader,
            "seq_parameter_set_id",
            31));

    pps.entropy_coding_mode_flag =
        reader.read_bit();

    pps.bottom_field_pic_order_in_frame_present_flag =
        reader.read_bit();

    pps.num_slice_groups_minus1 = static_cast<std::uint8_t>(
        detail::read_ue_max(
            reader,
            "num_slice_groups_minus1",
            7));

    if (pps.num_slice_groups_minus1 > 0) {

        auto& map =
            pps.slice_group_map;

        map.slice_group_map_type = static_cast<std::uint8_t>(
            detail::read_ue_max(
                reader,
                "slice_group_map_type",
                6));

        const std::uint8_t num_slice_groups =
            pps.num_slice_groups();

        switch (map.slice_group_map_type) {

        case 0:
            for (unsigned i = 0;
                 i < num_slice_groups;
                 ++i) {

                map.run_length_minus1[i] =
                    detail::read_ue_max(
                        reader,
                        "run_length_minus1",
                        0xFFFFFu);
            }
            break;

        case 2:
            for (unsigned i = 0;
                 i < num_slice_groups - 1;
                 ++i) {

                map.top_left[i] =
                    detail::read_ue_max(
                        reader,
                        "top_left",
                        0xFFFFFu);

                map.bottom_right[i] =
                    detail::read_ue_max(
                        reader,
                        "bottom_right",
                        0xFFFFFu);
            }
            break;

        case 3:
        case 4:
        case 5:
            map.slice_group_change_direction_flag =
                reader.read_bit();

            map.slice_group_change_rate_minus1 =
                detail::read_ue_max(
                    reader,
                    "slice_group_change_rate_minus1",
                    0xFFFFFu);
            break;

        case 6:
            map.pic_size_in_map_units_minus1 =
                detail::read_ue_max(
                    reader,
                    "pic_size_in_map_units_minus1",
                    0xFFFFFu);

            {
                const std::uint32_t pic_size =
                    map.pic_size_in_map_units_minus1 + 1;

                const unsigned bits =
                    detail::ceil_log2(
                        pps.num_slice_groups());

                map.slice_group_id.reserve(pic_size);

                for (std::uint32_t i = 0;
                     i < pic_size;
                     ++i) {

                    map.slice_group_id.push_back(
                        static_cast<std::uint8_t>(
                            reader.read_bits(bits)));
                }
            }
            break;

        default:
            throw ParseError(
                "pic_parameter_set: invalid slice_group_map_type");
        }
    }

    pps.num_ref_idx_l0_default_active_minus1 = static_cast<std::uint8_t>(
        detail::read_ue_max(
            reader,
            "num_ref_idx_l0_default_active_minus1",
            31));

    pps.num_ref_idx_l1_default_active_minus1 = static_cast<std::uint8_t>(
        detail::read_ue_max(
            reader,
            "num_ref_idx_l1_default_active_minus1",
            31));

    pps.weighted_pred_flag =
        reader.read_bit();

    pps.weighted_bipred_idc = static_cast<std::uint8_t>(
        reader.read_bits(2));

    pps.pic_init_qp_minus26 =
        detail::read_se_bounded(
            reader,
            "pic_init_qp_minus26",
            52);

    pps.pic_init_qs_minus26 =
        detail::read_se_bounded(
            reader,
            "pic_init_qs_minus26",
            52);

    pps.chroma_qp_index_offset =
        detail::read_se_bounded(
            reader,
            "chroma_qp_index_offset",
            52);

    pps.deblocking_filter_control_present_flag =
        reader.read_bit();

    pps.constrained_intra_pred_flag =
        reader.read_bit();

    pps.redundant_pic_cnt_present_flag =
        reader.read_bit();

    if (reader.more_rbsp_data()) {

        pps.transform_8x8_mode_flag =
            reader.read_bit();

        pps.pic_scaling_matrix_present_flag =
            reader.read_bit();

        if (pps.pic_scaling_matrix_present_flag) {

            for (unsigned i = 0;
                 i < 8;
                 ++i) {

                const bool present =
                    reader.read_bit();

                if (!present) {
                    continue;
                }

                if (i < 6) {
                    pps.pic_scaling_lists.present_4x4[i] = true;
                    detail::parse_scaling_list(
                        reader,
                        16,
                        static_cast<std::uint8_t>(i),
                        pps.pic_scaling_lists.list_4x4[i].data());
                } else {
                    const unsigned idx = i - 6;
                    pps.pic_scaling_lists.present_8x8[idx] = true;
                    detail::parse_scaling_list(
                        reader,
                        64,
                        static_cast<std::uint8_t>(i),
                        pps.pic_scaling_lists.list_8x8[idx].data());
                }
            }
        }

        pps.second_chroma_qp_index_offset =
            detail::read_se_bounded(
                reader,
                "second_chroma_qp_index_offset",
                52);
    }

    reader.read_rbsp_trailing_bits();

    return pps;
}


/*
 * -----------------------------------------------------------
 * Try variant + validation
 * -----------------------------------------------------------
 */

struct AvcPpsParseResult {
    PictureParameterSet pps{};
    bool ok = false;
};


[[nodiscard]]
inline AvcPpsParseResult
try_parse_picture_parameter_set(
    RbspBitstreamReader& reader)
{
    try {
        return AvcPpsParseResult{
            parse_picture_parameter_set(reader),
            true
        };
    }
    catch (const std::exception&) {
        return AvcPpsParseResult{};
    }
}


[[nodiscard]]
inline bool
validate_picture_parameter_set(
    const PictureParameterSet& pps) noexcept
{
    return pps.seq_parameter_set_id < kMaxSpsCount;
}

} // namespace avc
} // namespace bs