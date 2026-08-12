#include "hevc_parse.h"


namespace bsparser{

    static unsigned hevc_ceil_log2(uint32_t x)
    {
        if (x <= 1)
            return 0;

        --x;

        unsigned n = 0;

        while (x)
        {
            ++n;
            x >>= 1;
        }

        return n;
    }

    uint32_t hevc_num_pic_total_curr(
        const HevcShortTermRps& rps,
        uint32_t numLongTermCurr)
    {
        return
            rps.num_used_by_curr_pic() +
            numLongTermCurr;
    }


    unsigned hevc_num_pic_total_curr_bits(
        uint32_t numPicTotalCurr)
    {
        return hevc_ceil_log2(
            numPicTotalCurr
        );
    }


    uint32_t hevc_num_long_term_curr(
        const HevcSps& sps,
        const HevcSlice& slice)
    {
        uint32_t count = 0;

        // -------------------------------------------------------------------------
        // Long-term references coming from SPS
        // -------------------------------------------------------------------------

        for (uint32_t i = 0;
            i < slice.num_long_term_sps;
            ++i)
        {
            const uint32_t idx =
                slice.lt_idx_sps[i];

            if (idx >= sps.used_by_curr_pic_lt_sps_flag.size())
            {
                throw std::runtime_error(
                    "invalid HEVC long-term SPS index");
            }

            if (sps.used_by_curr_pic_lt_sps_flag[idx])
            {
                ++count;
            }
        }

        // -------------------------------------------------------------------------
        // Long-term references explicitly signalled in the slice
        // -------------------------------------------------------------------------

        for (uint32_t i = 0;
            i < slice.num_long_term_pics;
            ++i)
        {
            if (i >=
                slice.used_by_curr_pic_lt_flag.size())
            {
                throw std::runtime_error(
                    "invalid HEVC long-term slice reference index");
            }

            if (slice.used_by_curr_pic_lt_flag[
                    slice.num_long_term_sps + i])
            {
                ++count;
            }
        }

        return count;
    }


    const HevcShortTermRps& hevc_get_active_rps(
        const HevcSps& sps,
        const HevcSlice& slice,
        const HevcShortTermRps& inlineRps)
    {
        if (!slice.short_term_ref_pic_set_sps_flag)
        {
            return inlineRps;
        }

        if (slice.short_term_ref_pic_set_idx >=
            sps.short_term_ref_pic_sets.size())
        {
            throw std::runtime_error(
                "invalid HEVC active short-term RPS index");
        }

        return sps.short_term_ref_pic_sets[
            slice.short_term_ref_pic_set_idx
        ];
    }


    void parse_hevc_ref_pic_list_modification(
        BitReader& b,
        Header& h,
        HevcSlice& slice,
        const HevcPps& pps,
        uint32_t numPicTotalCurr)
    {
        slice.num_pic_total_curr =
            numPicTotalCurr;

        field(
            h,
            "num_pic_total_curr",
            numPicTotalCurr
        );

        // =========================================================================
        // HEVC only signals list modification when there is more than one possible
        // current reference picture.
        // =========================================================================

        if (!pps.lists_modification_present_flag ||
            numPicTotalCurr <= 1)
        {
            return;
        }

        const unsigned listEntryBits =
            hevc_ceil_log2(numPicTotalCurr);

        // =========================================================================
        // List 0
        // =========================================================================

        if (slice.slice_type == 1 ||
            slice.slice_type == 0)
        {
            slice.ref_pic_list_modification_flag_l0 =
                b.u(1) != 0;

            field(
                h,
                "ref_pic_list_modification_flag_l0",
                slice.ref_pic_list_modification_flag_l0
            );

            if (slice.ref_pic_list_modification_flag_l0)
            {
                slice.list_entry_l0.resize(
                    slice.num_ref_idx_l0_active_minus1 + 1
                );

                for (uint32_t i = 0;
                    i <= slice.num_ref_idx_l0_active_minus1;
                    ++i)
                {
                    uint32_t entry = 0;

                    if (listEntryBits > 0)
                    {
                        entry =
                            static_cast<uint32_t>(
                                b.u(listEntryBits)
                            );
                    }

                    if (entry >= numPicTotalCurr)
                    {
                        throw std::runtime_error(
                            "invalid HEVC list_entry_l0");
                    }

                    slice.list_entry_l0[i] =
                        entry;

                    field(
                        h,
                        "list_entry_l0[" +
                            std::to_string(i) + "]",
                        entry
                    );
                }
            }
        }

        // =========================================================================
        // List 1
        // =========================================================================

        if (slice.slice_type == 0)
        {
            slice.ref_pic_list_modification_flag_l1 =
                b.u(1) != 0;

            field(
                h,
                "ref_pic_list_modification_flag_l1",
                slice.ref_pic_list_modification_flag_l1
            );

            if (slice.ref_pic_list_modification_flag_l1)
            {
                slice.list_entry_l1.resize(
                    slice.num_ref_idx_l1_active_minus1 + 1
                );

                for (uint32_t i = 0;
                    i <= slice.num_ref_idx_l1_active_minus1;
                    ++i)
                {
                    uint32_t entry = 0;

                    if (listEntryBits > 0)
                    {
                        entry =
                            static_cast<uint32_t>(
                                b.u(listEntryBits)
                            );
                    }

                    if (entry >= numPicTotalCurr)
                    {
                        throw std::runtime_error(
                            "invalid HEVC list_entry_l1");
                    }

                    slice.list_entry_l1[i] =
                        entry;

                    field(
                        h,
                        "list_entry_l1[" +
                            std::to_string(i) + "]",
                        entry
                    );
                }
            }
        }
    }


    HevcShortTermRps parse_hevc_short_term_ref_pic_set(
        BitReader& b,
        Header& h,
        uint32_t stRpsIdx,
        uint32_t numShortTermRefPicSets,
        const std::vector<HevcShortTermRps>& previousRps)
    {
        HevcShortTermRps rps;

        // =========================================================================
        // inter_ref_pic_set_prediction_flag
        // =========================================================================

        if (stRpsIdx != 0)
        {
            rps.inter_ref_pic_set_prediction_flag =
                b.u(1) != 0;

            field(
                h,
                "inter_ref_pic_set_prediction_flag[" +
                    std::to_string(stRpsIdx) + "]",
                rps.inter_ref_pic_set_prediction_flag
            );
        }

        // =========================================================================
        // Non-predicted RPS
        // =========================================================================

        if (!rps.inter_ref_pic_set_prediction_flag)
        {
            rps.num_negative_pics =
                static_cast<uint32_t>(b.ue());

            rps.num_positive_pics =
                static_cast<uint32_t>(b.ue());

            field(
                h,
                "num_negative_pics[" +
                    std::to_string(stRpsIdx) + "]",
                rps.num_negative_pics
            );

            field(
                h,
                "num_positive_pics[" +
                    std::to_string(stRpsIdx) + "]",
                rps.num_positive_pics
            );

            constexpr uint32_t kMaxRpsPics = 1024;

            if (rps.num_negative_pics +
                    rps.num_positive_pics >
                kMaxRpsPics)
            {
                throw std::runtime_error(
                    "HEVC short-term RPS contains too many pictures");
            }

            rps.delta_poc_s0.resize(
                rps.num_negative_pics
            );

            rps.used_by_curr_pic_s0.resize(
                rps.num_negative_pics
            );

            rps.delta_poc_s1.resize(
                rps.num_positive_pics
            );

            rps.used_by_curr_pic_s1.resize(
                rps.num_positive_pics
            );

            // ---------------------------------------------------------------------
            // Negative pictures
            //
            // Syntax stores delta_poc_s0_minus1.
            //
            // Derived:
            //
            // DeltaPocS0[0] = -(delta_poc_s0_minus1[0] + 1)
            //
            // DeltaPocS0[i] = DeltaPocS0[i-1]
            //                 - delta_poc_s0_minus1[i] - 1
            // ---------------------------------------------------------------------

            int32_t prevDeltaPoc = 0;

            for (uint32_t i = 0;
                i < rps.num_negative_pics;
                ++i)
            {
                const uint32_t deltaMinus1 =
                    static_cast<uint32_t>(b.ue());

                const bool used =
                    b.u(1) != 0;

                field(
                    h,
                    "delta_poc_s0_minus1[" +
                        std::to_string(stRpsIdx) + "][" +
                        std::to_string(i) + "]",
                    deltaMinus1
                );

                field(
                    h,
                    "used_by_curr_pic_s0_flag[" +
                        std::to_string(stRpsIdx) + "][" +
                        std::to_string(i) + "]",
                    used
                );

                if (i == 0)
                {
                    prevDeltaPoc =
                        -static_cast<int32_t>(
                            deltaMinus1 + 1
                        );
                }
                else
                {
                    prevDeltaPoc -=
                        static_cast<int32_t>(
                            deltaMinus1 + 1
                        );
                }

                rps.delta_poc_s0[i] =
                    prevDeltaPoc;

                rps.used_by_curr_pic_s0[i] =
                    used;
            }

            // ---------------------------------------------------------------------
            // Positive pictures
            // ---------------------------------------------------------------------

            prevDeltaPoc = 0;

            for (uint32_t i = 0;
                i < rps.num_positive_pics;
                ++i)
            {
                const uint32_t deltaMinus1 =
                    static_cast<uint32_t>(b.ue());

                const bool used =
                    b.u(1) != 0;

                field(
                    h,
                    "delta_poc_s1_minus1[" +
                        std::to_string(stRpsIdx) + "][" +
                        std::to_string(i) + "]",
                    deltaMinus1
                );

                field(
                    h,
                    "used_by_curr_pic_s1_flag[" +
                        std::to_string(stRpsIdx) + "][" +
                        std::to_string(i) + "]",
                    used
                );

                if (i == 0)
                {
                    prevDeltaPoc =
                        static_cast<int32_t>(
                            deltaMinus1 + 1
                        );
                }
                else
                {
                    prevDeltaPoc +=
                        static_cast<int32_t>(
                            deltaMinus1 + 1
                        );
                }

                rps.delta_poc_s1[i] =
                    prevDeltaPoc;

                rps.used_by_curr_pic_s1[i] =
                    used;
            }

            return rps;
        }

        // =========================================================================
        // Predicted RPS
        // =========================================================================

        if (stRpsIdx == 0)
        {
            throw std::runtime_error(
                "invalid HEVC RPS prediction at index 0");
        }

        // -------------------------------------------------------------------------
        // delta_idx_minus1
        // -------------------------------------------------------------------------

        if (stRpsIdx == numShortTermRefPicSets)
        {
            rps.delta_idx_minus1 =
                static_cast<uint32_t>(
                    b.ue()
                );

            field(
                h,
                "delta_idx_minus1",
                rps.delta_idx_minus1
            );
        }

        if (rps.delta_idx_minus1 >= stRpsIdx)
        {
            throw std::runtime_error(
                "invalid HEVC delta_idx_minus1");
        }

        const uint32_t refRpsIdx =
            stRpsIdx -
            (rps.delta_idx_minus1 + 1);

        if (refRpsIdx >= previousRps.size())
        {
            throw std::runtime_error(
                "HEVC RPS references unavailable RPS");
        }

        const HevcShortTermRps& refRps =
            previousRps[refRpsIdx];

        // -------------------------------------------------------------------------
        // delta_rps_sign
        // -------------------------------------------------------------------------

        rps.delta_rps_sign =
            b.u(1) != 0;

        field(
            h,
            "delta_rps_sign",
            rps.delta_rps_sign
        );

        // -------------------------------------------------------------------------
        // abs_delta_rps_minus1
        // -------------------------------------------------------------------------

        rps.abs_delta_rps_minus1 =
            static_cast<uint32_t>(
                b.ue()
            );

        field(
            h,
            "abs_delta_rps_minus1",
            rps.abs_delta_rps_minus1
        );

        const int32_t deltaRps =
            rps.delta_rps_sign
                ? -static_cast<int32_t>(
                    rps.abs_delta_rps_minus1 + 1
                )
                : static_cast<int32_t>(
                    rps.abs_delta_rps_minus1 + 1
                );

        const uint32_t refNumDeltaPocs =
            refRps.num_delta_pocs();

        // -------------------------------------------------------------------------
        // used_by_curr_pic_flag / use_delta_flag
        //
        // There is one additional entry for deltaRps itself.
        // -------------------------------------------------------------------------

        std::vector<bool> usedByCurrPicFlag(
            refNumDeltaPocs + 1
        );

        std::vector<bool> useDeltaFlag(
            refNumDeltaPocs + 1
        );

        for (uint32_t j = 0;
            j <= refNumDeltaPocs;
            ++j)
        {
            usedByCurrPicFlag[j] =
                b.u(1) != 0;

            field(
                h,
                "used_by_curr_pic_flag[" +
                    std::to_string(j) + "]",
                usedByCurrPicFlag[j]
            );

            if (!usedByCurrPicFlag[j])
            {
                useDeltaFlag[j] =
                    b.u(1) != 0;

                field(
                    h,
                    "use_delta_flag[" +
                        std::to_string(j) + "]",
                    useDeltaFlag[j]
                );
            }
            else
            {
                useDeltaFlag[j] = true;
            }
        }

        // =========================================================================
        // Build temporary source RPS in spec order
        // =========================================================================

        std::vector<int32_t> refDeltaPoc;
        std::vector<bool> refUsed;

        refDeltaPoc.reserve(
            refNumDeltaPocs
        );

        refUsed.reserve(
            refNumDeltaPocs
        );

        for (size_t i = 0;
            i < refRps.delta_poc_s0.size();
            ++i)
        {
            refDeltaPoc.push_back(
                refRps.delta_poc_s0[i]
            );

            refUsed.push_back(
                refRps.used_by_curr_pic_s0[i]
            );
        }

        for (size_t i = 0;
            i < refRps.delta_poc_s1.size();
            ++i)
        {
            refDeltaPoc.push_back(
                refRps.delta_poc_s1[i]
            );

            refUsed.push_back(
                refRps.used_by_curr_pic_s1[i]
            );
        }



        // =========================================================================
        // The previous loop must not iterate over the vector being appended to.
        //
        // Rebuild DeltaPocS0 safely.
        // =========================================================================

        rps.delta_poc_s0.clear();
        rps.used_by_curr_pic_s0.clear();

        // Positive source entries, reverse.
        for (int j =
                static_cast<int>(
                    refRps.delta_poc_s1.size()
                ) - 1;
            j >= 0;
            --j)
        {
            const uint32_t sourceIndex =
                refRps.num_negative_pics +
                static_cast<uint32_t>(j);

            const int32_t dPoc =
                refRps.delta_poc_s1[j] +
                deltaRps;

            if (dPoc < 0 &&
                useDeltaFlag[sourceIndex])
            {
                rps.delta_poc_s0.push_back(dPoc);
                rps.used_by_curr_pic_s0.push_back(
                    usedByCurrPicFlag[sourceIndex]
                );
            }
        }

        // deltaRps.
        if (deltaRps < 0 &&
            useDeltaFlag[refNumDeltaPocs])
        {
            rps.delta_poc_s0.push_back(deltaRps);
            rps.used_by_curr_pic_s0.push_back(
                usedByCurrPicFlag[refNumDeltaPocs]
            );
        }

        // Negative source entries, forward.
        for (uint32_t j = 0;
            j < refRps.delta_poc_s0.size();
            ++j)
        {
            const int32_t dPoc =
                refRps.delta_poc_s0[j] +
                deltaRps;

            if (dPoc < 0 &&
                useDeltaFlag[j])
            {
                rps.delta_poc_s0.push_back(dPoc);
                rps.used_by_curr_pic_s0.push_back(
                    usedByCurrPicFlag[j]
                );
            }
        }

        // =========================================================================
        // Derive DeltaPocS1
        //
        // 1. Negative source entries, reverse
        // 2. deltaRps itself if positive
        // 3. Positive source entries forward
        // =========================================================================

        for (int j =
                static_cast<int>(
                    refRps.delta_poc_s0.size()
                ) - 1;
            j >= 0;
            --j)
        {
            const uint32_t sourceIndex =
                static_cast<uint32_t>(j);

            const int32_t dPoc =
                refRps.delta_poc_s0[j] +
                deltaRps;

            if (dPoc > 0 &&
                useDeltaFlag[sourceIndex])
            {
                rps.delta_poc_s1.push_back(dPoc);

                rps.used_by_curr_pic_s1.push_back(
                    usedByCurrPicFlag[sourceIndex]
                );
            }
        }

        if (deltaRps > 0 &&
            useDeltaFlag[refNumDeltaPocs])
        {
            rps.delta_poc_s1.push_back(deltaRps);

            rps.used_by_curr_pic_s1.push_back(
                usedByCurrPicFlag[refNumDeltaPocs]
            );
        }

        for (uint32_t j = 0;
            j < refRps.delta_poc_s1.size();
            ++j)
        {
            const uint32_t sourceIndex =
                refRps.num_negative_pics +
                j;

            const int32_t dPoc =
                refRps.delta_poc_s1[j] +
                deltaRps;

            if (dPoc > 0 &&
                useDeltaFlag[sourceIndex])
            {
                rps.delta_poc_s1.push_back(dPoc);

                rps.used_by_curr_pic_s1.push_back(
                    usedByCurrPicFlag[sourceIndex]
                );
            }
        }

        // =========================================================================
        // Derived counts
        // =========================================================================

        rps.num_negative_pics =
            static_cast<uint32_t>(
                rps.delta_poc_s0.size()
            );

        rps.num_positive_pics =
            static_cast<uint32_t>(
                rps.delta_poc_s1.size()
            );

        return rps;
    }


