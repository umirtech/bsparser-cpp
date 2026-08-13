#pragma once

#include <rbsp_bitstream_reader.hpp>
#include <hevc_scaling_list.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace bs {

/*
 * H.265 scaling_list_data()
 *
 * 7.3.4
 *
 * The syntax contains:
 *
 *   scaling_list_pred_mode_flag
 *   scaling_list_pred_matrix_id_delta
 *   scaling_list_dc_coef_minus8
 *   scaling_list_delta_coef
 *
 * The decoded coefficient matrices are stored in
 * ScalingListData.
 */


/*
 * -----------------------------------------------------------
 * Constants
 * -----------------------------------------------------------
 */

inline constexpr std::size_t kScalingListSizeCount = 4;

inline constexpr std::size_t kScalingListMatrixCount = 6;



[[nodiscard]]
constexpr std::size_t
scaling_list_matrix_size(
    std::size_t size_id) noexcept
{
    return std::size_t{1} << (size_id + 2);
}



/*
 * -----------------------------------------------------------
 * Scan order
 * -----------------------------------------------------------
 *
 * HEVC scaling-list coefficients are decoded in diagonal
 * scan order.
 *
 * For 4x4 / 8x8 / 16x16 this is the normal diagonal scan.
 *
 * For 32x32 the syntax uses a reduced 8x8 matrix.
 */


/*
 * Generate diagonal scan positions.
 */
inline void make_diagonal_scan(
    std::size_t width,
    std::size_t height,
    std::array<std::size_t, 64>& scan,
    std::size_t& count)
{
    count = 0;

    if (width == 0 || height == 0) {
        return;
    }

    /*
     * Standard HEVC diagonal traversal.
     */
    for (std::size_t diagonal = 0;
         diagonal < width + height - 1;
         ++diagonal) {

        const std::size_t row_start =
            diagonal < width
                ? 0
                : diagonal - width + 1;

        const std::size_t row_end =
            diagonal < height
                ? diagonal
                : height - 1;

        /*
         * Alternate direction on each diagonal.
         */
        if ((diagonal & 1u) == 0) {

            for (std::size_t row = row_start;
                 row <= row_end;
                 ++row) {

                const std::size_t col =
                    diagonal - row;

                if (count < scan.size()) {
                    scan[count++] =
                        row * width + col;
                }
            }

        } else {

            for (std::size_t row = row_end + 1;
                 row-- > row_start;) {

                const std::size_t col =
                    diagonal - row;

                if (count < scan.size()) {
                    scan[count++] =
                        row * width + col;
                }
            }
        }
    }
}


/*
 * -----------------------------------------------------------
 * Scaling-list matrix access
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline ScalingListMatrix&
scaling_list_matrix(
    ScalingListData& scaling_list,
    std::size_t size_id,
    std::size_t matrix_id)
{
    if (size_id >= scaling_list.matrices.size()) {
        throw std::out_of_range(
            "scaling_list: invalid sizeId");
    }

    if (matrix_id >=
        scaling_list.matrices[size_id].size()) {
        throw std::out_of_range(
            "scaling_list: invalid matrixId");
    }

    return scaling_list.matrices[size_id][matrix_id];
}


[[nodiscard]]
inline const ScalingListMatrix&
scaling_list_matrix(
    const ScalingListData& scaling_list,
    std::size_t size_id,
    std::size_t matrix_id)
{
    if (size_id >= scaling_list.matrices.size()) {
        throw std::out_of_range(
            "scaling_list: invalid sizeId");
    }

    if (matrix_id >=
        scaling_list.matrices[size_id].size()) {
        throw std::out_of_range(
            "scaling_list: invalid matrixId");
    }

    return scaling_list.matrices[size_id][matrix_id];
}


/*
 * -----------------------------------------------------------
 * Prediction matrix
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline ScalingListMatrix&
scaling_list_prediction_matrix(
    ScalingListData& scaling_list,
    std::size_t size_id,
    std::size_t matrix_id,
    std::uint32_t delta)
{
    if (delta > matrix_id) {
        throw std::runtime_error(
            "scaling_list: invalid "
            "predMatrixIdDelta");
    }

    const auto reference_id =
        matrix_id - delta;

    return scaling_list_matrix(
        scaling_list,
        size_id,
        reference_id);
}



/*
 * -----------------------------------------------------------
 * Matrix initialization
 * -----------------------------------------------------------
 */

inline void
initialize_scaling_list_matrix(
    ScalingListMatrix& matrix)
{
    matrix = {};

    matrix.pred_mode_flag = true;

    matrix.pred_matrix_id_delta = 0;

    matrix.dc_coef_minus8 = 8;

    matrix.dc_coef = 16;

    matrix.coefficients.fill(16);
}



/*
 * -----------------------------------------------------------
 * Copy predicted matrix
 * -----------------------------------------------------------
 */

inline void
copy_scaling_list_matrix(
    const ScalingListMatrix& source,
    ScalingListMatrix& destination)
{
    destination = source;
}


/*
 * -----------------------------------------------------------
 * Decode one scaling list
 * -----------------------------------------------------------
 */

