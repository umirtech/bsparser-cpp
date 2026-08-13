#pragma once

#include "avc_common.hpp"
#include "avc_parse_common.hpp"
#include "avc_pps.hpp"
#include "avc_slice_header.hpp"
#include "avc_sps.hpp"
#include "rbsp_bitstream_reader.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace bs {
namespace avc {

/*
 * -----------------------------------------------------------
 * AVC slice segment header parser (7.3.3.1)
 * -----------------------------------------------------------
 *
 * The parser requires the resolved SPS and PPS for the slice
 * (the PPS id is read from inside the header).
 */

namespace detail {

inline constexpr unsigned kMaxReorderingOps = 64;
inline constexpr unsigned kMaxMmcoOps = 64;


template <typename Reader>
inline RefPicListModification
parse_ref_pic_list_modification(
    Reader& reader,
    unsigned list_id,
    SliceType slice_type)
{
    RefPicListModification result;

    const bool is_i_or_si =
        slice_type == SliceType::I ||
        slice_type == SliceType::SI;

    const bool parse_list =
        (list_id == 0) ? !is_i_or_si
                       : (slice_type == SliceType::B);

    if (!parse_list) {
        return result;
    }

    result.modification_flag =
        reader.read_bit();

    if (!result.modification_flag) {
        return result;
    }

    for (;;) {

        if (result.ops.size() >= kMaxReorderingOps) {
            throw ParseError(
                "slice_header: ref_pic_list_reordering overrun");
        }

        RefPicReorderingOp op;

        op.reordering_of_pic_nums_idc =
            read_ue_max(
                reader,
                "reordering_of_pic_nums_idc",
                3);

        if (op.reordering_of_pic_nums_idc == 0 ||
            op.reordering_of_pic_nums_idc == 1) {

            op.abs_diff_pic_num_minus1 =
                read_ue_max(
                    reader,
                    "abs_diff_pic_num_minus1",
                    0xFFFFFu);

        } else if (op.reordering_of_pic_nums_idc == 2) {

            op.long_term_pic_num =
                read_ue_max(
                    reader,
                    "long_term_pic_num",
                    0xFFFFFu);
        }

        const bool done =
            op.reordering_of_pic_nums_idc == 3;

        result.ops.push_back(op);

        if (done) {
            break;
        }
    }

    return result;
}


template <typename Reader>
inline PredWeightTable
parse_pred_weight_table(
    Reader& reader,
    const SequenceParameterSet& sps,
    const SliceHeader& header)
{
    PredWeightTable table;

    const std::uint8_t num_l0 =
        static_cast<std::uint8_t>(
            header.num_ref_idx_l0_active_minus1 + 1);

    const std::uint8_t num_l1 =
        static_cast<std::uint8_t>(
            header.num_ref_idx_l1_active_minus1 + 1);

    table.luma_log2_weight_denom =
        read_ue_max(
            reader,
            "luma_log2_weight_denom",
            7);

    if (sps.chroma_format_idc != 0) {

        table.chroma_log2_weight_denom =
            read_ue_max(
                reader,
                "chroma_log2_weight_denom",
                7);
    }

    for (unsigned i = 0;
         i < num_l0;
         ++i) {

        table.luma_weight_l0_flag[i] =
            reader.read_bit();

        if (table.luma_weight_l0_flag[i]) {
            table.luma_weight_l0[i] = static_cast<std::int16_t>(
                read_se_bounded(
                    reader,
                    "luma_weight_l0",
                    128));
            table.luma_offset_l0[i] = static_cast<std::int16_t>(
                read_se_bounded(
                    reader,
                    "luma_offset_l0",
                    128));
        }
    }

    if (sps.chroma_format_idc != 0) {

        for (unsigned i = 0;
             i < num_l0;
             ++i) {

            table.chroma_weight_l0_flag[i] =
                reader.read_bit();

            if (table.chroma_weight_l0_flag[i]) {

                for (unsigned j = 0;
                     j < 2;
                     ++j) {

                    table.chroma_weight_l0[i][j] = static_cast<std::int16_t>(
                        read_se_bounded(
                            reader,
                            "chroma_weight_l0",
                            128));
                    table.chroma_offset_l0[i][j] = static_cast<std::int16_t>(
                        read_se_bounded(
                            reader,
                            "chroma_offset_l0",
                            128));
                }
            }
        }
    }

    if (header.slice_type == SliceType::B) {

        for (unsigned i = 0;
             i < num_l1;
             ++i) {

            table.luma_weight_l1_flag[i] =
                reader.read_bit();

            if (table.luma_weight_l1_flag[i]) {
                table.luma_weight_l1[i] = static_cast<std::int16_t>(
                    read_se_bounded(
                        reader,
                        "luma_weight_l1",
                        128));
                table.luma_offset_l1[i] = static_cast<std::int16_t>(
                    read_se_bounded(
                        reader,
                        "luma_offset_l1",
                        128));
            }
        }

        if (sps.chroma_format_idc != 0) {

            for (unsigned i = 0;
                 i < num_l1;
                 ++i) {

                table.chroma_weight_l1_flag[i] =
                    reader.read_bit();

                if (table.chroma_weight_l1_flag[i]) {

                    for (unsigned j = 0;
                         j < 2;
                         ++j) {

                        table.chroma_weight_l1[i][j] = static_cast<std::int16_t>(
                            read_se_bounded(
                                reader,
                                "chroma_weight_l1",
                                128));
                        table.chroma_offset_l1[i][j] = static_cast<std::int16_t>(
                            read_se_bounded(
                                reader,
                                "chroma_offset_l1",
                                128));
                    }
                }
            }
        }
    }

    return table;
}


template <typename Reader>
inline void
parse_dec_ref_pic_marking(
    Reader& reader,
    const NalUnitType nal_type,
    SliceHeader& header)
{
    if (is_idr_nal_unit(nal_type)) {

        header.no_output_of_prior_pics_flag =
            reader.read_bit();

        header.long_term_reference_flag =
            reader.read_bit();

        return;
    }

    header.adaptive_ref_pic_marking_mode_flag =
        reader.read_bit();

    if (!header.adaptive_ref_pic_marking_mode_flag) {
        return;
    }

    for (;;) {

        if (header.mmco_operations.size() >= kMaxMmcoOps) {
            throw ParseError(
                "slice_header: dec_ref_pic_marking overrun");
        }

        MmcoOperation op;

        op.memory_management_control_operation =
            read_ue_max(
                reader,
                "memory_management_control_operation",
                6);

        if (op.memory_management_control_operation == 1 ||
            op.memory_management_control_operation == 3) {

            op.difference_of_pic_nums_minus1 =
                read_ue_max(
                    reader,
                    "difference_of_pic_nums_minus1",
                    0xFFFFFu);
        }

        if (op.memory_management_control_operation == 2) {

            op.long_term_pic_num =
                read_ue_max(
                    reader,
                    "long_term_pic_num",
                    0xFFFFFu);
        }

        if (op.memory_management_control_operation == 3 ||
            op.memory_management_control_operation == 6) {

            op.long_term_frame_idx =
                read_ue_max(
                    reader,
                    "long_term_frame_idx",
                    0xFFFFFu);
        }

        if (op.memory_management_control_operation == 4) {

            op.max_long_term_frame_idx_plus1 =
                read_ue_max(
                    reader,
                    "max_long_term_frame_idx_plus1",
                    0xFFFFFu);
        }

        const bool done =
            op.memory_management_control_operation == 0;

        header.mmco_operations.push_back(op);

        if (done) {
            break;
        }
    }
}

} // namespace detail


/*
 * -----------------------------------------------------------
 * Parse the AVC slice segment header
 * -----------------------------------------------------------
 */

template <typename Reader>
[[nodiscard]]
inline SliceHeader
parse_slice_header(
    Reader& reader,
    const SequenceParameterSet& sps,
    const PictureParameterSet& pps,
    NalUnitType nal_type,
    std::uint8_t nal_ref_idc)
{
    SliceHeader header;

    header.first_mb_in_slice =
        detail::read_ue_max(
            reader,
            "first_mb_in_slice",
            0xFFFFFu);

    const std::uint32_t raw_slice_type =
        detail::read_ue_max(
            reader,
            "slice_type",
            9);

    header.slice_type =
        decode_slice_type(raw_slice_type);

    header.pic_parameter_set_id = static_cast<std::uint8_t>(
        detail::read_ue_max(
            reader,
            "pic_parameter_set_id",
            255));

    header.frame_num = static_cast<std::uint32_t>(
        reader.read_bits(
            sps.log2_max_frame_num()));

    if (!sps.frame_mbs_only_flag) {

        header.field_pic_flag =
            reader.read_bit();

        if (header.field_pic_flag) {
            header.bottom_field_flag =
                reader.read_bit();
        }
    }

    if (is_idr_nal_unit(nal_type)) {

        header.idr_pic_id =
            detail::read_ue_max(
                reader,
                "idr_pic_id",
                0xFFFFFu);
    }

    if (sps.pic_order_cnt_type == 0) {

        header.pic_order_cnt_lsb = static_cast<std::uint32_t>(
            reader.read_bits(
                sps.log2_max_pic_order_cnt_lsb()));

        if (pps.bottom_field_pic_order_in_frame_present_flag &&
            !header.field_pic_flag) {

            header.delta_pic_order_cnt_bottom =
                reader.read_se();
        }

    } else if (sps.pic_order_cnt_type == 1 &&
               !sps.delta_pic_order_always_zero_flag) {

        header.delta_pic_order_cnt[0] =
            reader.read_se();

        if (pps.bottom_field_pic_order_in_frame_present_flag &&
            !header.field_pic_flag) {

            header.delta_pic_order_cnt[1] =
                reader.read_se();
        }
    }

    if (pps.redundant_pic_cnt_present_flag) {

        header.redundant_pic_cnt =
            detail::read_ue_max(
                reader,
                "redundant_pic_cnt",
                0xFFFFFu);
    }

    const bool is_b =
        header.slice_type == SliceType::B;

    if (is_b) {
        header.direct_spatial_mv_pred_flag =
            reader.read_bit();
    }

    const bool uses_ref_pic_lists =
        header.slice_type == SliceType::P ||
        header.slice_type == SliceType::SP ||
        is_b;

    if (uses_ref_pic_lists) {

        header.num_ref_idx_active_override_flag =
            reader.read_bit();

        if (header.num_ref_idx_active_override_flag) {

            header.num_ref_idx_l0_active_minus1 = static_cast<std::uint8_t>(
                detail::read_ue_max(
                    reader,
                    "num_ref_idx_l0_active_minus1",
                    31));

            if (is_b) {
                header.num_ref_idx_l1_active_minus1 = static_cast<std::uint8_t>(
                    detail::read_ue_max(
                        reader,
                        "num_ref_idx_l1_active_minus1",
                        31));
            }

        } else {

            /*
             * 7.4.3.1: with the override flag clear, NumRefIdxActive
             * is inferred from the PPS defaults.
             */
            header.num_ref_idx_l0_active_minus1 =
                pps.num_ref_idx_l0_default_active_minus1;

            if (is_b) {
                header.num_ref_idx_l1_active_minus1 =
                    pps.num_ref_idx_l1_default_active_minus1;
            }
        }

        header.ref_pic_list_modification_l0 =
            detail::parse_ref_pic_list_modification(
                reader,
                0,
                header.slice_type);

        if (is_b) {

            header.ref_pic_list_modification_l1 =
                detail::parse_ref_pic_list_modification(
                    reader,
                    1,
                    header.slice_type);
        }

        if ((pps.weighted_pred_flag &&
             header.slice_type == SliceType::P) ||
            (pps.weighted_bipred_idc == 1 &&
             is_b)) {

            header.pred_weight_table =
                detail::parse_pred_weight_table(
                    reader,
                    sps,
                    header);
        }
    }

    if (nal_ref_idc != 0) {

        detail::parse_dec_ref_pic_marking(
            reader,
            nal_type,
            header);
    }

    const bool is_i =
        header.slice_type == SliceType::I;

    const bool is_si =
        header.slice_type == SliceType::SI;

    if (pps.entropy_coding_mode_flag &&
        !is_i &&
        !is_si) {

        header.cabac_init_idc = static_cast<std::uint8_t>(
            detail::read_ue_max(
                reader,
                "cabac_init_idc",
                2));
    }

    header.slice_qp_delta =
        detail::read_se_bounded(
            reader,
            "slice_qp_delta",
            52);

    if (pps.deblocking_filter_control_present_flag) {

        header.disable_deblocking_filter_idc = static_cast<std::uint8_t>(
            detail::read_ue_max(
                reader,
                "disable_deblocking_filter_idc",
                2));

        if (header.disable_deblocking_filter_idc != 1) {

            header.slice_alpha_c0_offset_div2 =
                detail::read_se_bounded(
                    reader,
                    "slice_alpha_c0_offset_div2",
                    52);

            header.slice_beta_offset_div2 =
                detail::read_se_bounded(
                    reader,
                    "slice_beta_offset_div2",
                    52);
        }
    }

    if (pps.num_slice_groups_minus1 > 0 &&
        pps.slice_group_map.slice_group_map_type >= 3 &&
        pps.slice_group_map.slice_group_map_type <= 5) {

        const unsigned bits =
            detail::ceil_log2(
                pps.slice_group_map.slice_group_change_rate_minus1 + 1);

        header.slice_group_change_cycle =
            static_cast<std::uint32_t>(
                reader.read_bits(bits));
    }

    if (header.slice_type == SliceType::SP ||
        header.slice_type == SliceType::SI) {

        header.sp_for_switch_flag =
            reader.read_bit();

        if (header.slice_type == SliceType::SP) {

            header.slice_qs_delta =
                detail::read_se_bounded(
                    reader,
                    "slice_qs_delta",
                    52);
        }

        if (header.slice_type == SliceType::SI) {

            header.slice_qs_delta =
                detail::read_se_bounded(
                    reader,
                    "slice_qs_delta",
                    52);
        }
    }

    return header;
}

} // namespace avc
} // namespace bs