    void parse_hevc_scaling_list_data(
        BitReader& b,
        Header& h)
    {
        /*
        * scaling_list_data()
        *
        * HEVC:
        *
        * sizeId = 0..3
        *
        * matrixId:
        *   sizeId 0..2 -> 0..5
        *   sizeId 3    -> 0..1
        *
        * The actual coefficient count is:
        *
        *   coefNum = min(64, 1 << (4 + (sizeId << 1)))
        *
        * For sizeId > 1 there is also:
        *
        *   scaling_list_dc_coef_minus8
        */

        for (uint32_t sizeId = 0;
            sizeId < 4;
            ++sizeId)
        {
            const uint32_t matrixCount =
                (sizeId == 3) ? 2 : 6;

            const uint32_t coefNum =
                std::min(
                    64u,
                    1u << (4 + (sizeId << 1))
                );

            for (uint32_t matrixId = 0;
                matrixId < matrixCount;
                ++matrixId)
            {
                const uint32_t predModeFlag =
                    static_cast<uint32_t>(b.u(1));

                field(
                    h,
                    "scaling_list_pred_mode_flag[" +
                        std::to_string(sizeId) + "][" +
                        std::to_string(matrixId) + "]",
                    predModeFlag
                );

                // -------------------------------------------------------------
                // Prediction mode
                // -------------------------------------------------------------

                if (!predModeFlag)
                {
                    const uint32_t predMatrixIdDelta =
                        static_cast<uint32_t>(b.ue());

                    field(
                        h,
                        "scaling_list_pred_matrix_id_delta[" +
                            std::to_string(sizeId) + "][" +
                            std::to_string(matrixId) + "]",
                        predMatrixIdDelta
                    );

                    /*
                    * No explicit coefficients follow.
                    *
                    * The matrix is predicted from another scaling list.
                    */
                    continue;
                }

                // -------------------------------------------------------------
                // Explicit scaling-list data
                // -------------------------------------------------------------

                int32_t nextCoef = 8;

                /*
                * For sizeId > 1 the first coefficient is represented by
                * scaling_list_dc_coef_minus8.
                */
                if (sizeId > 1)
                {
                    const int32_t dcCoefMinus8 =
                        b.se();

                    field(
                        h,
                        "scaling_list_dc_coef_minus8[" +
                            std::to_string(sizeId) + "][" +
                            std::to_string(matrixId) + "]",
                        dcCoefMinus8
                    );

                    /*
                    * This is the DC coefficient used as the starting
                    * predictor for the following coefficient deltas.
                    */
                    nextCoef = 8 + dcCoefMinus8;
                }

                /*
                * scaling_list_delta_coef is signed Exp-Golomb.
                *
                * The actual scaling coefficients are reconstructed as:
                *
                *     nextCoef = (nextCoef + deltaCoef + 256) % 256
                *
                * We don't need to retain the reconstructed matrix yet,
                * but we still need to consume every syntax element.
                */
                for (uint32_t i = 0;
                    i < coefNum;
                    ++i)
                {
                    const int32_t deltaCoef =
                        b.se();

                    field(
                        h,
                        "scaling_list_delta_coef[" +
                            std::to_string(sizeId) + "][" +
                            std::to_string(matrixId) + "][" +
                            std::to_string(i) + "]",
                        deltaCoef
                    );

                    nextCoef =
                        (nextCoef + deltaCoef + 256) & 255;
                }
            }
        }
    }


    void parse_hevc_hrd_parameters(
        BitReader& b,
        Header& h,
        bool commonInfPresentFlag,
        uint32_t maxNumSubLayersMinus1)
    {
        bool nalHrdParametersPresentFlag = false;
        bool vclHrdParametersPresentFlag = false;
        bool subPicHrdParamsPresentFlag = false;

        // =========================================================================
        // commonInfPresentFlag
        // =========================================================================

        if (commonInfPresentFlag)
        {
            nalHrdParametersPresentFlag =
                b.u(1) != 0;

            field(
                h,
                "nal_hrd_parameters_present_flag",
                nalHrdParametersPresentFlag
            );

            vclHrdParametersPresentFlag =
                b.u(1) != 0;

            field(
                h,
                "vcl_hrd_parameters_present_flag",
                vclHrdParametersPresentFlag
            );

            if (nalHrdParametersPresentFlag ||
                vclHrdParametersPresentFlag)
            {
                subPicHrdParamsPresentFlag =
                    b.u(1) != 0;

                field(
                    h,
                    "sub_pic_hrd_params_present_flag",
                    subPicHrdParamsPresentFlag
                );
            }

            // ---------------------------------------------------------------------
            // Sub-picture HRD parameters
            // ---------------------------------------------------------------------

            if (subPicHrdParamsPresentFlag)
            {
                field(
                    h,
                    "tick_divisor_minus2",
                    b.u(8)
                );

                field(
                    h,
                    "du_cpb_removal_delay_increment_length_minus1",
                    b.u(5)
                );

                field(
                    h,
                    "sub_pic_cpb_params_in_pic_timing_sei_flag",
                    b.u(1)
                );

                field(
                    h,
                    "dpb_output_delay_du_length_minus1",
                    b.u(5)
                );
            }

            // ---------------------------------------------------------------------
            // Common HRD scales
            // ---------------------------------------------------------------------

            field(
                h,
                "bit_rate_scale",
                b.u(4)
            );

            field(
                h,
                "cpb_size_scale",
                b.u(4)
            );

            if (subPicHrdParamsPresentFlag)
            {
                field(
                    h,
                    "cpb_size_du_scale",
                    b.u(4)
                );
            }

            field(
                h,
                "initial_cpb_removal_delay_length_minus1",
                b.u(5)
            );

            field(
                h,
                "au_cpb_removal_delay_length_minus1",
                b.u(5)
            );

            field(
                h,
                "dpb_output_delay_length_minus1",
                b.u(5)
            );
        }

        // =========================================================================
        // sub_layer_hrd_parameters()
        // =========================================================================

        for (uint32_t i = 0;
            i <= maxNumSubLayersMinus1;
            ++i)
        {
            // ---------------------------------------------------------------------
            // fixed_pic_rate_general_flag
            // ---------------------------------------------------------------------

            const bool fixedPicRateGeneralFlag =
                b.u(1) != 0;

            field(
                h,
                "fixed_pic_rate_general_flag[" +
                    std::to_string(i) + "]",
                fixedPicRateGeneralFlag
            );

            // ---------------------------------------------------------------------
            // fixed_pic_rate_within_cvs_flag
            // ---------------------------------------------------------------------

            bool fixedPicRateWithinCvsFlag = true;

            if (!fixedPicRateGeneralFlag)
            {
                fixedPicRateWithinCvsFlag =
                    b.u(1) != 0;

                field(
                    h,
                    "fixed_pic_rate_within_cvs_flag[" +
                        std::to_string(i) + "]",
                    fixedPicRateWithinCvsFlag
                );
            }
            else
            {
                /*
                * When fixed_pic_rate_general_flag is 1,
                * fixed_pic_rate_within_cvs_flag is inferred to 1.
                */
                field(
                    h,
                    "fixed_pic_rate_within_cvs_flag[" +
                        std::to_string(i) + "]",
                    1
                );
            }

            // ---------------------------------------------------------------------
            // elemental_duration_in_tc_minus1
            // ---------------------------------------------------------------------

            bool lowDelayHrdFlag = false;
            uint32_t cpbCntMinus1 = 0;

            if (fixedPicRateWithinCvsFlag)
            {
                field(
                    h,
                    "elemental_duration_in_tc_minus1[" +
                        std::to_string(i) + "]",
                    b.ue()
                );
            }
            else
            {
                // -------------------------------------------------------------
                // low_delay_hrd_flag
                // -------------------------------------------------------------

                lowDelayHrdFlag =
                    b.u(1) != 0;

                field(
                    h,
                    "low_delay_hrd_flag[" +
                        std::to_string(i) + "]",
                    lowDelayHrdFlag
                );

                // -------------------------------------------------------------
                // cpb_cnt_minus1
                // -------------------------------------------------------------

                if (!lowDelayHrdFlag)
                {
                    cpbCntMinus1 =
                        static_cast<uint32_t>(
                            b.ue()
                        );

                    field(
                        h,
                        "cpb_cnt_minus1[" +
                            std::to_string(i) + "]",
                        cpbCntMinus1
                    );

                    constexpr uint32_t kMaxCpbCnt = 4096;

                    if (cpbCntMinus1 >= kMaxCpbCnt)
                    {
                        throw std::runtime_error(
                            "HEVC HRD cpb_cnt_minus1 too large");
                    }
                }
            }

            // ---------------------------------------------------------------------
            // NAL HRD
            // ---------------------------------------------------------------------

            if (nalHrdParametersPresentFlag)
            {
                for (uint32_t j = 0;
                    j <= cpbCntMinus1;
                    ++j)
                {
                    field(
                        h,
                        "bit_rate_value_minus1[" +
                            std::to_string(i) + "][" +
                            std::to_string(j) + "]",
                        b.ue()
                    );

                    field(
                        h,
                        "cpb_size_value_minus1[" +
                            std::to_string(i) + "][" +
                            std::to_string(j) + "]",
                        b.ue()
                    );

                    if (subPicHrdParamsPresentFlag)
                    {
                        field(
                            h,
                            "bit_rate_du_value_minus1[" +
                                std::to_string(i) + "][" +
                                std::to_string(j) + "]",
                            b.ue()
                        );

                        field(
                            h,
                            "cpb_size_du_value_minus1[" +
                                std::to_string(i) + "][" +
                                std::to_string(j) + "]",
                            b.ue()
                        );
                    }

                    field(
                        h,
                        "cbr_flag[" +
                            std::to_string(i) + "][" +
                            std::to_string(j) + "]",
                        b.u(1)
                    );
                }
            }

            // ---------------------------------------------------------------------
            // VCL HRD
            // ---------------------------------------------------------------------

            if (vclHrdParametersPresentFlag)
            {
                for (uint32_t j = 0;
                    j <= cpbCntMinus1;
                    ++j)
                {
                    field(
                        h,
                        "vcl_bit_rate_value_minus1[" +
                            std::to_string(i) + "][" +
                            std::to_string(j) + "]",
                        b.ue()
                    );

                    field(
                        h,
                        "vcl_cpb_size_value_minus1[" +
                            std::to_string(i) + "][" +
                            std::to_string(j) + "]",
                        b.ue()
                    );

                    if (subPicHrdParamsPresentFlag)
                    {
                        field(
                            h,
                            "vcl_bit_rate_du_value_minus1[" +
                                std::to_string(i) + "][" +
                                std::to_string(j) + "]",
                            b.ue()
                        );

                        field(
                            h,
                            "vcl_cpb_size_du_value_minus1[" +
                                std::to_string(i) + "][" +
                                std::to_string(j) + "]",
                            b.ue()
                        );
                    }

                    field(
                        h,
                        "vcl_cbr_flag[" +
                            std::to_string(i) + "][" +
                            std::to_string(j) + "]",
                        b.u(1)
                    );
                }
            }
        }
    }


