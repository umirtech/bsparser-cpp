#pragma once

#include "hevc_common.hpp"
#include "hevc_pps.hpp"
#include "hevc_sps.hpp"
#include "hevc_short_term_ref_pic_set.hpp"
#include "hevc_slice_header.hpp"
#include "hevc_reference_picture_manager.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include "log.hpp"

namespace bs {

/*
 * H.265 slice_segment_header() parser.
 *
 * The parser operates directly on RbspBitstreamReader.
 *
 * The parser does not own the SPS/PPS and does not modify
 * parameter sets.
 *
 * Syntax reference:
 *
 *     7.3.6.1 slice segment header syntax
 *     7.3.7.1 slice segment header semantics
 */


/*
 * -----------------------------------------------------------
 * Parser error
 * -----------------------------------------------------------
 */

class SliceSegmentHeaderParseError
    : public std::runtime_error
{
public:
    explicit SliceSegmentHeaderParseError(
        const char* message)
        : std::runtime_error(message)
    {
    }
};


/*
 * -----------------------------------------------------------
 * Generic reader helpers
 * -----------------------------------------------------------
 *
 * RbspBitstreamReader already exposes the required syntax
 * primitives. These wrappers keep the parser readable.
 */

template <typename Reader>
[[nodiscard]]
inline bool read_flag(
    Reader& reader)
{
    return reader.read_bit();
}


template <typename Reader>
[[nodiscard]]
inline std::uint32_t read_u(
    Reader& reader,
    unsigned width)
{
    if (width == 0) {
        return 0;
    }

    if (width > 32) {
        throw SliceSegmentHeaderParseError(
            "slice parser: u(v) width > 32");
    }

    return static_cast<std::uint32_t>(
        reader.read_bits(width));
}


template <typename Reader>
[[nodiscard]]
inline std::uint32_t read_ue_checked(
    Reader& reader)
{
    return reader.read_ue();
}


template <typename Reader>
[[nodiscard]]
inline std::int32_t read_se_checked(
    Reader& reader)
{
    return reader.read_se();
}



inline void derive_num_poc_total_curr(
    const SequenceParameterSet& sps,
    SliceSegmentHeader& header)
{
    ReferencePictureList rps{};

    if (header.short_term_ref_pic_set_sps_flag) {

        const auto index =
            static_cast<std::size_t>(
                header.short_term_ref_pic_set_idx);

        rps =
            ReferencePictureManager{}
                .build_short_term_rps(
                    sps.reference_picture_sets
                        .short_term_ref_pic_sets,
                    index,
                    header.derived_poc);

    } else {

        /*
         * Slice-level RPS.
         *
         * If it is inter-predicted, the reference SPS RPS
         * must be resolved first.
         */
        if (header.short_term_ref_pic_set
                .inter_ref_pic_set_prediction_flag) {

            const auto ref_idx =
                static_cast<std::size_t>(
                    header.short_term_ref_pic_set
                        .inter_prediction
                        .reference_rps_idx);

            if (ref_idx >=
                sps.reference_picture_sets
                    .short_term_ref_pic_sets.size()) {

                throw SliceSegmentHeaderParseError(
                    "slice parser: invalid slice RPS reference index");
            }

            rps =
                ReferencePictureManager{}
                    .build_inter_predicted_list(
                        header.short_term_ref_pic_set,
                        sps.reference_picture_sets
                            .short_term_ref_pic_sets[ref_idx],
                        header.derived_poc);

        } else {

            rps =
                ReferencePictureManager{}
                    .build_explicit_short_term_list(
                        header.short_term_ref_pic_set,
                        header.derived_poc);
        }
    }

    std::uint32_t count = 0;

    for (const auto& reference : rps.entries) {
        if (reference.used_by_curr_pic) {
            ++count;
        }
    }

    /*
     * Long-term references selected for the current picture
     * are also part of NumPocTotalCurr.
     */
    for (const auto& lt :
         header.long_term_references) {

        if (lt.used_by_curr_pic_lt_flag) {
            ++count;
        }
    }

    header.reference_pictures
        .num_poc_total_curr = count;
}



/*
 * ceil(log2(x))
 *
 * H.265 uses this repeatedly for syntax such as:
 *
 *     CeilLog2( PicSizeInCtbsY )
 */
[[nodiscard]]
constexpr unsigned ceil_log2(
    std::uint64_t value) noexcept
{
    if (value <= 1) {
        return 0;
    }

    unsigned result = 0;
    std::uint64_t n = value - 1;

    while (n != 0) {
        n >>= 1;
        ++result;
    }

    return result;
}


/*
 * -----------------------------------------------------------
 * SPS geometry helpers
 * -----------------------------------------------------------
 */


/*
 * PicWidthInCtbsY
 *
 * PicWidthInCtbsY =
 *
 *     Ceil(
 *         pic_width_in_luma_samples /
 *         CtbSizeY
 *     )
 */
[[nodiscard]]
inline std::uint32_t
pic_width_in_ctbs_y(
    const SequenceParameterSet& sps)
{
    const auto ctb_size =
        sps.coding_blocks.max_luma_coding_block_size();

    if (ctb_size == 0) {
        throw SliceSegmentHeaderParseError(
            "slice parser: invalid CTB size");
    }

    return
        (sps.pic_width_in_luma_samples +
         ctb_size - 1) /
        ctb_size;
}


/*
 * PicHeightInCtbsY.
 */
[[nodiscard]]
inline std::uint32_t
pic_height_in_ctbs_y(
    const SequenceParameterSet& sps)
{
    const auto ctb_size =
        sps.coding_blocks.max_luma_coding_block_size();

    if (ctb_size == 0) {
        throw SliceSegmentHeaderParseError(
            "slice parser: invalid CTB size");
    }

    return
        (sps.pic_height_in_luma_samples +
         ctb_size - 1) /
        ctb_size;
}


/*
 * PicSizeInCtbsY.
 */
[[nodiscard]]
inline std::uint64_t
pic_size_in_ctbs_y(
    const SequenceParameterSet& sps)
{
    return
        static_cast<std::uint64_t>(
            pic_width_in_ctbs_y(sps)) *
        static_cast<std::uint64_t>(
            pic_height_in_ctbs_y(sps));
}


/*
 * Number of bits used for slice_segment_address.
 */
[[nodiscard]]
inline unsigned
slice_segment_address_bits(
    const SequenceParameterSet& sps)
{
    return ceil_log2(
        pic_size_in_ctbs_y(sps));
}


/*
 * -----------------------------------------------------------
 * Short-term RPS parser
 * -----------------------------------------------------------
 *
 * Parses:
 *
 *     short_term_ref_pic_set( stRpsIdx )
 *
 * This function handles both:
 *
 *     explicit RPS
 *
 * and:
 *
 *     inter-RPS predicted RPS
 *
 * The resulting structure retains the signaled syntax.
 */

template <typename Reader>
inline ShortTermRefPicSet
parse_short_term_ref_pic_set(
    Reader& reader,
    const SequenceParameterSet& sps,
    std::uint32_t st_rps_idx,
    std::uint32_t num_short_term_ref_pic_sets)
{
    ShortTermRefPicSet rps{};

    rps.index = st_rps_idx;

    /*
     * inter_ref_pic_set_prediction_flag
     *
     * Present only when stRpsIdx != 0.
     */
    if (st_rps_idx != 0) {
        rps.inter_ref_pic_set_prediction_flag =
            read_flag(reader);
    }

    if (!rps.inter_ref_pic_set_prediction_flag) {

        /*
         * Explicit RPS.
         */
        rps.num_negative_pics =
            read_ue_checked(reader);

        rps.num_positive_pics =
            read_ue_checked(reader);

        BS_LOG_TRACE("RPS bits after counts = "
          << reader.position()
          << " neg=" << rps.num_negative_pics
          << " pos=" << rps.num_positive_pics
          << '\n');

        /*
         * Protect vector allocations against obviously
         * impossible streams.
         */
        if (rps.num_negative_pics >
                65535 ||
            rps.num_positive_pics >
                65535) {

            throw SliceSegmentHeaderParseError(
                "slice parser: unreasonable RPS size");
        }

        rps.negative_pics.resize(
            rps.num_negative_pics);

        rps.positive_pics.resize(
            rps.num_positive_pics);

        for (std::uint32_t i = 0;
             i < rps.num_negative_pics;
             ++i) {

            auto& pic =
                rps.negative_pics[i];

            BS_LOG_TRACE("RPS negative[" << i << "] before delta = "
                    << reader.position() << '\n');

            pic.delta_poc_minus1 =
                read_ue_checked(reader);

            BS_LOG_TRACE("RPS negative[" << i << "] after delta = "
                    << reader.position()
                    << " delta=" << pic.delta_poc_minus1
                    << '\n');

            pic.used_by_curr_pic =
                read_flag(reader);

            BS_LOG_TRACE("RPS negative[" << i << "] after used = "
                    << reader.position()
                    << " used=" << pic.used_by_curr_pic
                    << '\n');
        }

        for (std::uint32_t i = 0;
             i < rps.num_positive_pics;
             ++i) {

            auto& pic =
                rps.positive_pics[i];

            BS_LOG_TRACE("RPS positive[" << i << "] before delta = "
                    << reader.position() << '\n');

            pic.delta_poc_minus1 =
                read_ue_checked(reader);

            BS_LOG_TRACE("RPS positive[" << i << "] after delta = "
                    << reader.position()
                    << " delta=" << pic.delta_poc_minus1
                    << '\n');

            pic.used_by_curr_pic =
                read_flag(reader);

            BS_LOG_TRACE("RPS positive[" << i << "] after used = "
                    << reader.position()
                    << " used=" << pic.used_by_curr_pic
                    << '\n');
        }

        derive_explicit_rps(rps);

        return rps;
    }


    /*
     * -------------------------------------------------------
     * Inter-RPS prediction
     * -------------------------------------------------------
     */

    if (st_rps_idx == num_short_term_ref_pic_sets) {

        rps.inter_prediction.delta_idx_present = true;

        rps.inter_prediction.delta_idx_minus1 =
            read_ue_checked(reader);
    }


    /*
     * Derive RefRpsIdx.
     *
     * For the normal SPS RPS array:
     *
     *     RefRpsIdx =
     *         stRpsIdx - delta_idx_minus1 - 1
     *
     * For an additional slice-level RPS:
     *
     *     RefRpsIdx =
     *         stRpsIdx - delta_idx_minus1 - 1
     *
     * where stRpsIdx == num_short_term_ref_pic_sets.
     */
    const std::uint64_t delta_idx =
        static_cast<std::uint64_t>(
            rps.inter_prediction
                .delta_idx_minus1) + 1;

    if (delta_idx > st_rps_idx) {
        throw SliceSegmentHeaderParseError(
            "slice parser: invalid RPS delta_idx");
    }

    const std::uint32_t ref_rps_idx =
        st_rps_idx -
        static_cast<std::uint32_t>(
            delta_idx);

    rps.inter_prediction.reference_rps_idx =
        ref_rps_idx;


    /*
     * For the slice-level additional RPS the reference must
     * come from the SPS RPS array.
     */
    if (ref_rps_idx >=
        sps.reference_picture_sets
            .short_term_ref_pic_sets.size()) {

        throw SliceSegmentHeaderParseError(
            "slice parser: invalid RefRpsIdx");
    }


    const auto& reference_rps =
        sps.reference_picture_sets
            .short_term_ref_pic_sets[
                ref_rps_idx];


    rps.inter_prediction.delta_rps_sign =
        read_flag(reader);

    rps.inter_prediction.abs_delta_rps_minus1 =
        read_ue_checked(reader);

    rps.inter_prediction.delta_rps =
        calculate_delta_rps(
            rps.inter_prediction
                .delta_rps_sign,
            rps.inter_prediction
                .abs_delta_rps_minus1);


    const auto entry_count =
        inter_rps_prediction_entry_count(
            reference_rps.num_delta_pocs);

    rps.inter_prediction.entries.resize(
        entry_count);


    for (std::size_t j = 0;
         j < entry_count;
         ++j) {

        auto& entry =
            rps.inter_prediction.entries[j];

        entry.used_by_curr_pic_flag =
            read_flag(reader);

        if (!entry.used_by_curr_pic_flag) {
            entry.use_delta_flag =
                read_flag(reader);
        } else {
            /*
             * Syntax does not carry use_delta_flag in this
             * case. Keep the model default false.
             */
            entry.use_delta_flag = false;
        }
    }


    /*
     * NumDeltaPocs for an inter-predicted RPS is derived from
     * the prediction process. For the syntax model, the
     * reference count is a useful conservative value.
     *
     * Full semantic construction is intentionally left to
     * the RPS decoder layer.
     */
    rps.num_delta_pocs =
        reference_rps.num_delta_pocs + 1;

    return rps;
}


/*
 * -----------------------------------------------------------
 * Long-term reference parser
 * -----------------------------------------------------------
 */

template <typename Reader>
inline void parse_long_term_references(
    Reader& reader,
    const SequenceParameterSet& sps,
    SliceSegmentHeader& header)
{
    const auto& rps =
        sps.reference_picture_sets;

    if (!rps.long_term_ref_pics_present_flag) {
        return;
    }


    const auto sps_lt_count =
        rps.num_long_term_ref_pics_sps;


    /*
     * num_long_term_sps
     *
     * Number of references selected from the SPS list.
     */
    if (sps_lt_count != 0) {

        header.num_long_term_sps =
            read_ue_checked(reader);

        if (header.num_long_term_sps >
            sps_lt_count) {

            throw SliceSegmentHeaderParseError(
                "slice parser: num_long_term_sps exceeds SPS");
        }

    } else {
        header.num_long_term_sps = 0;
    }


    /*
     * num_long_term_pics
     */
    header.num_long_term_pics =
        read_ue_checked(reader);


    initialize_slice_long_term_references(
        header);


    const unsigned lt_idx_bits =
        ceil_log2(
            sps_lt_count);


    const unsigned poc_lsb_bits =
        static_cast<unsigned>(
            sps.log2_max_pic_order_cnt_lsb_minus4) +
        4;


    for (std::size_t i = 0;
         i < header.long_term_references.size();
         ++i) {

        auto& lt =
            header.long_term_references[i];


        if (i <
            header.num_long_term_sps) {

            /*
             * lt_idx_sps
             */
            if (lt_idx_bits != 0) {
                lt.lt_idx_sps =
                    read_u(
                        reader,
                        lt_idx_bits);
            } else {
                lt.lt_idx_sps = 0;
            }

            /*
             * poc_lsb_lt and used flag are still explicitly
             * signaled for the slice syntax.
             */
            lt.poc_lsb_lt =
                read_u(
                    reader,
                    poc_lsb_bits);

        } else {

            /*
             * Slice-signaled long-term picture.
             */
            lt.poc_lsb_lt =
                read_u(
                    reader,
                    poc_lsb_bits);
        }


        lt.used_by_curr_pic_lt_flag =
            read_flag(reader);

        lt.delta_poc_msb_present_flag =
            read_flag(reader);

        if (lt.delta_poc_msb_present_flag) {

            lt.delta_poc_msb_cycle_lt =
                read_ue_checked(reader);
        }
    }
}


/*
 * -----------------------------------------------------------
 * Reference-list modification parser
 * -----------------------------------------------------------
 */

template <typename Reader>
inline void parse_ref_pic_list_modification(
    Reader& reader,
    const SequenceParameterSet& sps,
    const PictureParameterSet& pps,
    SliceSegmentHeader& header)
{
    if (!pps.lists_modification_present_flag) {
        return;
    }


    const auto num_poc_total_curr =
        header.reference_pictures
            .num_poc_total_curr;


    if (num_poc_total_curr <= 1) {
        return;
    }


    const unsigned list_entry_bits =
        ceil_log2(num_poc_total_curr);


    /*
     * list_entry_l0
     */
    if (header.slice_type == SliceType::P ||
        header.slice_type == SliceType::B) {

        header.reference_pictures
            .list_modification
            .list0.modification_flag =
            read_flag(reader);


        if (header.reference_pictures
                .list_modification
                .list0.modification_flag) {

            const auto count =
                header.effective_num_ref_idx_l0;

            auto& entries =
                header.reference_pictures
                    .list_modification
                    .list0.list_entry;

            entries.resize(count);

            for (std::uint32_t i = 0;
                 i < count;
                 ++i) {

                entries[i] =
                    read_u(
                        reader,
                        list_entry_bits);
            }
        }
    }


    /*
     * list_entry_l1
     */
    if (header.slice_type == SliceType::B) {

        header.reference_pictures
            .list_modification
            .list1.modification_flag =
            read_flag(reader);


        if (header.reference_pictures
                .list_modification
                .list1.modification_flag) {

            const auto count =
                header.effective_num_ref_idx_l1;

            auto& entries =
                header.reference_pictures
                    .list_modification
                    .list1.list_entry;

            entries.resize(count);

            for (std::uint32_t i = 0;
                 i < count;
                 ++i) {

                entries[i] =
                    read_u(
                        reader,
                        list_entry_bits);
            }
        }
    }

    (void)sps;
}


/*
 * -----------------------------------------------------------
 * Prediction-weight table parser
 * -----------------------------------------------------------
 */

template <typename Reader>
inline void parse_prediction_weight_table(
    Reader& reader,
    const SequenceParameterSet& sps,
    SliceSegmentHeader& header)
{
    auto& table =
        header.prediction_weight_table;

    table = {};


    table.luma_log2_weight_denom = read_ue_checked(reader);


    if (sps.chroma_format != ChromaFormat::Monochrome) {

        table.delta_chroma_log2_weight_denom = read_se_checked(reader);

        const auto chroma_log2_weight_denom =
            static_cast<std::int32_t>(
                table.luma_log2_weight_denom) +
            table.delta_chroma_log2_weight_denom;

        BS_LOG_TRACE("DEBUG prediction weight denominator\n"
            << "  luma_log2_weight_denom = "
            << table.luma_log2_weight_denom
            << '\n'
            << "  delta_chroma_log2_weight_denom = "
            << table.delta_chroma_log2_weight_denom
            << '\n'
            << "  derived chroma_log2_weight_denom = "
            << chroma_log2_weight_denom
            << '\n');

        if (chroma_log2_weight_denom < 0 ||
            chroma_log2_weight_denom > 7) {
            throw SliceSegmentHeaderParseError(
                "slice parser: invalid chroma weight denominator");
        }
    }


    /*
     * L0.
     */
    table.l0.resize(
        header.effective_num_ref_idx_l0);


    for (auto& weight :
         table.l0) {

        weight.luma_weight_luma_flag =
            read_flag(reader);
    }


    if (sps.chroma_format !=
        ChromaFormat::Monochrome) {

        for (auto& weight :
             table.l0) {

            weight.chroma_weight_flag =
                read_flag(reader);
        }
    }


    /*
     * L0 weight values.
     */
    for (auto& weight :
         table.l0) {

        if (weight.luma_weight_luma_flag) {

            weight.delta_luma_weight =
                read_se_checked(reader);

            weight.luma_offset =
                read_se_checked(reader);
        }
    }


    if (sps.chroma_format !=
        ChromaFormat::Monochrome) {

        for (auto& weight :
             table.l0) {

            if (!weight.chroma_weight_flag) {
                continue;
            }

            for (unsigned c = 0;
                 c < 2;
                 ++c) {

                weight.delta_chroma_weight[c] =
                    read_se_checked(reader);

                weight.delta_chroma_offset[c] =
                    read_se_checked(reader);
            }
        }
    }


    /*
     * L1 is present only for B slices.
     */
    if (header.slice_type != SliceType::B) {
        return;
    }


    table.l1.resize(
        header.effective_num_ref_idx_l1);


    for (auto& weight :
         table.l1) {

        weight.luma_weight_luma_flag =
            read_flag(reader);
    }


    if (sps.chroma_format !=
        ChromaFormat::Monochrome) {

        for (auto& weight :
             table.l1) {

            weight.chroma_weight_flag =
                read_flag(reader);
        }
    }


    for (auto& weight :
         table.l1) {

        if (weight.luma_weight_luma_flag) {

            weight.delta_luma_weight =
                read_se_checked(reader);

            weight.luma_offset =
                read_se_checked(reader);
        }
    }


    if (sps.chroma_format !=
        ChromaFormat::Monochrome) {

        for (auto& weight :
             table.l1) {

            if (!weight.chroma_weight_flag) {
                continue;
            }

            for (unsigned c = 0;
                 c < 2;
                 ++c) {

                weight.delta_chroma_weight[c] =
                    read_se_checked(reader);

                weight.delta_chroma_offset[c] =
                    read_se_checked(reader);
            }
        }
    }
}


/*
 * -----------------------------------------------------------
 * Entry-point parser
 * -----------------------------------------------------------
 */

template <typename Reader>
inline void parse_entry_point_offsets(
    Reader& reader,
    const SequenceParameterSet& sps,
    const PictureParameterSet& pps,
    SliceSegmentHeader& header)
{
    /*
     * Entry points are present when tiles or WPP are enabled.
     */
    if (!pps.tiles.tiles_enabled_flag &&
        !pps.entropy_coding_sync_enabled_flag) {

        return;
    }


    header.entry_points.num_entry_point_offsets =
        read_ue_checked(reader);


    if (header.entry_points
            .num_entry_point_offsets == 0) {

        return;
    }


    /*
     * offset_len_minus1 is coded once.
     */
    header.entry_points.offset_len_minus1 =
        read_ue_checked(reader);


    if (header.entry_points.offset_len_minus1 > 31) {
        throw SliceSegmentHeaderParseError(
            "slice parser: invalid offset_len_minus1");
    }


    const auto count =
        header.entry_points
            .num_entry_point_offsets;


    header.entry_points
        .entry_point_offset_minus1
        .resize(count);


    const unsigned width =
        header.entry_points.offset_len_minus1 + 1;


    for (auto& offset :
         header.entry_points
             .entry_point_offset_minus1) {

        offset =
            read_u(reader, width);
    }


    (void)sps;
}


/*
 * -----------------------------------------------------------
 * Slice-header extension parser
 * -----------------------------------------------------------
 */

template <typename Reader>
inline void parse_slice_header_extension(
    Reader& reader,
    const PictureParameterSet& pps,
    SliceSegmentHeader& header)
{
    if (!pps.slice_segment_header_extension_present_flag) {
        return;
    }


    header.extension.length =
        read_ue_checked(reader);


    header.extension.data.resize(
        header.extension.length);


    /*
     * slice_header_extension_data_byte[]
     *
     * Each byte is u(8).
     */
    for (auto& byte :
         header.extension.data) {

        byte =
            static_cast<std::uint8_t>(reader.read_bits(8));
    }
}


/*
 * -----------------------------------------------------------
 * Main parser
 * -----------------------------------------------------------
 */

template <typename Reader>
[[nodiscard]]
inline SliceSegmentHeader parse_slice_segment_header(
    Reader& reader,
    const SequenceParameterSet& sps,
    const PictureParameterSet& pps,
    std::uint8_t nal_unit_type,
    std::uint8_t temporal_id)
{
    SliceSegmentHeader header{};

    initialize_slice_segment_header(header);


    header.nal_unit_type = nal_unit_type;

    header.temporal_id = temporal_id;


    /*
     * =======================================================
     * first_slice_segment_in_pic_flag
     * =======================================================
     */

    header.first_slice_segment_in_pic_flag = read_flag(reader);


    /*
     * no_output_of_prior_pics_flag
     *
     * Present for IRAP pictures.
     */
    if (is_irap_nal_unit(nal_unit_type)) {

        header.no_output_of_prior_pics_flag =
            read_flag(reader);
    }


    /*
     * =======================================================
     * PPS selection
     * =======================================================
     */

    header.slice_pic_parameter_set_id =
        read_ue_checked(reader);


    if (header.slice_pic_parameter_set_id !=
        pps.pps_pic_parameter_set_id) {

        throw SliceSegmentHeaderParseError(
            "slice parser: PPS ID does not match supplied PPS");
    }


    /*
     * =======================================================
     * Dependent slice segment
     * =======================================================
     *
     * Only non-first segments may be dependent.
     */

    if (!header.first_slice_segment_in_pic_flag &&
        pps.dependent_slice_segments_enabled_flag) {

        header.dependent_slice_segment_flag =
            read_flag(reader);
    }


    /*
     * slice_segment_address
     */
    if (!header.first_slice_segment_in_pic_flag) {

        const unsigned address_bits =
            slice_segment_address_bits(sps);

        header.slice_segment_address.bit_width =
            address_bits;

        if (address_bits != 0) {

            header.slice_segment_address.value =
                read_u(
                    reader,
                    address_bits);
        }
    }


    /*
     * =======================================================
     * Dependent slice
     * =======================================================
     *
     * For a dependent slice the remainder of the header is
     * inherited from the associated independent slice.
     *
     * The parser therefore stops here.
     */
    if (header.dependent_slice_segment_flag) {

        return header;
    }


    /*
     * =======================================================
     * Reserved slice header bits
     * =======================================================
     */

    header.slice_reserved_flag.resize(
        pps.num_extra_slice_header_bits);

    for (std::size_t i = 0;
        i < header.slice_reserved_flag.size();
        ++i) {

        header.slice_reserved_flag[i] =
            read_flag(reader);
    }


    /*
     * =======================================================
     * slice_type
     * =======================================================
     */

    const auto slice_type = read_ue_checked(reader);


    if (slice_type > 2) {

        throw SliceSegmentHeaderParseError(
            "slice parser: invalid slice_type");
    }

    header.slice_type = static_cast<SliceType>(slice_type);

    BS_LOG_DEBUG("  slice_type: "
        << static_cast<unsigned>(
            header.slice_type)
        << '\n');

    BS_LOG_DEBUG("  five_minus_max_num_merge_cand: "
        << header.five_minus_max_num_merge_cand
        << '\n');

    BS_LOG_DEBUG("  tiles_enabled: "
        << pps.tiles.tiles_enabled_flag
        << '\n');

    BS_LOG_DEBUG("  entropy_coding_sync_enabled: "
        << pps.entropy_coding_sync_enabled_flag
        << '\n');

    /*
     * =======================================================
     * pic_output_flag
     * =======================================================
     */

    if (pps.output_flag_present_flag) {

        header.pic_output_flag =
            read_flag(reader);
    }


    /*
     * =======================================================
     * colour_plane_id
     * =======================================================
     */

    if (sps.separate_colour_plane_flag) {

        header.colour_plane_id =
            static_cast<std::uint8_t>(
                read_u(reader, 2));
    }


    /*
     * =======================================================
     * POC / RPS
     * =======================================================
     *
     * IDR pictures do not carry the normal POC/RPS syntax.
     */

    if (!is_idr_nal_unit(nal_unit_type)) {

        header.slice_pic_order_cnt_lsb =
            read_u(
                reader,
                static_cast<unsigned>(
                    sps.log2_max_pic_order_cnt_lsb_minus4) +
                4);


        /*
        * Short-term RPS.
        */
        const auto sps_rps_count =
            sps.reference_picture_sets
                .num_short_term_ref_pic_sets;

        /*
        * H.265 7.3.6.1:
        *
        *     if( nal_unit_type != IDR_W_RADL && nal_unit_type != IDR_N_LP ) {
        *         slice_pic_order_cnt_lsb              u(v)
        *         short_term_ref_pic_set_sps_flag      u(1)
        *         if( !short_term_ref_pic_set_sps_flag )
        *             short_term_ref_pic_set( num_short_term_ref_pic_sets )
        *         else if( num_short_term_ref_pic_sets > 1 )
        *             short_term_ref_pic_set_idx       u(v)
        *         ...
        *
        * The flag is always present here regardless of
        * num_short_term_ref_pic_sets.  Only the SPS-RPS
        * selection index is conditional on that count.
        */
        header.short_term_ref_pic_set_sps_flag =
            read_flag(reader);


        if (!header.short_term_ref_pic_set_sps_flag) {

            /*
            * Slice-level short-term RPS.
            */
            header.short_term_ref_pic_set =
                parse_short_term_ref_pic_set(
                    reader,
                    sps,
                    sps_rps_count,
                    sps_rps_count);

        } else {

            /*
            * Select an SPS RPS.
            */
            if (sps_rps_count > 1) {

                const unsigned bits =
                    ceil_log2(sps_rps_count);

                header.short_term_ref_pic_set_idx =
                    read_u(reader, bits);

                if (header.short_term_ref_pic_set_idx >=
                    sps_rps_count) {

                    throw SliceSegmentHeaderParseError(
                        "slice parser: invalid short-term RPS index");
                }

            } else {

                header.short_term_ref_pic_set_idx = 0;
            }
        }


        /*
         * Long-term references.
         */
        if (sps.reference_picture_sets
                .long_term_ref_pics_present_flag) {

            parse_long_term_references(
                reader,
                sps,
                header);
        }

        BS_LOG_TRACE("BITPOS after RPS = "
                << reader.position()
                << '\n');

        BS_LOG_TRACE("RPS DEBUG\n"
                << "  short_term_ref_pic_set_sps_flag = "
                << header.short_term_ref_pic_set_sps_flag
                << '\n'
                << "  num_negative_pics = "
                << header.short_term_ref_pic_set.num_negative_pics
                << '\n'
                << "  num_positive_pics = "
                << header.short_term_ref_pic_set.num_positive_pics
                << '\n'
                << "  num_delta_pocs = "
                << header.short_term_ref_pic_set.num_delta_pocs
                << '\n');



        /*
         * slice_temporal_mvp_enabled_flag
         */
        if (sps.sps_temporal_mvp_enabled_flag) {

            header.slice_temporal_mvp_enabled_flag =
                read_flag(reader);
        }


        BS_LOG_TRACE("BITPOS before SAO = "
                << reader.position()
                << '\n');


        /*
        * =======================================================
        * Sample Adaptive Offset
        * =======================================================
        *
        * H.265:
        *s
        * if( slice_sao_luma_flag )
        * if( slice_sao_chroma_flag )
        *
        * These flags are present when SAO is enabled in the SPS.
        */
        if (sps.sample_adaptive_offset_enabled_flag) {

            header.slice_sao_luma_flag = read_flag(reader);

            if (sps.chroma_format !=
                ChromaFormat::Monochrome) {

                header.slice_sao_chroma_flag = read_flag(reader);
            }
        }


        BS_LOG_TRACE("BITPOS after SAO = "
            << reader.position()
            << '\n');


        BS_LOG_TRACE("DEBUG SAO\n"
            << "  sao_enabled = "
            << sps.sample_adaptive_offset_enabled_flag
            << '\n'
            << "  slice_sao_luma_flag = "
            << header.slice_sao_luma_flag
            << '\n'
            << "  slice_sao_chroma_flag = "
            << header.slice_sao_chroma_flag
            << '\n');


        BS_LOG_TRACE("DEBUG temporal MVP\n"
            << "  sps_temporal_mvp_enabled_flag = "
            << sps.sps_temporal_mvp_enabled_flag
            << '\n'
            << "  slice_temporal_mvp_enabled_flag = "
            << header.slice_temporal_mvp_enabled_flag
            << '\n');

    }


    /*
     * =======================================================
     * Reference picture counts
     * =======================================================
     */

    if (header.slice_type == SliceType::P ||
        header.slice_type == SliceType::B) {

        if (pps.lists_modification_present_flag ||
            header.slice_type == SliceType::B ||
            header.slice_type == SliceType::P) {

            BS_LOG_TRACE("BITPOS before REF COUNTS = "
                << reader.position()
                << '\n');

            header.reference_pictures
                .num_ref_idx_active_override_flag =
                read_flag(reader);


            BS_LOG_TRACE("DEBUG REF COUNTS\n"
                << "  override = "
                << header.reference_pictures
                    .num_ref_idx_active_override_flag
                << '\n');

            if (header.reference_pictures
                    .num_ref_idx_active_override_flag) {

                BS_LOG_TRACE("  l0_minus1 = "
                    << header.reference_pictures
                        .num_ref_idx_l0_active_minus1
                    << '\n'
                    << "  l1_minus1 = "
                    << header.reference_pictures
                        .num_ref_idx_l1_active_minus1
                    << '\n');
            }

        }


        if (header.reference_pictures
                .num_ref_idx_active_override_flag) {

            header.reference_pictures
                .num_ref_idx_l0_active_minus1 =
                read_ue_checked(reader);

            if (header.slice_type == SliceType::B) {

                header.reference_pictures
                    .num_ref_idx_l1_active_minus1 =
                    read_ue_checked(reader);
            }
        }



        derive_slice_reference_counts(
            header,
            pps.num_ref_idx_l0_default_active() ,
            pps.num_ref_idx_l1_default_active());

        BS_LOG_TRACE("BITPOS after REF COUNTS = "
            << reader.position()
            << '\n');

        BS_LOG_TRACE("  effective L0 = "
                << header.effective_num_ref_idx_l0
                << '\n');

        BS_LOG_TRACE("  effective L1 = "
                << header.effective_num_ref_idx_l1
                << '\n');


        /*
         * NumPocTotalCurr.
         *
         * For the syntax/data model we derive it from the
         * selected explicit RPS when possible.
         *
         * The full decoder semantic derivation can refine it
         * for inter-predicted RPS and long-term references.
         */

        derive_num_poc_total_curr(sps, header);


        /*
         * Reference-list modification.
         */
        parse_ref_pic_list_modification(
            reader,
            sps,
            pps,
            header);
    } else {

        derive_slice_reference_counts(
            header,
            pps.num_ref_idx_l0_default_active(),
            pps.num_ref_idx_l1_default_active());
    }


    /*
     * =======================================================
     * B-slice motion syntax
     * =======================================================
     */

    if (header.slice_type == SliceType::B) {
        BS_LOG_TRACE("BITPOS before mvd_l1_zero_flag = "
                << reader.position() << '\n');

        header.mvd_l1_zero_flag = read_flag(reader);

        BS_LOG_TRACE("BITPOS after mvd_l1_zero_flag = "
                << reader.position()
                << " value=" << header.mvd_l1_zero_flag
                << '\n');
    }


    /*
     * =======================================================
     * CABAC initialization
     * =======================================================
     */

    if (pps.cabac_init_present_flag) {

        header.cabac_init_flag =
            read_flag(reader);
    }


    BS_LOG_TRACE("BITPOS before COLLOCATED SYNTAX = "
          << reader.position()
          << '\n');

    /*
    * =======================================================
    * Collocated reference
    * =======================================================
    */

    if (header.slice_temporal_mvp_enabled_flag) {

        /*
        * collocated_from_l0_flag
        *
        * B slice: explicitly coded.
        * P slice: inferred to be 1.
        */
        if (header.slice_type == SliceType::B) {

            BS_LOG_TRACE("BITPOS before collocated_from_l0_flag = "
                    << reader.position() << '\n');

            header.collocated_from_l0_flag = read_flag(reader);

            BS_LOG_TRACE("BITPOS after collocated_from_l0_flag = "
                    << reader.position()
                    << " value="
                    << header.collocated_from_l0_flag
                    << '\n');

        } else {
            header.collocated_from_l0_flag = true;
        }


        /*
        * ===================================================
        * collocated_ref_idx
        * ===================================================
        *
        * Only present when the selected reference list
        * contains more than one active reference.
        */

        if (header.collocated_from_l0_flag) {

            // L0 is the collocated list
            if (header.effective_num_ref_idx_l0 > 1) {

                BS_LOG_TRACE("BITPOS before collocated_ref_idx = "
                        << reader.position() << '\n');

                header.collocated_ref_idx = reader.read_ue();

                BS_LOG_TRACE("BITPOS after collocated_ref_idx = "
                        << reader.position()
                        << " value="
                        << header.collocated_ref_idx
                        << '\n');

            } else {
                header.collocated_ref_idx = 0;
            }

        } else {

            // L1 is the collocated list
            if (header.effective_num_ref_idx_l1 > 1) {

                BS_LOG_TRACE("BITPOS before collocated_ref_idx = "
                        << reader.position() << '\n');

                header.collocated_ref_idx = reader.read_ue();

                BS_LOG_TRACE("BITPOS after collocated_ref_idx = "
                        << reader.position()
                        << " value="
                        << header.collocated_ref_idx
                        << '\n');

            } else {
                header.collocated_ref_idx = 0;
            }
        }
    }

    BS_LOG_TRACE("BITPOS after COLLOCATED = "
            << reader.position()
            << '\n');

    BS_LOG_TRACE("DEBUG collocated\n"
    << "  slice_type = "
    << static_cast<unsigned>(header.slice_type)
    << '\n'
    << "  collocated_from_l0_flag = "
    << header.collocated_from_l0_flag
    << '\n'
    << "  collocated_ref_idx = "
    << header.collocated_ref_idx
    << '\n'
    << "  effective L0 = "
    << header.effective_num_ref_idx_l0
    << '\n'
    << "  effective L1 = "
    << header.effective_num_ref_idx_l1
    << '\n');



    BS_LOG_TRACE("BITPOS before WEIGHT TABLE = "
            << reader.position()
            << '\n');

    /*
     * =======================================================
     * Prediction weight table
     * =======================================================
     *
     * P slice:
     *
     *     weighted_pred_flag
     *
     * B slice:
     *
     *     weighted_bipred_flag
     */

    const bool weighted_prediction =
        (header.slice_type == SliceType::P &&
         pps.weighted_pred_flag) ||
        (header.slice_type == SliceType::B &&
         pps.weighted_bipred_flag);


    if (weighted_prediction) {

        header.prediction_weight_table_present = true;

        BS_LOG_TRACE("DEBUG pred_weight_table\n"
            << "  slice_type = "
            << static_cast<unsigned>(header.slice_type)
            << '\n'
            << "  num_ref_idx_l0_active = "
            << header.effective_num_ref_idx_l0
            << '\n'
            << "  num_ref_idx_l1_active = "
            << header.effective_num_ref_idx_l1
            << '\n');

        parse_prediction_weight_table(
            reader,
            sps,
            header);


        BS_LOG_TRACE("DEBUG prediction weight table\n"
            << "  slice_type = "
            << static_cast<unsigned>(header.slice_type)
            << '\n'
            << "  weighted_pred_flag = "
            << pps.weighted_pred_flag
            << '\n'
            << "  weighted_bipred_flag = "
            << pps.weighted_bipred_flag
            << '\n'
            << "  L0 refs = "
            << header.effective_num_ref_idx_l0
            << '\n'
            << "  L1 refs = "
            << header.effective_num_ref_idx_l1
            << '\n');
    }


    BS_LOG_TRACE("BITPOS after WEIGHT TABLE = "
          << reader.position()
          << '\n');


    /*
    * =======================================================
    * Five minus max merge candidates
    * =======================================================
    *
    * H.265:
    *
    *     if( slice_type != I )
    *         five_minus_max_num_merge_cand
    *
    * This syntax element occurs AFTER the prediction
    * weight table.
    */

    if (header.slice_type != SliceType::I) {

        BS_LOG_TRACE("BITPOS before MERGE = "
                << reader.position()
                << '\n');

        BS_LOG_TRACE("DEBUG before merge candidate\n"
                << "  slice_type = "
                << static_cast<int>(header.slice_type)
                << '\n'
                << "  temporal_mvp = "
                << header.slice_temporal_mvp_enabled_flag
                << '\n'
                << "  five_minus_max_num_merge_cand is about to be read\n");



        header.five_minus_max_num_merge_cand = read_ue_checked(reader);

        BS_LOG_TRACE("BITPOS after MERGE = "
                << reader.position()
                << '\n');

        BS_LOG_TRACE("DEBUG merge candidate value = "
            << header.five_minus_max_num_merge_cand
            << '\n');

        if (header.five_minus_max_num_merge_cand > 5) {
            throw SliceSegmentHeaderParseError(
                "slice parser: invalid five_minus_max_num_merge_cand");
        }

    } else {

        /*
        * Not present for I slices.
        */
        header.five_minus_max_num_merge_cand = 0;
    }




    /*
     * =======================================================
     * QP
     * =======================================================
     */

    header.slice_qp_delta =
        read_se_checked(reader);


    /*
     * =======================================================
     * Chroma QP offsets
     * =======================================================
     */

    if (pps.slice_chroma_qp_offsets_present_flag) {

        header.slice_cb_qp_offset =
            read_se_checked(reader);

        header.slice_cr_qp_offset =
            read_se_checked(reader);
    }


    /*
     * =======================================================
     * Deblocking
     * =======================================================
     */

    if (pps.deblocking
            .deblocking_filter_control_present_flag) {

        if (pps.deblocking
                .deblocking_filter_override_enabled_flag) {

            header.deblocking.override_flag =
                read_flag(reader);
        }


        const bool use_pps_deblocking =
            !header.deblocking.override_flag;


        if (use_pps_deblocking) {

            header.deblocking.disabled_flag =
                pps.deblocking
                    .pps_deblocking_filter_disabled_flag;

            header.deblocking.beta_offset_div2 =
                pps.deblocking
                    .pps_beta_offset_div2;

            header.deblocking.tc_offset_div2 =
                pps.deblocking
                    .pps_tc_offset_div2;

        } else {

            header.deblocking.disabled_flag =
                read_flag(reader);

            if (!header.deblocking.disabled_flag) {

                header.deblocking.beta_offset_div2 =
                    read_se_checked(reader);

                header.deblocking.tc_offset_div2 =
                    read_se_checked(reader);
            }
        }
    }


    /*
     * =======================================================
     * Loop filtering across slices
     * =======================================================
     */

    if (pps.pps_loop_filter_across_slices_enabled_flag) {
        header.slice_loop_filter_across_slices_enabled_flag =
            read_flag(reader);
    }


    /*
     * =======================================================
     * Entry-point offsets
     * =======================================================
     */

    parse_entry_point_offsets(
        reader,
        sps,
        pps,
        header);


    /*
     * =======================================================
     * Slice header extension
     * =======================================================
     */

    parse_slice_header_extension(
        reader,
        pps,
        header);


    /*
     * =======================================================
     * Final validation
     * =======================================================
     */

    if (!validate_slice_segment_header(header)) {

        throw SliceSegmentHeaderParseError(
            "slice parser: invalid slice segment header");
    }


    return header;
}


/*
 * -----------------------------------------------------------
 * Convenience overload
 * -----------------------------------------------------------
 *
 * Uses the PPS/SPS IDs already associated with the caller's
 * parameter-set objects.
 */

template <typename Reader>
[[nodiscard]]
inline SliceSegmentHeader parse_slice_segment_header(
    Reader& reader,
    const SequenceParameterSet& sps,
    const PictureParameterSet& pps)
{
    return parse_slice_segment_header(
        reader,
        sps,
        pps,
        0,
        0);
}

} // namespace bs