inline void
parse_scaling_list_matrix(
    RbspBitstreamReader& bs,
    ScalingListData& scaling_list,
    std::size_t size_id,
    std::size_t matrix_id)
{
    auto& matrix =
        scaling_list_matrix(
            scaling_list,
            size_id,
            matrix_id);


    /*
     * scaling_list_pred_mode_flag
     */
    matrix.pred_mode_flag =
        bs.read_bit();


    if (!matrix.pred_mode_flag) {

        /*
         * ---------------------------------------------------
         * Prediction mode
         * ---------------------------------------------------
         *
         * scaling_list_pred_matrix_id_delta
         */
        matrix.pred_matrix_id_delta =
            bs.read_ue();


        if (matrix.pred_matrix_id_delta >
            matrix_id) {
            throw std::runtime_error(
                "scaling_list: "
                "pred_matrix_id_delta > matrixId");
        }


        /*
         * The matrix is predicted from another matrix.
         *
         * For the same sizeId, coefficients are copied.
         */
        const auto reference_id =
            matrix_id -
            matrix.pred_matrix_id_delta;


        const auto& reference =
            scaling_list_matrix(
                scaling_list,
                size_id,
                reference_id);


        copy_scaling_list_matrix(
            reference,
            matrix);


        /*
         * Restore the actual signaled prediction metadata.
         */
        matrix.pred_mode_flag = false;

        matrix.pred_matrix_id_delta =
            static_cast<std::uint32_t>(
                matrix.pred_matrix_id_delta);

        return;
    }


    /*
     * -------------------------------------------------------
     * Explicit matrix
     * -------------------------------------------------------
     */

    std::int32_t next_coef = 8;

    /*
     * For sizeId > 1:
     *
     * scaling_list_dc_coef_minus8
     */
    if (size_id > 1) {

        matrix.dc_coef_minus8 =
            bs.read_se();

        next_coef =
            matrix.dc_coef_minus8 + 8;
    } else {

        matrix.dc_coef_minus8 = 8;
    }


    matrix.dc_coef =
        static_cast<std::int32_t>(
            next_coef);


    /*
     * scaling_list_delta_coef
     */
    const std::size_t coefficient_count =
        scaling_list_coefficient_count(
            size_id);


    std::array<std::size_t, 64> scan{};
    std::size_t scan_count = 0;


    /*
     * sizeId 3 uses an 8x8 coefficient matrix.
     */
    const std::size_t scan_size =
        size_id == 3
            ? 8
            : scaling_list_matrix_size(
                  size_id);


    make_diagonal_scan(
        scan_size,
        scan_size,
        scan,
        scan_count);


    if (scan_count != coefficient_count) {
        throw std::runtime_error(
            "scaling_list: invalid scan size");
    }


    /*
     * First coefficient.
     *
     * The scaling-list DC coefficient is used as the
     * starting predictor.
     */
    std::int32_t last_coef =
        next_coef;


    for (std::size_t i = 0;
         i < coefficient_count;
         ++i) {

        const auto delta_coef =
            bs.read_se();

        /*
         * H.265:
         *
         * nextCoef =
         *     (lastCoef + deltaCoef + 256) % 256
         */
        const std::int32_t value =
            (last_coef +
             delta_coef +
             256) % 256;


        const auto position =
            scan[i];


        matrix.coefficients[position] =
            static_cast<std::int16_t>(
                value);


        last_coef = value;
    }
}


/*
 * -----------------------------------------------------------
 * Parse scaling_list_data()
 * -----------------------------------------------------------
 */

inline void
parse_scaling_list_data(
    RbspBitstreamReader& bs,
    ScalingListData& scaling_list)
{

    /*
     * HEVC:
     *
     * for(sizeId = 0; sizeId < 4; sizeId++)
     */
    for (std::size_t size_id = 0;
         size_id < 4;
         ++size_id) {

        const std::size_t matrix_count =
            scaling_list_matrix_count(
                size_id);


        /*
         * for(matrixId = 0;
         *     matrixId < 6;
         *     matrixId++)
         *
         * sizeId 3 has only two matrices.
         */
        for (std::size_t matrix_id = 0;
             matrix_id < matrix_count;
             ++matrix_id) {

            parse_scaling_list_matrix(
                bs,
                scaling_list,
                size_id,
                matrix_id);
        }
    }
}


/*
 * -----------------------------------------------------------
 * Scaling-list access
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline std::int16_t
scaling_list_coefficient(
    const ScalingListData& scaling_list,
    std::size_t size_id,
    std::size_t matrix_id,
    std::size_t index)
{
    const auto& matrix =
        scaling_list_matrix(
            scaling_list,
            size_id,
            matrix_id);

    if (index >= matrix.coefficients.size()) {
        throw std::out_of_range(
            "scaling_list: coefficient index");
    }

    return matrix.coefficients[index];
}


/*
 * -----------------------------------------------------------
 * Validation
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline bool
validate_scaling_list_data(
    const ScalingListData& scaling_list) noexcept
{
    for (std::size_t size_id = 0;
         size_id < 4;
         ++size_id) {

        const auto count =
            scaling_list_matrix_count(
                size_id);

        if (count >
            scaling_list.matrices[size_id].size()) {
            return false;
        }
    }

    return true;
}

} // namespace bs