    void parse_hevc_vui_parameters(
        BitReader& b,
        Header& h,
        uint32_t maxSubLayersMinus1)
    {
        // -------------------------------------------------------------------------
        // aspect_ratio_info_present_flag
        // -------------------------------------------------------------------------

        const uint32_t aspectRatioInfoPresent =
            static_cast<uint32_t>(b.u(1));

        field(
            h,
            "aspect_ratio_info_present_flag",
            aspectRatioInfoPresent
        );

        if (aspectRatioInfoPresent)
        {
            const uint32_t aspectRatioIdc =
                static_cast<uint32_t>(b.u(8));

            field(
                h,
                "aspect_ratio_idc",
                aspectRatioIdc
            );

            if (aspectRatioIdc == 255)
            {
                field(
                    h,
                    "sar_width",
                    b.u(16)
                );

                field(
                    h,
                    "sar_height",
                    b.u(16)
                );
            }
        }

        // -------------------------------------------------------------------------
        // overscan_info_present_flag
        // -------------------------------------------------------------------------

        const uint32_t overscanInfoPresent =
            static_cast<uint32_t>(b.u(1));

        field(
            h,
            "overscan_info_present_flag",
            overscanInfoPresent
        );

        if (overscanInfoPresent)
        {
            field(
                h,
                "overscan_appropriate_flag",
                b.u(1)
            );
        }

        // -------------------------------------------------------------------------
        // video_signal_type_present_flag
        // -------------------------------------------------------------------------

        const uint32_t videoSignalTypePresent =
            static_cast<uint32_t>(b.u(1));

        field(
            h,
            "video_signal_type_present_flag",
            videoSignalTypePresent
        );

        if (videoSignalTypePresent)
        {
            field(
                h,
                "video_format",
                b.u(3)
            );

            field(
                h,
                "video_full_range_flag",
                b.u(1)
            );

            const uint32_t colourDescriptionPresent =
                static_cast<uint32_t>(b.u(1));

            field(
                h,
                "colour_description_present_flag",
                colourDescriptionPresent
            );

            if (colourDescriptionPresent)
            {
                field(
                    h,
                    "colour_primaries",
                    b.u(8)
                );

                field(
                    h,
                    "transfer_characteristics",
                    b.u(8)
                );

                field(
                    h,
                    "matrix_coefficients",
                    b.u(8)
                );
            }
        }

        // -------------------------------------------------------------------------
        // chroma_loc_info_present_flag
        // -------------------------------------------------------------------------

        const uint32_t chromaLocInfoPresent =
            static_cast<uint32_t>(b.u(1));

        field(
            h,
            "chroma_loc_info_present_flag",
            chromaLocInfoPresent
        );

        if (chromaLocInfoPresent)
        {
            field(
                h,
                "chroma_sample_loc_type_top_field",
                b.ue()
            );

            field(
                h,
                "chroma_sample_loc_type_bottom_field",
                b.ue()
            );
        }

        // -------------------------------------------------------------------------
        // neutral_chroma_indication_flag
        // -------------------------------------------------------------------------

        field(
            h,
            "neutral_chroma_indication_flag",
            b.u(1)
        );

        // -------------------------------------------------------------------------
        // field_seq_flag
        // -------------------------------------------------------------------------

        field(
            h,
            "field_seq_flag",
            b.u(1)
        );

        // -------------------------------------------------------------------------
        // frame_field_info_present_flag
        // -------------------------------------------------------------------------

        field(
            h,
            "frame_field_info_present_flag",
            b.u(1)
        );

        // -------------------------------------------------------------------------
        // default_display_window_flag
        // -------------------------------------------------------------------------

        const uint32_t defaultDisplayWindowFlag =
            static_cast<uint32_t>(b.u(1));

        field(
            h,
            "default_display_window_flag",
            defaultDisplayWindowFlag
        );

        if (defaultDisplayWindowFlag)
        {
            field(
                h,
                "def_disp_win_left_offset",
                b.ue()
            );

            field(
                h,
                "def_disp_win_right_offset",
                b.ue()
            );

            field(
                h,
                "def_disp_win_top_offset",
                b.ue()
            );

            field(
                h,
                "def_disp_win_bottom_offset",
                b.ue()
            );
        }

        // -------------------------------------------------------------------------
        // vui_timing_info_present_flag
        // -------------------------------------------------------------------------

        const uint32_t vuiTimingInfoPresent =
            static_cast<uint32_t>(b.u(1));

        field(
            h,
            "vui_timing_info_present_flag",
            vuiTimingInfoPresent
        );

        if (vuiTimingInfoPresent)
        {
            field(
                h,
                "vui_num_units_in_tick",
                b.u(32)
            );

            field(
                h,
                "vui_time_scale",
                b.u(32)
            );

            const uint32_t pocProportionalToTiming =
                static_cast<uint32_t>(b.u(1));

            field(
                h,
                "vui_poc_proportional_to_timing_flag",
                pocProportionalToTiming
            );

            if (pocProportionalToTiming)
            {
                field(
                    h,
                    "vui_num_ticks_poc_diff_one_minus1",
                    b.ue()
                );
            }
        }

        // -------------------------------------------------------------------------
        // HRD parameters
        // -------------------------------------------------------------------------
        //
        // hrd_parameters(
        //     commonInfPresentFlag = 1,
        //     maxNumSubLayersMinus1
        // )
        //
        // We parse the complete syntax here so that the BitReader remains
        // synchronized when HRD is present.
        // -------------------------------------------------------------------------

        const bool hrdPresent =
            b.u(1) != 0;

        field(
            h,
            "hrd_parameters_present_flag",
            hrdPresent
        );

        if (hrdPresent)
        {
            parse_hevc_hrd_parameters(
                b,
                h,
                true,
                maxSubLayersMinus1
            );
        }

        // -------------------------------------------------------------------------
        // bitstream_restriction_flag
        // -------------------------------------------------------------------------

        const uint32_t bitstreamRestrictionPresent =
            static_cast<uint32_t>(b.u(1));

        field(
            h,
            "bitstream_restriction_flag",
            bitstreamRestrictionPresent
        );

        if (bitstreamRestrictionPresent)
        {
            field(
                h,
                "tiles_fixed_structure_flag",
                b.u(1)
            );

            field(
                h,
                "motion_vectors_over_pic_boundaries_flag",
                b.u(1)
            );

            field(
                h,
                "restricted_ref_pic_lists_flag",
                b.u(1)
            );

            field(
                h,
                "min_spatial_segmentation_idc",
                b.ue()
            );

            field(
                h,
                "max_bytes_per_pic_denom",
                b.ue()
            );

            field(
                h,
                "max_bits_per_min_cu_denom",
                b.ue()
            );

            field(
                h,
                "log2_max_mv_length_horizontal",
                b.ue()
            );

            field(
                h,
                "log2_max_mv_length_vertical",
                b.ue()
            );
        }
    }



    void parse_hevc_profile_tier_level(
        BitReader& b, Header& h,
        uint32_t profilePresentFlag,
        uint32_t maxNumSubLayersMinus1)
    {
        if (profilePresentFlag)
        {
            field(h, "general_profile_space", b.u(2));
            field(h, "general_tier_flag", b.u(1));
            field(h, "general_profile_idc", b.u(5));

            // general_profile_compatibility_flag[32]
            for (int i = 0; i < 32; ++i)
                field(h, "general_profile_compatibility_flag[" + std::to_string(i) + "]",
                        b.u(1));

            field(h, "general_progressive_source_flag", b.u(1));
            field(h, "general_interlaced_source_flag", b.u(1));
            field(h, "general_non_packed_constraint_flag", b.u(1));
            field(h, "general_frame_only_constraint_flag", b.u(1));

            /*
                * HEVC constraint flags.
                *
                * The exact interpretation depends on profile/version.
                * Consume the complete 44-bit general constraint area.
                */
            field(h, "general_reserved_zero_44bits", b.u(44));

            field(h, "general_level_idc", b.u(8));
        }

        bool subLayerProfilePresent[8] = {};
        bool subLayerLevelPresent[8]   = {};

        for (uint32_t i = 0; i < maxNumSubLayersMinus1; ++i)
        {
            subLayerProfilePresent[i] = b.u(1);
            field(h,
                    "sub_layer_profile_present_flag[" + std::to_string(i) + "]",
                    subLayerProfilePresent[i]);

            subLayerLevelPresent[i] = b.u(1);
            field(h,
                    "sub_layer_level_present_flag[" + std::to_string(i) + "]",
                    subLayerLevelPresent[i]);
        }

        if (maxNumSubLayersMinus1 > 0)
        {
            for (uint32_t i = maxNumSubLayersMinus1;
                    i < 8;
                    ++i)
            {
                field(h,
                        "reserved_zero_2bits[" + std::to_string(i) + "]",
                        b.u(2));
            }
        }

        for (uint32_t i = 0; i < maxNumSubLayersMinus1; ++i)
        {
            if (subLayerProfilePresent[i])
            {
                field(h,
                        "sub_layer_profile_space[" + std::to_string(i) + "]",
                        b.u(2));

                field(h,
                        "sub_layer_tier_flag[" + std::to_string(i) + "]",
                        b.u(1));

                field(h,
                        "sub_layer_profile_idc[" + std::to_string(i) + "]",
                        b.u(5));

                for (int j = 0; j < 32; ++j)
                {
                    field(h,
                            "sub_layer_profile_compatibility_flag[" +
                                std::to_string(i) + "][" +
                                std::to_string(j) + "]",
                            b.u(1));
                }

                field(h,
                        "sub_layer_progressive_source_flag[" + std::to_string(i) + "]",
                        b.u(1));

                field(h,
                        "sub_layer_interlaced_source_flag[" + std::to_string(i) + "]",
                        b.u(1));

                field(h,
                        "sub_layer_non_packed_constraint_flag[" +
                            std::to_string(i) + "]",
                        b.u(1));

                field(h,
                        "sub_layer_frame_only_constraint_flag[" +
                            std::to_string(i) + "]",
                        b.u(1));

                field(h,
                        "sub_layer_reserved_zero_44bits[" +
                            std::to_string(i) + "]",
                        b.u(44));
            }

            if (subLayerLevelPresent[i])
            {
                field(h,
                        "sub_layer_level_idc[" + std::to_string(i) + "]",
                        b.u(8));
            }
        }
    }


    HevcVps parse_hevc_vps(
        BitReader& b,
        Header& h)
    {
        HevcVps vps;

        // -------------------------------------------------------------------------
        // video_parameter_set_rbsp()
        // -------------------------------------------------------------------------

        vps.id = static_cast<uint32_t>(b.u(4));

        field(
            h,
            "vps_video_parameter_set_id",
            vps.id
        );

        vps.base_layer_internal_flag =
            b.u(1) != 0;

        field(
            h,
            "vps_base_layer_internal_flag",
            vps.base_layer_internal_flag
        );

        vps.base_layer_available_flag =
            b.u(1) != 0;

        field(
            h,
            "vps_base_layer_available_flag",
            vps.base_layer_available_flag
        );

        vps.max_layers_minus1 =
            static_cast<uint32_t>(b.u(6));

        field(
            h,
            "vps_max_layers_minus1",
            vps.max_layers_minus1
        );

        vps.max_sub_layers_minus1 =
            static_cast<uint32_t>(b.u(3));

        field(
            h,
            "vps_max_sub_layers_minus1",
            vps.max_sub_layers_minus1
        );

        vps.temporal_id_nesting_flag =
            b.u(1) != 0;

        field(
            h,
            "vps_temporal_id_nesting_flag",
            vps.temporal_id_nesting_flag
        );

        // -------------------------------------------------------------------------
        // Basic HEVC constraint
        // -------------------------------------------------------------------------
        //
        // When max_sub_layers_minus1 == 0, temporal_id_nesting_flag shall be 1.
        //
        // This is also checked by FFmpeg's HEVC parser.
        // -------------------------------------------------------------------------

        if (vps.max_sub_layers_minus1 == 0 &&
            !vps.temporal_id_nesting_flag)
        {
            throw std::runtime_error(
                "invalid HEVC VPS: "
                "vps_temporal_id_nesting_flag must be 1 "
                "when vps_max_sub_layers_minus1 is 0");
        }

        // -------------------------------------------------------------------------
        // Reserved bits
        // -------------------------------------------------------------------------

        const uint32_t reserved =
            static_cast<uint32_t>(b.u(16));

        field(
            h,
            "vps_reserved_0xffff_16bits",
            reserved
        );

        if (reserved != 0xffff)
        {
            throw std::runtime_error(
                "invalid HEVC VPS reserved bits");
        }

        // -------------------------------------------------------------------------
        // profile_tier_level()
        // -------------------------------------------------------------------------

        parse_hevc_profile_tier_level(
            b,
            h,
            1,
            vps.max_sub_layers_minus1
        );

        // -------------------------------------------------------------------------
        // vps_sub_layer_ordering_info_present_flag
        // -------------------------------------------------------------------------

        const bool subLayerOrderingPresent =
            b.u(1) != 0;

        field(
            h,
            "vps_sub_layer_ordering_info_present_flag",
            subLayerOrderingPresent
        );

        const uint32_t firstSubLayer =
            subLayerOrderingPresent
                ? 0
                : vps.max_sub_layers_minus1;

        for (uint32_t i = firstSubLayer;
            i <= vps.max_sub_layers_minus1;
            ++i)
        {
            const uint32_t maxDecPicBufferingMinus1 =
                static_cast<uint32_t>(b.ue());

            field(
                h,
                "vps_max_dec_pic_buffering_minus1[" +
                    std::to_string(i) + "]",
                maxDecPicBufferingMinus1
            );

            const uint32_t maxNumReorderPics =
                static_cast<uint32_t>(b.ue());

            field(
                h,
                "vps_max_num_reorder_pics[" +
                    std::to_string(i) + "]",
                maxNumReorderPics
            );

            if (maxNumReorderPics >
                maxDecPicBufferingMinus1)
            {
                throw std::runtime_error(
                    "invalid HEVC VPS: "
                    "vps_max_num_reorder_pics exceeds "
                    "vps_max_dec_pic_buffering_minus1");
            }

            const uint32_t maxLatencyIncreasePlus1 =
                static_cast<uint32_t>(b.ue());

            field(
                h,
                "vps_max_latency_increase_plus1[" +
                    std::to_string(i) + "]",
                maxLatencyIncreasePlus1
            );
        }

        // -------------------------------------------------------------------------
        // Layer sets
        // -------------------------------------------------------------------------

        const uint32_t maxLayerId =
            static_cast<uint32_t>(b.u(6));

        field(
            h,
            "vps_max_layer_id",
            maxLayerId
        );

        vps.num_layer_sets_minus1 =
            static_cast<uint32_t>(b.ue());

        field(
            h,
            "vps_num_layer_sets_minus1",
            vps.num_layer_sets_minus1
        );

        /*
        * Defensive limit.
        *
        * The actual allowed values are constrained by the HEVC profile/level.
        * This prevents malformed input from causing enormous loops.
        */
        constexpr uint32_t kMaxLayerSets = 4096;

        if (vps.num_layer_sets_minus1 >= kMaxLayerSets)
        {
            throw std::runtime_error(
                "HEVC VPS has too many layer sets");
        }

        for (uint32_t i = 1;i <= vps.num_layer_sets_minus1; ++i)
        {
            for (uint32_t j = 0;
                j <= maxLayerId;
                ++j)
            {
                field(
                    h,
                    "layer_id_included_flag[" +
                        std::to_string(i) + "][" +
                        std::to_string(j) + "]",
                    b.u(1)
                );
            }
        }

        // -------------------------------------------------------------------------
        // Timing information
        // -------------------------------------------------------------------------

        const bool timingPresent =
            b.u(1) != 0;

        field(
            h,
            "vps_timing_info_present_flag",
            timingPresent
        );

        if (timingPresent)
        {
            field(
                h,
                "vps_num_units_in_tick",
                b.u(32)
            );

            field(
                h,
                "vps_time_scale",
                b.u(32)
            );

            const bool pocProportionalToTiming =
                b.u(1) != 0;

            field(
                h,
                "vps_poc_proportional_to_timing_flag",
                pocProportionalToTiming
            );

            if (pocProportionalToTiming)
            {
                field(
                    h,
                    "vps_num_ticks_poc_diff_one_minus1",
                    b.ue()
                );
            }

            const uint32_t numHrdParameters =
                static_cast<uint32_t>(b.ue());

            field(
                h,
                "vps_num_hrd_parameters",
                numHrdParameters
            );

            constexpr uint32_t kMaxHrdParameters = 4096;

            if (numHrdParameters > kMaxHrdParameters)
            {
                throw std::runtime_error(
                    "HEVC VPS has too many HRD parameters");
            }

            for (uint32_t i = 0;i < numHrdParameters;++i)
            {
                const uint32_t hrdLayerSetIdx =
                    static_cast<uint32_t>(b.ue());

                field(
                    h,
                    "hrd_layer_set_idx[" +
                        std::to_string(i) + "]",
                    hrdLayerSetIdx
                );

                if (hrdLayerSetIdx > vps.num_layer_sets_minus1)
                {
                    throw std::runtime_error(
                        "invalid HEVC VPS: "
                        "hrd_layer_set_idx exceeds "
                        "vps_num_layer_sets_minus1");
                }

                bool cprmsPresent = true;

                if (i > 0)
                {
                    cprmsPresent =
                        b.u(1) != 0;

                    field(
                        h,
                        "cprms_present_flag[" +
                            std::to_string(i) + "]",
                        cprmsPresent
                    );
                }

                parse_hevc_hrd_parameters(
                    b,
                    h,
                    cprmsPresent,
                    vps.max_sub_layers_minus1
                );
            }
        }

        // -------------------------------------------------------------------------
        // VPS extension
        // -------------------------------------------------------------------------

        const bool extensionFlag =
            b.u(1) != 0;

        field(
            h,
            "vps_extension_flag",
            extensionFlag
        );

        if (extensionFlag)
        {
            while (b.more_rbsp_data())
            {
                field(
                    h,
                    "vps_extension_data_flag",
                    b.u(1)
                );
            }
        }

        return vps;
    }


    HevcSps parse_hevc_sps(
        BitReader& b,
        Header& h)
    {
        HevcSps sps;

        // =========================================================================
        // seq_parameter_set_rbsp()
        // =========================================================================

        // -------------------------------------------------------------------------
        // sps_video_parameter_set_id
        // -------------------------------------------------------------------------

        sps.vps_id =
            static_cast<uint32_t>(b.u(4));

        field(
            h,
            "sps_video_parameter_set_id",
            sps.vps_id
        );

        // -------------------------------------------------------------------------
        // sps_max_sub_layers_minus1
        // -------------------------------------------------------------------------

        sps.max_sub_layers_minus1 =
            static_cast<uint32_t>(b.u(3));

        field(
            h,
            "sps_max_sub_layers_minus1",
            sps.max_sub_layers_minus1
        );

        // -------------------------------------------------------------------------
        // sps_temporal_id_nesting_flag
        // -------------------------------------------------------------------------

        sps.temporal_id_nesting_flag =
            b.u(1) != 0;

        field(
            h,
            "sps_temporal_id_nesting_flag",
            sps.temporal_id_nesting_flag
        );

        if (sps.max_sub_layers_minus1 == 0 &&
            !sps.temporal_id_nesting_flag)
        {
            throw std::runtime_error(
                "invalid HEVC SPS: "
                "sps_temporal_id_nesting_flag must be 1 "
                "when sps_max_sub_layers_minus1 is 0");
        }

        // -------------------------------------------------------------------------
        // profile_tier_level()
        // -------------------------------------------------------------------------

        parse_hevc_profile_tier_level(
            b,
            h,
            1,
            sps.max_sub_layers_minus1
        );

        // -------------------------------------------------------------------------
        // sps_seq_parameter_set_id
        // -------------------------------------------------------------------------

        sps.id =
            static_cast<uint32_t>(b.ue());

        field(
            h,
            "sps_seq_parameter_set_id",
            sps.id
        );

        // -------------------------------------------------------------------------
        // chroma_format_idc
        // -------------------------------------------------------------------------

        sps.chroma_format_idc =
            static_cast<uint32_t>(b.ue());

        field(
            h,
            "chroma_format_idc",
            sps.chroma_format_idc
        );

        if (sps.chroma_format_idc > 3)
        {
            throw std::runtime_error(
                "invalid HEVC SPS chroma_format_idc");
        }

        if (sps.chroma_format_idc == 3)
        {
            sps.separate_colour_plane_flag =
                b.u(1) != 0;

            field(
                h,
                "separate_colour_plane_flag",
                sps.separate_colour_plane_flag
            );
        }

        // -------------------------------------------------------------------------
        // Picture dimensions
        // -------------------------------------------------------------------------

        sps.pic_width_in_luma_samples =
            static_cast<uint32_t>(b.ue());

        sps.pic_height_in_luma_samples =
            static_cast<uint32_t>(b.ue());

        if (sps.pic_width_in_luma_samples == 0 ||
            sps.pic_height_in_luma_samples == 0)
        {
            throw std::runtime_error(
                "invalid HEVC SPS picture dimensions");
        }

        field(
            h,
            "pic_width_in_luma_samples",
            sps.pic_width_in_luma_samples
        );

        field(
            h,
            "pic_height_in_luma_samples",
            sps.pic_height_in_luma_samples
        );

        // -------------------------------------------------------------------------
        // Conformance window
        // -------------------------------------------------------------------------

        const bool conformanceWindowFlag =
            b.u(1) != 0;

        field(
            h,
            "conformance_window_flag",
            conformanceWindowFlag
        );

        if (conformanceWindowFlag)
        {
            sps.conf_win_left_offset =
                static_cast<uint32_t>(b.ue());

            sps.conf_win_right_offset =
                static_cast<uint32_t>(b.ue());

            sps.conf_win_top_offset =
                static_cast<uint32_t>(b.ue());

            sps.conf_win_bottom_offset =
                static_cast<uint32_t>(b.ue());

            field(
                h,
                "conf_win_left_offset",
                sps.conf_win_left_offset
            );

            field(
                h,
                "conf_win_right_offset",
                sps.conf_win_right_offset
            );

            field(
                h,
                "conf_win_top_offset",
                sps.conf_win_top_offset
            );

            field(
                h,
                "conf_win_bottom_offset",
                sps.conf_win_bottom_offset
            );
        }

        // -------------------------------------------------------------------------
        // Derive displayed width / height
        // -------------------------------------------------------------------------
        //
        // SubWidthC and SubHeightC depend on chroma_format_idc.
        //
        // 4:0:0 -> 1, 1
        // 4:2:0 -> 2, 2
        // 4:2:2 -> 2, 1
        // 4:4:4 -> 1, 1
        //
        // When separate_colour_plane_flag is set, the picture is treated as
        // monochrome for the separate colour planes.
        // -------------------------------------------------------------------------

        uint32_t subWidthC = 1;
        uint32_t subHeightC = 1;

        if (!sps.separate_colour_plane_flag)
        {
            switch (sps.chroma_format_idc)
            {
                case 0:
                    subWidthC = 1;
                    subHeightC = 1;
                    break;

                case 1:
                    subWidthC = 2;
                    subHeightC = 2;
                    break;

                case 2:
                    subWidthC = 2;
                    subHeightC = 1;
                    break;

                case 3:
                    subWidthC = 1;
                    subHeightC = 1;
                    break;

                default:
                    throw std::runtime_error(
                        "invalid HEVC chroma_format_idc");
            }
        }

        const uint64_t cropWidth =
            static_cast<uint64_t>(
                sps.conf_win_left_offset +
                sps.conf_win_right_offset) *
            subWidthC;

        const uint64_t cropHeight =
            static_cast<uint64_t>(
                sps.conf_win_top_offset +
                sps.conf_win_bottom_offset) *
            subHeightC;

        if (cropWidth > sps.pic_width_in_luma_samples ||
            cropHeight > sps.pic_height_in_luma_samples)
        {
            throw std::runtime_error(
                "invalid HEVC SPS conformance window");
        }

        sps.width =
            sps.pic_width_in_luma_samples -
            static_cast<uint32_t>(cropWidth);

        sps.height =
            sps.pic_height_in_luma_samples -
            static_cast<uint32_t>(cropHeight);

        field(
            h,
            "width",
            sps.width
        );

        field(
            h,
            "height",
            sps.height
        );

        // -------------------------------------------------------------------------
        // Bit depth
        // -------------------------------------------------------------------------

        sps.bit_depth_luma_minus8 =
            static_cast<uint32_t>(b.ue());

        sps.bit_depth_chroma_minus8 =
            static_cast<uint32_t>(b.ue());

        field(
            h,
            "bit_depth_luma_minus8",
            sps.bit_depth_luma_minus8
        );

        field(
            h,
            "bit_depth_chroma_minus8",
            sps.bit_depth_chroma_minus8
        );

        field(
            h,
            "bit_depth_luma",
            sps.bit_depth_luma_minus8 + 8
        );

        field(
            h,
            "bit_depth_chroma",
            sps.bit_depth_chroma_minus8 + 8
        );

        // -------------------------------------------------------------------------
        // log2_max_pic_order_cnt_lsb_minus4
        // -------------------------------------------------------------------------
        //
        // This is CRITICAL for slice parsing.
        //
        // slice_pic_order_cnt_lsb uses:
        //
        //     log2_max_pic_order_cnt_lsb_minus4 + 4
        //
        // bits.
        //
        // Do NOT hardcode 8 in the slice parser.
        // -------------------------------------------------------------------------

        sps.log2_max_pic_order_cnt_lsb_minus4 =
            static_cast<uint32_t>(b.ue());

        if (sps.log2_max_pic_order_cnt_lsb_minus4 > 28)
        {
            throw std::runtime_error(
                "invalid HEVC SPS "
                "log2_max_pic_order_cnt_lsb_minus4");
        }

        field(
            h,
            "log2_max_pic_order_cnt_lsb_minus4",
            sps.log2_max_pic_order_cnt_lsb_minus4
        );

        // -------------------------------------------------------------------------
        // sps_sub_layer_ordering_info_present_flag
        // -------------------------------------------------------------------------

        const bool orderingPresent =
            b.u(1) != 0;

        field(
            h,
            "sps_sub_layer_ordering_info_present_flag",
            orderingPresent
        );

        const uint32_t firstSubLayer =
            orderingPresent
                ? 0
                : sps.max_sub_layers_minus1;

        for (uint32_t i = firstSubLayer;
            i <= sps.max_sub_layers_minus1;
            ++i)
        {
            const uint32_t maxDecPicBufferingMinus1 =
                static_cast<uint32_t>(b.ue());

            field(
                h,
                "sps_max_dec_pic_buffering_minus1[" +
                    std::to_string(i) + "]",
                maxDecPicBufferingMinus1
            );

            const uint32_t maxNumReorderPics =
                static_cast<uint32_t>(b.ue());

            field(
                h,
                "sps_max_num_reorder_pics[" +
                    std::to_string(i) + "]",
                maxNumReorderPics
            );

            if (maxNumReorderPics >
                maxDecPicBufferingMinus1)
            {
                throw std::runtime_error(
                    "invalid HEVC SPS: "
                    "max_num_reorder_pics exceeds "
                    "max_dec_pic_buffering_minus1");
            }

            const uint32_t maxLatencyIncreasePlus1 =
                static_cast<uint32_t>(b.ue());

            field(
                h,
                "sps_max_latency_increase_plus1[" +
                    std::to_string(i) + "]",
                maxLatencyIncreasePlus1
            );
        }

        // -------------------------------------------------------------------------
        // Coding block sizes
        // -------------------------------------------------------------------------

        sps.log2_min_luma_coding_block_size_minus3 =
            static_cast<uint32_t>(b.ue());

        sps.log2_diff_max_min_luma_coding_block_size =
            static_cast<uint32_t>(b.ue());

        field(
            h,
            "log2_min_luma_coding_block_size_minus3",
            sps.log2_min_luma_coding_block_size_minus3
        );

        field(
            h,
            "log2_diff_max_min_luma_coding_block_size",
            sps.log2_diff_max_min_luma_coding_block_size
        );

        // -------------------------------------------------------------------------
        // Transform block sizes
        // -------------------------------------------------------------------------

        sps.log2_min_luma_transform_block_size_minus2 =
            static_cast<uint32_t>(b.ue());

        sps.log2_diff_max_min_luma_transform_block_size =
            static_cast<uint32_t>(b.ue());

        field(
            h,
            "log2_min_luma_transform_block_size_minus2",
            sps.log2_min_luma_transform_block_size_minus2
        );

        field(
            h,
            "log2_diff_max_min_luma_transform_block_size",
            sps.log2_diff_max_min_luma_transform_block_size
        );

        sps.max_transform_hierarchy_depth_inter =
            static_cast<uint32_t>(b.ue());

        sps.max_transform_hierarchy_depth_intra =
            static_cast<uint32_t>(b.ue());

        field(
            h,
            "max_transform_hierarchy_depth_inter",
            sps.max_transform_hierarchy_depth_inter
        );

        field(
            h,
            "max_transform_hierarchy_depth_intra",
            sps.max_transform_hierarchy_depth_intra
        );

        // -------------------------------------------------------------------------
        // Derived coding-block values
        // -------------------------------------------------------------------------

        const uint32_t log2MinCbSize =
            sps.log2_min_luma_coding_block_size_minus3 + 3;

        if (log2MinCbSize > 30)
        {
            throw std::runtime_error(
                "invalid HEVC SPS minimum coding block size");
        }

        sps.min_cb_size =
            1u << log2MinCbSize;

        const uint32_t log2MaxCbSize =
            log2MinCbSize +
            sps.log2_diff_max_min_luma_coding_block_size;

        if (log2MaxCbSize > 30)
        {
            throw std::runtime_error(
                "invalid HEVC SPS maximum coding block size");
        }

        sps.max_cb_size =
            1u << log2MaxCbSize;

        field(
            h,
            "min_luma_coding_block_size",
            sps.min_cb_size
        );

        field(
            h,
            "max_luma_coding_block_size",
            sps.max_cb_size
        );

        // -------------------------------------------------------------------------
        // Scaling list
        // -------------------------------------------------------------------------

        sps.scaling_list_enabled_flag =
            b.u(1) != 0;

        field(
            h,
            "scaling_list_enabled_flag",
            sps.scaling_list_enabled_flag
        );

        if (sps.scaling_list_enabled_flag)
        {
            sps.sps_scaling_list_data_present_flag =
                b.u(1) != 0;

            field(
                h,
                "sps_scaling_list_data_present_flag",
                sps.sps_scaling_list_data_present_flag
            );

            if (sps.sps_scaling_list_data_present_flag)
            {
                parse_hevc_scaling_list_data(
                    b,
                    h
                );
            }
        }

        // -------------------------------------------------------------------------
        // AMP
        // -------------------------------------------------------------------------

        sps.amp_enabled_flag =
            b.u(1) != 0;

        field(
            h,
            "amp_enabled_flag",
            sps.amp_enabled_flag
        );

        // -------------------------------------------------------------------------
        // SAO
        // -------------------------------------------------------------------------

        sps.sample_adaptive_offset_enabled_flag =
            b.u(1) != 0;

        field(
            h,
            "sample_adaptive_offset_enabled_flag",
            sps.sample_adaptive_offset_enabled_flag
        );

        // -------------------------------------------------------------------------
        // PCM
        // -------------------------------------------------------------------------

        sps.pcm_enabled_flag =
            b.u(1) != 0;

        field(
            h,
            "pcm_enabled_flag",
            sps.pcm_enabled_flag
        );

        if (sps.pcm_enabled_flag)
        {
            sps.pcm_sample_bit_depth_luma_minus1 =
                static_cast<uint32_t>(b.u(4));

            sps.pcm_sample_bit_depth_chroma_minus1 =
                static_cast<uint32_t>(b.u(4));

            field(
                h,
                "pcm_sample_bit_depth_luma_minus1",
                sps.pcm_sample_bit_depth_luma_minus1
            );

            field(
                h,
                "pcm_sample_bit_depth_chroma_minus1",
                sps.pcm_sample_bit_depth_chroma_minus1
            );

            sps.log2_min_pcm_luma_coding_block_size_minus3 =
                static_cast<uint32_t>(b.ue());

            sps.log2_diff_max_min_pcm_luma_coding_block_size =
                static_cast<uint32_t>(b.ue());

            field(
                h,
                "log2_min_pcm_luma_coding_block_size_minus3",
                sps.log2_min_pcm_luma_coding_block_size_minus3
            );

            field(
                h,
                "log2_diff_max_min_pcm_luma_coding_block_size",
                sps.log2_diff_max_min_pcm_luma_coding_block_size
            );

            sps.pcm_loop_filter_disabled_flag =
                b.u(1) != 0;

            field(
                h,
                "pcm_loop_filter_disabled_flag",
                sps.pcm_loop_filter_disabled_flag
            );
        }

        // -------------------------------------------------------------------------
        // Short-term reference picture sets
        // -------------------------------------------------------------------------

        const uint32_t numShortTermRefPicSets =
            static_cast<uint32_t>(b.ue());

        field(
            h,
            "num_short_term_ref_pic_sets",
            numShortTermRefPicSets
        );

        constexpr uint32_t kMaxShortTermRefPicSets = 1024;

        if (numShortTermRefPicSets >
            kMaxShortTermRefPicSets)
        {
            throw std::runtime_error(
                "HEVC SPS contains too many "
                "short-term reference picture sets");
        }

        sps.short_term_ref_pic_sets.reserve(
            numShortTermRefPicSets
        );

        std::vector<HevcShortTermRps> parsedRps;

        parsedRps.reserve(
            numShortTermRefPicSets
        );

        for (uint32_t i = 0;
            i < numShortTermRefPicSets;
            ++i)
        {
            HevcShortTermRps rps =
                parse_hevc_short_term_ref_pic_set(
                    b,
                    h,
                    i,
                    numShortTermRefPicSets,
                    parsedRps
                );

            parsedRps.push_back(
                std::move(rps)
            );
        }

        sps.short_term_ref_pic_sets = std::move(parsedRps);


        // -------------------------------------------------------------------------
        // Long-term reference pictures
        // -------------------------------------------------------------------------

        sps.long_term_ref_pics_present_flag =
            b.u(1) != 0;

        field(
            h,
            "long_term_ref_pics_present_flag",
            sps.long_term_ref_pics_present_flag
        );

        if (sps.long_term_ref_pics_present_flag)
        {
            sps.num_long_term_ref_pics_sps =
                static_cast<uint32_t>(b.ue());

            field(
                h,
                "num_long_term_ref_pics_sps",
                sps.num_long_term_ref_pics_sps
            );

            constexpr uint32_t kMaxLongTermRefPics = 1024;

            if (sps.num_long_term_ref_pics_sps >
                kMaxLongTermRefPics)
            {
                throw std::runtime_error(
                    "HEVC SPS contains too many "
                    "long-term reference pictures");
            }

            const uint32_t pocBits =
                sps.log2_max_pic_order_cnt_lsb_minus4 + 4;

            sps.lt_ref_pic_poc_lsb_sps.resize(
                sps.num_long_term_ref_pics_sps
            );

            sps.used_by_curr_pic_lt_sps_flag.resize(
                sps.num_long_term_ref_pics_sps
            );

            for (uint32_t i = 0;
                i < sps.num_long_term_ref_pics_sps;
                ++i)
            {
                sps.lt_ref_pic_poc_lsb_sps[i] =
                    static_cast<uint32_t>(
                        b.u(pocBits)
                    );

                field(
                    h,
                    "lt_ref_pic_poc_lsb_sps[" +
                        std::to_string(i) + "]",
                    sps.lt_ref_pic_poc_lsb_sps[i]
                );

                sps.used_by_curr_pic_lt_sps_flag[i] =
                    static_cast<uint8_t>(
                        b.u(1)
                    );

                field(
                    h,
                    "used_by_curr_pic_lt_sps_flag[" +
                        std::to_string(i) + "]",
                    sps.used_by_curr_pic_lt_sps_flag[i]
                );
            }
        }

        // -------------------------------------------------------------------------
        // Temporal MVP
        // -------------------------------------------------------------------------

        sps.temporal_mvp_enabled_flag =
            b.u(1) != 0;

        field(
            h,
            "sps_temporal_mvp_enabled_flag",
            sps.temporal_mvp_enabled_flag
        );

        // -------------------------------------------------------------------------
        // Strong intra smoothing
        // -------------------------------------------------------------------------

        sps.strong_intra_smoothing_enabled_flag =
            b.u(1) != 0;

        field(
            h,
            "strong_intra_smoothing_enabled_flag",
            sps.strong_intra_smoothing_enabled_flag
        );

        // -------------------------------------------------------------------------
        // VUI
        // -------------------------------------------------------------------------

        const bool vuiPresent =
            b.u(1) != 0;

        field(
            h,
            "vui_parameters_present_flag",
            vuiPresent
        );

        if (vuiPresent)
        {
            parse_hevc_vui_parameters(
                b,
                h,
                sps.max_sub_layers_minus1
            );
        }

        // -------------------------------------------------------------------------
        // SPS extension
        // -------------------------------------------------------------------------

        const bool extensionFlag =
            b.u(1) != 0;

        field(
            h,
            "sps_extension_flag",
            extensionFlag
        );

        if (extensionFlag)
        {
            /*
            * We intentionally do not pretend that all HEVC extensions are
            * ordinary extension_data_flag bits.
            *
            * A full parser should dispatch the applicable range-extension /
            * multilayer / 3D / SCC syntax here based on the profile and
            * extension flags.
            *
            * For the base HEVC parser, consume extension_data_flag only after
            * the standard base syntax has been completely parsed.
            */

            while (b.more_rbsp_data())
            {
                field(
                    h,
                    "sps_extension_data_flag",
                    b.u(1)
                );
            }
        }

        // -------------------------------------------------------------------------
        // Derived CTB geometry
        // -------------------------------------------------------------------------
        //
        // CtbLog2SizeY =
        //     log2_min_luma_coding_block_size_minus3
        //     + 3
        //     + log2_diff_max_min_luma_coding_block_size
        //
        // PicWidthInCtbsY =
        //     ceil(pic_width_in_luma_samples / CtbSizeY)
        //
        // PicHeightInCtbsY =
        //     ceil(pic_height_in_luma_samples / CtbSizeY)
        //
        // These values are required for slice_segment_address.
        // -------------------------------------------------------------------------

        sps.log2_ctb_size =
            log2MinCbSize +
            sps.log2_diff_max_min_luma_coding_block_size;

        if (sps.log2_ctb_size > 30)
        {
            throw std::runtime_error(
                "invalid HEVC SPS CTB size");
        }

        sps.ctb_size =
            1u << sps.log2_ctb_size;

        sps.pic_width_in_ctbs =
            (
                sps.pic_width_in_luma_samples +
                sps.ctb_size - 1
            ) / sps.ctb_size;

        sps.pic_height_in_ctbs =
            (
                sps.pic_height_in_luma_samples +
                sps.ctb_size - 1
            ) / sps.ctb_size;

        if (sps.pic_width_in_ctbs == 0 ||
            sps.pic_height_in_ctbs == 0)
        {
            throw std::runtime_error(
                "invalid HEVC SPS CTB dimensions");
        }

        const uint64_t picSizeInCtbs =
            static_cast<uint64_t>(
                sps.pic_width_in_ctbs
            ) *
            static_cast<uint64_t>(
                sps.pic_height_in_ctbs
            );

        if (picSizeInCtbs >
            std::numeric_limits<uint32_t>::max())
        {
            throw std::runtime_error(
                "HEVC SPS picture has too many CTBs");
        }

        sps.pic_size_in_ctbs =
            static_cast<uint32_t>(
                picSizeInCtbs
            );

        field(
            h,
            "log2_ctb_size",
            sps.log2_ctb_size
        );

        field(
            h,
            "ctb_size",
            sps.ctb_size
        );

        field(
            h,
            "pic_width_in_ctbs",
            sps.pic_width_in_ctbs
        );

        field(
            h,
            "pic_height_in_ctbs",
            sps.pic_height_in_ctbs
        );

        field(
            h,
            "pic_size_in_ctbs",
            sps.pic_size_in_ctbs
        );

        return sps;
    }



    HevcPps parse_hevc_pps(
        BitReader& b,
        Header& h)
    {
        HevcPps pps;

        // =========================================================================
        // pic_parameter_set_rbsp()
        // =========================================================================

        // -------------------------------------------------------------------------
        // PPS / SPS IDs
        // -------------------------------------------------------------------------

        pps.id =
            static_cast<uint32_t>(b.ue());

        field(
            h,
            "pps_pic_parameter_set_id",
            pps.id
        );

        if (pps.id > 63)
        {
            throw std::runtime_error(
                "invalid HEVC PPS id");
        }

        pps.sps_id =
            static_cast<uint32_t>(b.ue());

        field(
            h,
            "pps_seq_parameter_set_id",
            pps.sps_id
        );

        if (pps.sps_id > 15)
        {
            throw std::runtime_error(
                "invalid HEVC PPS SPS id");
        }

        // -------------------------------------------------------------------------
        // Slice header flags
        // -------------------------------------------------------------------------

        pps.dependent_slice_segments_enabled_flag =
            b.u(1) != 0;

        field(
            h,
            "dependent_slice_segments_enabled_flag",
            pps.dependent_slice_segments_enabled_flag
        );

        pps.output_flag_present_flag =
            b.u(1) != 0;

        field(
            h,
            "output_flag_present_flag",
            pps.output_flag_present_flag
        );

        pps.num_extra_slice_header_bits =
            static_cast<uint32_t>(b.u(3));

        field(
            h,
            "num_extra_slice_header_bits",
            pps.num_extra_slice_header_bits
        );

        if (pps.num_extra_slice_header_bits > 2)
        {
            /*
            * The specification reserves values 3..7 for this syntax element
            * in the base HEVC version, although decoders are expected to allow
            * them. We therefore don't reject the stream.
            */
        }

        // -------------------------------------------------------------------------
        // Coding tools
        // -------------------------------------------------------------------------

        pps.sign_data_hiding_enabled_flag =
            b.u(1) != 0;

        field(
            h,
            "sign_data_hiding_enabled_flag",
            pps.sign_data_hiding_enabled_flag
        );

        pps.cabac_init_present_flag =
            b.u(1) != 0;

        field(
            h,
            "cabac_init_present_flag",
            pps.cabac_init_present_flag
        );

        // -------------------------------------------------------------------------
        // Default reference indices
        // -------------------------------------------------------------------------

        pps.num_ref_idx_l0_default_active_minus1 =
            static_cast<uint32_t>(b.ue());

        field(
            h,
            "num_ref_idx_l0_default_active_minus1",
            pps.num_ref_idx_l0_default_active_minus1
        );

        pps.num_ref_idx_l1_default_active_minus1 =
            static_cast<uint32_t>(b.ue());

        field(
            h,
            "num_ref_idx_l1_default_active_minus1",
            pps.num_ref_idx_l1_default_active_minus1
        );

        // -------------------------------------------------------------------------
        // Initial QP
        // -------------------------------------------------------------------------

        pps.init_qp_minus26 =
            static_cast<int32_t>(b.se());

        field(
            h,
            "init_qp_minus26",
            pps.init_qp_minus26
        );

        // -------------------------------------------------------------------------
        // Prediction / transform tools
        // -------------------------------------------------------------------------

        pps.constrained_intra_pred_flag =
            b.u(1) != 0;

        field(
            h,
            "constrained_intra_pred_flag",
            pps.constrained_intra_pred_flag
        );

        pps.transform_skip_enabled_flag =
            b.u(1) != 0;

        field(
            h,
            "transform_skip_enabled_flag",
            pps.transform_skip_enabled_flag
        );

        pps.cu_qp_delta_enabled_flag =
            b.u(1) != 0;

        field(
            h,
            "cu_qp_delta_enabled_flag",
            pps.cu_qp_delta_enabled_flag
        );

        if (pps.cu_qp_delta_enabled_flag)
        {
            pps.diff_cu_qp_delta_depth =
                static_cast<uint32_t>(b.ue());

            field(
                h,
                "diff_cu_qp_delta_depth",
                pps.diff_cu_qp_delta_depth
            );
        }

        // -------------------------------------------------------------------------
        // Chroma QP offsets
        // -------------------------------------------------------------------------

        pps.pps_cb_qp_offset =
            static_cast<int32_t>(b.se());

        field(
            h,
            "pps_cb_qp_offset",
            pps.pps_cb_qp_offset
        );

        pps.pps_cr_qp_offset =
            static_cast<int32_t>(b.se());

        field(
            h,
            "pps_cr_qp_offset",
            pps.pps_cr_qp_offset
        );

        pps.pps_slice_chroma_qp_offsets_present_flag =
            b.u(1) != 0;

        field(
            h,
            "pps_slice_chroma_qp_offsets_present_flag",
            pps.pps_slice_chroma_qp_offsets_present_flag
        );

        // -------------------------------------------------------------------------
        // Weighted prediction
        // -------------------------------------------------------------------------

        pps.weighted_pred_flag =
            b.u(1) != 0;

        field(
            h,
            "weighted_pred_flag",
            pps.weighted_pred_flag
        );

        pps.weighted_bipred_flag =
            b.u(1) != 0;

        field(
            h,
            "weighted_bipred_flag",
            pps.weighted_bipred_flag
        );

        // -------------------------------------------------------------------------
        // Transquant bypass
        // -------------------------------------------------------------------------

        pps.transquant_bypass_enabled_flag =
            b.u(1) != 0;

        field(
            h,
            "transquant_bypass_enabled_flag",
            pps.transquant_bypass_enabled_flag
        );

        // -------------------------------------------------------------------------
        // Tiles
        // -------------------------------------------------------------------------

        pps.tiles_enabled_flag =
            b.u(1) != 0;

        field(
            h,
            "tiles_enabled_flag",
            pps.tiles_enabled_flag
        );

        pps.entropy_coding_sync_enabled_flag =
            b.u(1) != 0;

        field(
            h,
            "entropy_coding_sync_enabled_flag",
            pps.entropy_coding_sync_enabled_flag
        );

        if (pps.tiles_enabled_flag)
        {
            pps.num_tile_columns_minus1 =
                static_cast<uint32_t>(b.ue());

            pps.num_tile_rows_minus1 =
                static_cast<uint32_t>(b.ue());

            field(
                h,
                "num_tile_columns_minus1",
                pps.num_tile_columns_minus1
            );

            field(
                h,
                "num_tile_rows_minus1",
                pps.num_tile_rows_minus1
            );

            constexpr uint32_t kMaxTiles = 1024;

            if (pps.num_tile_columns_minus1 >= kMaxTiles ||
                pps.num_tile_rows_minus1 >= kMaxTiles)
            {
                throw std::runtime_error(
                    "HEVC PPS has too many tiles");
            }

            pps.uniform_spacing_flag =
                b.u(1) != 0;

            field(
                h,
                "uniform_spacing_flag",
                pps.uniform_spacing_flag
            );

            if (!pps.uniform_spacing_flag)
            {
                /*
                * column_width[i]
                *
                * i = 0 .. num_tile_columns_minus1 - 1
                *
                * The final column width is inferred.
                */

                if (pps.num_tile_columns_minus1 > 0)
                {
                    pps.column_width.resize(
                        pps.num_tile_columns_minus1
                    );

                    for (uint32_t i = 0;
                        i < pps.num_tile_columns_minus1;
                        ++i)
                    {
                        pps.column_width[i] =
                            static_cast<uint32_t>(b.ue());

                        field(
                            h,
                            "column_width[" +
                                std::to_string(i) + "]",
                            pps.column_width[i]
                        );
                    }
                }

                /*
                * row_height[i]
                *
                * i = 0 .. num_tile_rows_minus1 - 1
                */
                if (pps.num_tile_rows_minus1 > 0)
                {
                    pps.row_height.resize(
                        pps.num_tile_rows_minus1
                    );

                    for (uint32_t i = 0;
                        i < pps.num_tile_rows_minus1;
                        ++i)
                    {
                        pps.row_height[i] =
                            static_cast<uint32_t>(b.ue());

                        field(
                            h,
                            "row_height[" +
                                std::to_string(i) + "]",
                            pps.row_height[i]
                        );
                    }
                }
            }
        }

        // -------------------------------------------------------------------------
        // Loop filtering across tiles
        // -------------------------------------------------------------------------

        if (pps.tiles_enabled_flag)
        {
            pps.loop_filter_across_tiles_enabled_flag =
                b.u(1) != 0;

            field(
                h,
                "loop_filter_across_tiles_enabled_flag",
                pps.loop_filter_across_tiles_enabled_flag
            );
        }

        // -------------------------------------------------------------------------
        // Loop filtering across slices
        // -------------------------------------------------------------------------

        pps.pps_loop_filter_across_slices_enabled_flag =
            b.u(1) != 0;

        field(
            h,
            "pps_loop_filter_across_slices_enabled_flag",
            pps.pps_loop_filter_across_slices_enabled_flag
        );

        // -------------------------------------------------------------------------
        // Deblocking filter control
        // -------------------------------------------------------------------------

        pps.deblocking_filter_control_present_flag =
            b.u(1) != 0;

        field(
            h,
            "deblocking_filter_control_present_flag",
            pps.deblocking_filter_control_present_flag
        );

        if (pps.deblocking_filter_control_present_flag)
        {
            pps.deblocking_filter_override_enabled_flag =
                b.u(1) != 0;

            field(
                h,
                "deblocking_filter_override_enabled_flag",
                pps.deblocking_filter_override_enabled_flag
            );

            pps.pps_deblocking_filter_disabled_flag =
                b.u(1) != 0;

            field(
                h,
                "pps_deblocking_filter_disabled_flag",
                pps.pps_deblocking_filter_disabled_flag
            );

            if (!pps.pps_deblocking_filter_disabled_flag)
            {
                pps.pps_beta_offset_div2 =
                    static_cast<int32_t>(b.se());

                pps.pps_tc_offset_div2 =
                    static_cast<int32_t>(b.se());

                field(
                    h,
                    "pps_beta_offset_div2",
                    pps.pps_beta_offset_div2
                );

                field(
                    h,
                    "pps_tc_offset_div2",
                    pps.pps_tc_offset_div2
                );
            }
        }

        // -------------------------------------------------------------------------
        // PPS scaling list
        // -------------------------------------------------------------------------

        pps.pps_scaling_list_data_present_flag =
            b.u(1) != 0;

        field(
            h,
            "pps_scaling_list_data_present_flag",
            pps.pps_scaling_list_data_present_flag
        );

        if (pps.pps_scaling_list_data_present_flag)
        {
            parse_hevc_scaling_list_data(
                b,
                h
            );
        }

        // -------------------------------------------------------------------------
        // Reference picture list modification
        // -------------------------------------------------------------------------

        pps.lists_modification_present_flag =
            b.u(1) != 0;

        field(
            h,
            "lists_modification_present_flag",
            pps.lists_modification_present_flag
        );

        // -------------------------------------------------------------------------
        // Parallel merge level
        // -------------------------------------------------------------------------

        pps.log2_parallel_merge_level_minus2 =
            static_cast<uint32_t>(b.ue());

        field(
            h,
            "log2_parallel_merge_level_minus2",
            pps.log2_parallel_merge_level_minus2
        );

        // -------------------------------------------------------------------------
        // Slice segment header extension
        // -------------------------------------------------------------------------

        pps.slice_segment_header_extension_present_flag =
            b.u(1) != 0;

        field(
            h,
            "slice_segment_header_extension_present_flag",
            pps.slice_segment_header_extension_present_flag
        );

        // -------------------------------------------------------------------------
        // PPS extension
        // -------------------------------------------------------------------------

        pps.pps_extension_present_flag =
            b.u(1) != 0;

        field(
            h,
            "pps_extension_present_flag",
            pps.pps_extension_present_flag
        );

        if (pps.pps_extension_present_flag)
        {
            /*
            * Base HEVC PPS extension syntax can contain range-extension,
            * multilayer and 3D-related syntax depending on the profile.
            *
            * Don't pretend those are ordinary extension_data_flag bits.
            * Until those profile-specific extensions are implemented,
            * consume the remaining extension bits conservatively.
            */
            while (b.more_rbsp_data())
            {
                field(
                    h,
                    "pps_extension_data_flag",
                    b.u(1)
                );
            }
        }

        return pps;
    }


    HevcSlice parse_hevc_slice_header(
        BitReader& b,
        Header& h,
        uint8_t nalType,
        const HevcParseState& state)
    {
        HevcSlice slice;

        // =========================================================================
        // Resolve RAP / IDR properties from NAL type.
        // =========================================================================

        const bool isRap =
            nalType >= 16 &&
            nalType <= 23;

        const bool isIdr =
            nalType == 19 ||
            nalType == 20;

        // =========================================================================
        // first_slice_segment_in_pic_flag
        // =========================================================================

        slice.first_slice_segment_in_pic_flag =
            b.u(1) != 0;

        field(
            h,
            "first_slice_segment_in_pic_flag",
            slice.first_slice_segment_in_pic_flag
        );

        // =========================================================================
        // no_output_of_prior_pics_flag
        // =========================================================================

        if (isRap)
        {
            slice.no_output_of_prior_pics_flag =
                b.u(1) != 0;

            field(
                h,
                "no_output_of_prior_pics_flag",
                slice.no_output_of_prior_pics_flag
            );
        }

        // =========================================================================
        // slice_pic_parameter_set_id
        // =========================================================================

        slice.pps_id =
            static_cast<uint32_t>(b.ue());

        field(
            h,
            "slice_pic_parameter_set_id",
            slice.pps_id
        );

        // =========================================================================
        // Resolve PPS
        // =========================================================================

        const auto ppsIt =
            state.pps.find(slice.pps_id);

        if (ppsIt == state.pps.end())
        {
            throw std::runtime_error(
                "HEVC slice references unavailable PPS id " +
                std::to_string(slice.pps_id));
        }

        const HevcPps& pps =
            ppsIt->second;

        // =========================================================================
        // Resolve SPS
        // =========================================================================

        const auto spsIt =
            state.sps.find(pps.sps_id);

        if (spsIt == state.sps.end())
        {
            throw std::runtime_error(
                "HEVC PPS references unavailable SPS id " +
                std::to_string(pps.sps_id));
        }

        const HevcSps& sps =
            spsIt->second;

        // =========================================================================
        // Validate first slice / dependent slice relationship
        // =========================================================================

        if (!slice.first_slice_segment_in_pic_flag)
        {
            if (pps.dependent_slice_segments_enabled_flag)
            {
                slice.dependent_slice_segment_flag =
                    b.u(1) != 0;

                field(
                    h,
                    "dependent_slice_segment_flag",
                    slice.dependent_slice_segment_flag
                );
            }

            // -------------------------------------------------------------
            // slice_segment_address
            // -------------------------------------------------------------

            const unsigned addressBits =
                hevc_ceil_log2(
                    sps.pic_size_in_ctbs
                );

            if (addressBits > 0)
            {
                slice.slice_segment_address =
                    static_cast<uint32_t>(
                        b.u(addressBits)
                    );
            }
            else
            {
                slice.slice_segment_address = 0;
            }

            field(
                h,
                "slice_segment_address",
                slice.slice_segment_address
            );

            if (slice.slice_segment_address >=
                sps.pic_size_in_ctbs)
            {
                throw std::runtime_error(
                    "invalid HEVC slice_segment_address");
            }
        }

        // =========================================================================
        // Dependent slice segment
        // =========================================================================
        //
        // A dependent slice segment inherits most of its header information from
        // the preceding independent slice segment.
        //
        // We cannot parse slice_type / POC / RPS here because those fields are
        // not present in the dependent segment.
        // =========================================================================

        if (slice.dependent_slice_segment_flag)
        {
            field(
                h,
                "slice_header_dependent",
                true
            );

            return slice;
        }

        // =========================================================================
        // slice_reserved_undetermined_flag[]
        // =========================================================================

        slice.slice_reserved_flag.resize(
            pps.num_extra_slice_header_bits
        );

        for (uint32_t i = 0;
            i < pps.num_extra_slice_header_bits;
            ++i)
        {
            slice.slice_reserved_flag[i] =
                static_cast<uint8_t>(
                    b.u(1)
                );

            field(
                h,
                "slice_reserved_undetermined_flag[" +
                    std::to_string(i) + "]",
                slice.slice_reserved_flag[i]
            );
        }

        // =========================================================================
        // slice_type
        // =========================================================================

        slice.slice_type =
            static_cast<uint32_t>(b.ue());

        if (slice.slice_type > 2)
        {
            throw std::runtime_error(
                "invalid HEVC slice_type");
        }

        field(
            h,
            "slice_type",
            slice.slice_type
        );

        static const char* sliceTypeNames[] =
        {
            "B",
            "P",
            "I"
        };

        field(
            h,
            "slice_type_name",
            sliceTypeNames[slice.slice_type]
        );

        // =========================================================================
        // pic_output_flag
        // =========================================================================

        if (pps.output_flag_present_flag)
        {
            slice.pic_output_flag =
                b.u(1) != 0;

            field(
                h,
                "pic_output_flag",
                slice.pic_output_flag
            );
        }

        // =========================================================================
        // separate_colour_plane_flag
        // =========================================================================

        if (sps.separate_colour_plane_flag)
        {
            slice.colour_plane_id =
                static_cast<uint32_t>(
                    b.u(2)
                );

            field(
                h,
                "colour_plane_id",
                slice.colour_plane_id
            );
        }

        // =========================================================================
        // POC / reference picture information
        // =========================================================================

        if (!isIdr)
        {
            const unsigned pocBits =
                sps.log2_max_pic_order_cnt_lsb_minus4 + 4;

            slice.slice_pic_order_cnt_lsb =
                static_cast<uint32_t>(
                    b.u(pocBits)
                );

            field(
                h,
                "slice_pic_order_cnt_lsb",
                slice.slice_pic_order_cnt_lsb
            );

            field(
                h,
                "poc",
                slice.slice_pic_order_cnt_lsb
            );

            // ---------------------------------------------------------------------
            // Short-term reference picture set
            // ---------------------------------------------------------------------


            HevcShortTermRps inlineRps;

            const HevcShortTermRps* activeRps = nullptr;

            if (!sps.short_term_ref_pic_sets.empty())
            {
                slice.short_term_ref_pic_set_sps_flag =
                    b.u(1) != 0;

                field(
                    h,
                    "short_term_ref_pic_set_sps_flag",
                    slice.short_term_ref_pic_set_sps_flag
                );

                if (slice.short_term_ref_pic_set_sps_flag)
                {
                    const unsigned rpsBits =
                        hevc_ceil_log2(
                            static_cast<uint32_t>(
                                sps.short_term_ref_pic_sets.size()
                            )
                        );

                    if (rpsBits > 0)
                    {
                        slice.short_term_ref_pic_set_idx =
                            static_cast<uint32_t>(
                                b.u(rpsBits)
                            );
                    }
                    else
                    {
                        slice.short_term_ref_pic_set_idx = 0;
                    }

                    field(
                        h,
                        "short_term_ref_pic_set_idx",
                        slice.short_term_ref_pic_set_idx
                    );

                    if (slice.short_term_ref_pic_set_idx >=
                        sps.short_term_ref_pic_sets.size())
                    {
                        throw std::runtime_error(
                            "invalid HEVC short-term RPS index");
                    }

                    activeRps =
                        &sps.short_term_ref_pic_sets[
                            slice.short_term_ref_pic_set_idx
                        ];
                }
                else
                {
                    // -------------------------------------------------------------
                    // Inline RPS
                    // -------------------------------------------------------------

                    const uint32_t inlineRpsIdx =
                        static_cast<uint32_t>(
                            sps.short_term_ref_pic_sets.size()
                        );

                    inlineRps =
                        parse_hevc_short_term_ref_pic_set(
                            b,
                            h,
                            inlineRpsIdx,
                            inlineRpsIdx,
                            sps.short_term_ref_pic_sets
                        );

                    activeRps =
                        &inlineRps;
                }
            }
            else
            {
                /*
                * No SPS short-term RPS exists.
                *
                * The syntax does not contain short_term_ref_pic_set_sps_flag.
                *
                * Therefore there are no short-term references.
                */
                activeRps = nullptr;
            }

            
            // =========================================================================
            // Long-term references
            // =========================================================================

            if (!isIdr &&
                sps.long_term_ref_pics_present_flag)
            {
                if (sps.num_long_term_ref_pics_sps > 0)
                {
                    slice.num_long_term_sps =
                        static_cast<uint32_t>(
                            b.ue()
                        );

                    field(
                        h,
                        "num_long_term_sps",
                        slice.num_long_term_sps
                    );

                    if (slice.num_long_term_sps >
                        sps.num_long_term_ref_pics_sps)
                    {
                        throw std::runtime_error(
                            "invalid HEVC num_long_term_sps");
                    }
                }

                slice.num_long_term_pics =
                    static_cast<uint32_t>(
                        b.ue()
                    );

                field(
                    h,
                    "num_long_term_pics",
                    slice.num_long_term_pics
                );

                constexpr uint32_t kMaxLongTermPics = 1024;

                if (slice.num_long_term_sps +
                        slice.num_long_term_pics >
                    kMaxLongTermPics)
                {
                    throw std::runtime_error(
                        "too many HEVC long-term references");
                }

                const uint32_t totalLongTerm =
                    slice.num_long_term_sps +
                    slice.num_long_term_pics;

                slice.lt_idx_sps.resize(
                    slice.num_long_term_sps
                );

                slice.poc_lsb_lt.resize(
                    totalLongTerm
                );

                slice.used_by_curr_pic_lt_flag.resize(
                    totalLongTerm
                );

                slice.delta_poc_msb_present_flag.resize(
                    totalLongTerm
                );

                slice.delta_poc_msb_cycle_lt.resize(
                    totalLongTerm
                );

                const unsigned pocBits =
                    sps.log2_max_pic_order_cnt_lsb_minus4 + 4;

                for (uint32_t i = 0;
                    i < totalLongTerm;
                    ++i)
                {
                    // -------------------------------------------------------------
                    // SPS long-term reference
                    // -------------------------------------------------------------

                    if (i < slice.num_long_term_sps)
                    {
                        const unsigned idxBits =
                            hevc_ceil_log2(
                                sps.num_long_term_ref_pics_sps
                            );

                        uint32_t idx = 0;

                        if (idxBits > 0)
                        {
                            idx =
                                static_cast<uint32_t>(
                                    b.u(idxBits)
                                );
                        }

                        slice.lt_idx_sps[i] =
                            idx;

                        field(
                            h,
                            "lt_idx_sps[" +
                                std::to_string(i) + "]",
                            idx
                        );

                        if (idx >=
                            sps.num_long_term_ref_pics_sps)
                        {
                            throw std::runtime_error(
                                "invalid HEVC lt_idx_sps");
                        }

                        slice.poc_lsb_lt[i] =
                            sps.lt_ref_pic_poc_lsb_sps[idx];

                        slice.used_by_curr_pic_lt_flag[i] =
                            sps.used_by_curr_pic_lt_sps_flag[idx];

                        field(
                            h,
                            "poc_lsb_lt[" +
                                std::to_string(i) + "]",
                            slice.poc_lsb_lt[i]
                        );

                        field(
                            h,
                            "used_by_curr_pic_lt_flag[" +
                                std::to_string(i) + "]",
                            slice.used_by_curr_pic_lt_flag[i]
                        );
                    }

                    // -------------------------------------------------------------
                    // Slice-signalled long-term reference
                    // -------------------------------------------------------------

                    else
                    {
                        slice.poc_lsb_lt[i] =
                            static_cast<uint32_t>(
                                b.u(pocBits)
                            );

                        field(
                            h,
                            "poc_lsb_lt[" +
                                std::to_string(i) + "]",
                            slice.poc_lsb_lt[i]
                        );

                        slice.used_by_curr_pic_lt_flag[i] =
                            b.u(1) != 0;

                        field(
                            h,
                            "used_by_curr_pic_lt_flag[" +
                                std::to_string(i) + "]",
                            slice.used_by_curr_pic_lt_flag[i]
                        );
                    }

                    // -------------------------------------------------------------
                    // delta_poc_msb_present_flag
                    // -------------------------------------------------------------

                    slice.delta_poc_msb_present_flag[i] =
                        b.u(1) != 0;

                    field(
                        h,
                        "delta_poc_msb_present_flag[" +
                            std::to_string(i) + "]",
                        slice.delta_poc_msb_present_flag[i]
                    );

                    if (slice.delta_poc_msb_present_flag[i])
                    {
                        slice.delta_poc_msb_cycle_lt[i] =
                            static_cast<uint32_t>(
                                b.ue()
                            );

                        field(
                            h,
                            "delta_poc_msb_cycle_lt[" +
                                std::to_string(i) + "]",
                            slice.delta_poc_msb_cycle_lt[i]
                        );
                    }
                }
            }

            // =========================================================================
            // Calculate NumPicTotalCurr
            // =========================================================================

            uint32_t numLongTermCurr = 0;

            for (uint32_t i = 0;
                i < slice.num_long_term_sps;
                ++i)
            {
                if (slice.used_by_curr_pic_lt_flag[i])
                    ++numLongTermCurr;
            }

            for (uint32_t i = slice.num_long_term_sps;
                i < slice.num_long_term_sps +
                    slice.num_long_term_pics;
                ++i)
            {
                if (slice.used_by_curr_pic_lt_flag[i])
                    ++numLongTermCurr;
            }

            slice.num_long_term_curr =
                numLongTermCurr;

            uint32_t numShortTermCurr = 0;

            if (activeRps != nullptr)
            {
                numShortTermCurr =
                    activeRps->num_used_by_curr_pic();
            }

            slice.num_pic_total_curr =
                numShortTermCurr +
                numLongTermCurr;

            field(
                h,
                "num_long_term_curr",
                numLongTermCurr
            );

            field(
                h,
                "num_short_term_curr",
                numShortTermCurr
            );

            field(
                h,
                "num_pic_total_curr",
                slice.num_pic_total_curr
            );

        }

        // =========================================================================
        // SAO
        // =========================================================================

        if (sps.sample_adaptive_offset_enabled_flag)
        {
            slice.slice_sao_luma_flag =
                b.u(1) != 0;

            field(
                h,
                "slice_sao_luma_flag",
                slice.slice_sao_luma_flag
            );

            if (sps.chroma_format_idc != 0)
            {
                slice.slice_sao_chroma_flag =
                    b.u(1) != 0;

                field(
                    h,
                    "slice_sao_chroma_flag",
                    slice.slice_sao_chroma_flag
                );
            }
        }

        // =========================================================================
        // Reference index counts
        // =========================================================================

        if (slice.slice_type == 1 ||
            slice.slice_type == 0)
        {
            slice.num_ref_idx_active_override_flag =
                b.u(1) != 0;

            field(
                h,
                "num_ref_idx_active_override_flag",
                slice.num_ref_idx_active_override_flag
            );

            if (slice.num_ref_idx_active_override_flag)
            {
                slice.num_ref_idx_l0_active_minus1 =
                    static_cast<uint32_t>(
                        b.ue()
                    );

                field(
                    h,
                    "num_ref_idx_l0_active_minus1",
                    slice.num_ref_idx_l0_active_minus1
                );

                if (slice.slice_type == 0)
                {
                    slice.num_ref_idx_l1_active_minus1 =
                        static_cast<uint32_t>(
                            b.ue()
                        );

                    field(
                        h,
                        "num_ref_idx_l1_active_minus1",
                        slice.num_ref_idx_l1_active_minus1
                    );
                }
            }
            else
            {
                slice.num_ref_idx_l0_active_minus1 =
                    pps.num_ref_idx_l0_default_active_minus1;

                slice.num_ref_idx_l1_active_minus1 =
                    pps.num_ref_idx_l1_default_active_minus1;
            }
        }

        // =========================================================================
        // Reference picture list modification
        // =========================================================================

        if (pps.lists_modification_present_flag)
        {
            parse_hevc_ref_pic_list_modification(
                b,
                h,
                slice,
                pps,
                slice.num_pic_total_curr
            );
        }


        // =========================================================================
        // mvd_l1_zero_flag
        // =========================================================================

        if (slice.slice_type == 0)
        {
            slice.mvd_l1_zero_flag =
                b.u(1) != 0;

            field(
                h,
                "mvd_l1_zero_flag",
                slice.mvd_l1_zero_flag
            );
        }


        // =========================================================================
        // cabac_init_flag
        // =========================================================================

        if (pps.cabac_init_present_flag &&
            slice.slice_type != 2)
        {
            slice.cabac_init_flag =
                b.u(1) != 0;

            field(
                h,
                "cabac_init_flag",
                slice.cabac_init_flag
            );
        }



        // =========================================================================
        // slice_qp_delta
        // =========================================================================

        slice.slice_qp_delta =
            static_cast<int32_t>(
                b.se()
            );

        field(
            h,
            "slice_qp_delta",
            slice.slice_qp_delta
        );



        // =========================================================================
        // Slice chroma QP offsets
        // =========================================================================

        if (pps.pps_slice_chroma_qp_offsets_present_flag)
        {
            slice.slice_cb_qp_offset =
                static_cast<int32_t>(
                    b.se()
                );

            slice.slice_cr_qp_offset =
                static_cast<int32_t>(
                    b.se()
                );

            field(
                h,
                "slice_cb_qp_offset",
                slice.slice_cb_qp_offset
            );

            field(
                h,
                "slice_cr_qp_offset",
                slice.slice_cr_qp_offset
            );
        }




        // =========================================================================
        // cu_chroma_qp_offset_enabled_flag
        // =========================================================================

        if (pps.pps_slice_chroma_qp_offsets_present_flag)
        {
            slice.cu_chroma_qp_offset_enabled_flag =
                b.u(1) != 0;

            field(
                h,
                "cu_chroma_qp_offset_enabled_flag",
                slice.cu_chroma_qp_offset_enabled_flag
            );
        }



        // =========================================================================
        // Temporal MVP / collocated reference
        // =========================================================================

        if (slice.slice_temporal_mvp_enabled_flag)
        {
            if (slice.slice_type == 0)
            {
                slice.collocated_from_l0_flag =
                    b.u(1) != 0;

                field(
                    h,
                    "collocated_from_l0_flag",
                    slice.collocated_from_l0_flag
                );
            }
            else
            {
                // P slice: collocated reference is from L0.
                slice.collocated_from_l0_flag = true;
            }

            const bool needCollocatedRefIdx =
                (slice.collocated_from_l0_flag &&
                slice.num_ref_idx_l0_active_minus1 > 0) ||
                (!slice.collocated_from_l0_flag &&
                slice.num_ref_idx_l1_active_minus1 > 0);

            if (needCollocatedRefIdx)
            {
                slice.collocated_ref_idx =
                    static_cast<uint32_t>(
                        b.ue()
                    );

                field(
                    h,
                    "collocated_ref_idx",
                    slice.collocated_ref_idx
                );
            }
        }

        // =========================================================================
        // Five minus max_num_merge_cand
        // =========================================================================

        slice.five_minus_max_num_merge_cand =
            static_cast<uint32_t>(
                b.ue()
            );

        field(
            h,
            "five_minus_max_num_merge_cand",
            slice.five_minus_max_num_merge_cand
        );

       
        // =========================================================================
        // Deblocking filter
        // =========================================================================

        if (pps.deblocking_filter_control_present_flag)
        {
            if (pps.deblocking_filter_override_enabled_flag)
            {
                slice.deblocking_filter_override_flag =
                    b.u(1) != 0;

                field(
                    h,
                    "deblocking_filter_override_flag",
                    slice.deblocking_filter_override_flag
                );
            }
            else
            {
                slice.deblocking_filter_override_flag =
                    false;
            }

            if (slice.deblocking_filter_override_flag)
            {
                slice.slice_deblocking_filter_disabled_flag =
                    b.u(1) != 0;

                field(
                    h,
                    "slice_deblocking_filter_disabled_flag",
                    slice.slice_deblocking_filter_disabled_flag
                );

                if (!slice.slice_deblocking_filter_disabled_flag)
                {
                    slice.slice_beta_offset_div2 =
                        static_cast<int32_t>(
                            b.se()
                        );

                    slice.slice_tc_offset_div2 =
                        static_cast<int32_t>(
                            b.se()
                        );

                    field(
                        h,
                        "slice_beta_offset_div2",
                        slice.slice_beta_offset_div2
                    );

                    field(
                        h,
                        "slice_tc_offset_div2",
                        slice.slice_tc_offset_div2
                    );
                }
            }
            else
            {
                // Inherit PPS values.

                slice.slice_deblocking_filter_disabled_flag =
                    pps.pps_deblocking_filter_disabled_flag;

                slice.slice_beta_offset_div2 =
                    pps.pps_beta_offset_div2;

                slice.slice_tc_offset_div2 =
                    pps.pps_tc_offset_div2;
            }
        }
        else
        {
            // Deblocking control isn't present in the PPS.
            // All values are inferred.

            slice.slice_deblocking_filter_disabled_flag =
                false;

            slice.slice_beta_offset_div2 = 0;
            slice.slice_tc_offset_div2 = 0;
        }


        // =========================================================================
        // Loop filter across slices
        // =========================================================================

        if (pps.pps_loop_filter_across_slices_enabled_flag &&
            (
                slice.slice_sao_luma_flag ||
                slice.slice_sao_chroma_flag ||
                !slice.slice_deblocking_filter_disabled_flag
            ))
        {
            slice.slice_loop_filter_across_slices_enabled_flag =
                b.u(1) != 0;

            field(
                h,
                "slice_loop_filter_across_slices_enabled_flag",
                slice.slice_loop_filter_across_slices_enabled_flag
            );
        }
        else
        {
            slice.slice_loop_filter_across_slices_enabled_flag =
                false;
        }



        // =========================================================================
        // Entry point offsets
        // =========================================================================

        const bool tilesOrWpp =
            pps.tiles_enabled_flag ||
            pps.entropy_coding_sync_enabled_flag;

        if (tilesOrWpp)
        {
            slice.num_entry_point_offsets =
                static_cast<uint32_t>(
                    b.ue()
                );

            field(
                h,
                "num_entry_point_offsets",
                slice.num_entry_point_offsets
            );

            constexpr uint32_t kMaxEntryPoints = 65535;

            if (slice.num_entry_point_offsets >
                kMaxEntryPoints)
            {
                throw std::runtime_error(
                    "too many HEVC entry point offsets");
            }

            if (slice.num_entry_point_offsets > 0)
            {
                slice.offset_len_minus1 =
                    static_cast<uint32_t>(
                        b.ue()
                    );

                field(
                    h,
                    "offset_len_minus1",
                    slice.offset_len_minus1
                );

                const unsigned offsetBits =
                    slice.offset_len_minus1 + 1;

                if (offsetBits > 32)
                {
                    throw std::runtime_error(
                        "invalid HEVC entry point offset length");
                }

                slice.entry_point_offset_minus1.resize(
                    slice.num_entry_point_offsets
                );

                for (uint32_t i = 0;
                    i < slice.num_entry_point_offsets;
                    ++i)
                {
                    const uint32_t offset =
                        static_cast<uint32_t>(
                            b.u(offsetBits)
                        );

                    slice.entry_point_offset_minus1[i] =
                        offset;

                    field(
                        h,
                        "entry_point_offset_minus1[" +
                            std::to_string(i) + "]",
                        offset
                    );
                }
            }
        }



        // =========================================================================
        // slice_segment_header_extension_data
        // =========================================================================

        if (pps.slice_segment_header_extension_present_flag)
        {
            slice.slice_segment_header_extension_length =
                static_cast<uint32_t>(
                    b.ue()
                );

            field(
                h,
                "slice_segment_header_extension_length",
                slice.slice_segment_header_extension_length
            );

            constexpr uint32_t kMaxExtensionBytes = 4096;

            if (slice.slice_segment_header_extension_length >
                kMaxExtensionBytes)
            {
                throw std::runtime_error(
                    "HEVC slice header extension too large");
            }

            slice.slice_segment_header_extension_data_byte.resize(
                slice.slice_segment_header_extension_length
            );

            for (uint32_t i = 0;
                i < slice.slice_segment_header_extension_length;
                ++i)
            {
                slice.slice_segment_header_extension_data_byte[i] =
                    static_cast<uint8_t>(
                        b.u(8)
                    );

                field(
                    h,
                    "slice_segment_header_extension_data_byte[" +
                        std::to_string(i) + "]",
                    slice.slice_segment_header_extension_data_byte[i]
                );
            }
        }

        return slice;
    }


    Header parse_hevc(const Bytes& d, uint64_t off,HevcParseState& parseState)
    {
        if (d.size() < 2) throw std::out_of_range("truncated NAL header");
        
    
        uint8_t type = (d[0] >> 1) & 0x3F;
        uint8_t layer_id = ((d[0] & 0x01) << 5) | (d[1] >> 3);
        uint8_t tid = d[1] & 0x07;
        

        if (tid == 0)
            throw std::runtime_error("invalid HEVC temporal_id_plus1");


        Header h{off, d.size(), "HEVC NAL", false, {}};
        
        field(h, "nal_unit_type", type);
        field(h, "nuh_layer_id", layer_id);
        field(h, "nuh_temporal_id_plus1", tid);


        static const char* types[] = {
                "TRAIL_N",             // 0
                "TRAIL_R",             // 1
                "TSA_N",               // 2
                "TSA_R",               // 3
                "STSA_N",              // 4
                "STSA_R",              // 5
                "RADL_N",              // 6
                "RADL_R",              // 7
                "RASL_N",              // 8
                "RASL_R",              // 9
                "RSV_VCL_N10",         // 10
                "RSV_VCL_R11",         // 11
                "RSV_VCL_N12",         // 12
                "RSV_VCL_R13",         // 13
                "RSV_VCL_N14",         // 14
                "RSV_VCL_R15",         // 15
                "BLA_W_LP",            // 16
                "BLA_W_RADL",          // 17
                "BLA_N_LP",            // 18
                "IDR_W_RADL",          // 19
                "IDR_N_LP",            // 20
                "CRA_NUT",             // 21
                "RSV_IRAP_VCL22",      // 22
                "RSV_IRAP_VCL23",      // 23
                "RSV_VCL24",            // 24
                "RSV_VCL25",            // 25
                "RSV_VCL26",            // 26
                "RSV_VCL27",            // 27
                "RSV_VCL28",            // 28
                "RSV_VCL29",            // 29
                "RSV_VCL30",            // 30
                "RSV_VCL31",            // 31
                "VPS",                  // 32
                "SPS",                  // 33
                "PPS",                  // 34
                "AUD",                  // 35
                "EOS",                  // 36
                "EOB",                  // 37
                "FD",                   // 38
                "PREFIX_SEI",           // 39
                "SUFFIX_SEI"            // 40
        };

        constexpr size_t kHevcNalTypeCount =
            sizeof(types) / sizeof(types[0]);

        h.type = type < kHevcNalTypeCount
            ? types[type]
            : "HEVC NAL";

        const bool is_vcl = type <= 31;
        const bool is_irap = type >= 16 && type <= 23;
        const bool is_idr = type == 19 || type == 20;

        h.keyframe = is_irap;


        Bytes r = rbsp(d, 2);
        BitReader b(r);
        try
        {
            if (type == 32)
            {
                auto vps = parse_hevc_vps(b, h);
                parseState.vps[vps.id] = std::move(vps);
            }
            else if (type == 33)
            {
                auto sps = parse_hevc_sps(b, h);
                parseState.sps[sps.id] = std::move(sps);
            }
            else if (type == 34)
            {
                auto pps = parse_hevc_pps(b, h);
                parseState.pps[pps.id] = std::move(pps);
            }
            else if (type <= 31)
            {
                const HevcSlice slice =
                    parse_hevc_slice_header(
                        b,
                        h,
                        type,
                        parseState
                    );

                (void)slice;
            }
            else if (type == 35)
            {
                field(h, "pic_type", b.u(3));
            }
        }
        catch (const std::exception& e)
        {
            field(h, "parse_error", e.what());
        }
        return h;
    }

}