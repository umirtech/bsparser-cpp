#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace bs {

/*
 * H.265 scaling_list_data()
 *
 * sizeId:
 *
 *     0 -> 4x4
 *     1 -> 8x8
 *     2 -> 16x16
 *     3 -> 32x32
 *
 * matrixId:
 *
 *     sizeId 0,1,2 -> 0..5
 *     sizeId 3     -> 0,3
 *
 * The syntax stores up to 64 coefficients for every
 * matrix. For sizeId > 1 there is also a DC coefficient.
 */

constexpr std::size_t kScalingListSizeIds = 4;
constexpr std::size_t kScalingListMatrixIds = 6;
constexpr std::size_t kScalingListMaxCoefficients = 64;

/*
 * One scaling-list matrix.
 *
 * This contains both the syntax representation and the
 * decoded coefficient matrix.
 */
struct ScalingListMatrix {
    /*
     * scaling_list_pred_mode_flag
     *
     * false:
     *     matrix is predicted from another matrix.
     *
     * true:
     *     coefficients are explicitly coded.
     */
    bool pred_mode_flag = false;

    /*
     * scaling_list_pred_matrix_id_delta
     *
     * Only meaningful when pred_mode_flag == false.
     */
    std::uint32_t pred_matrix_id_delta = 0;

    /*
     * scaling_list_dc_coef_minus8
     *
     * Only present for:
     *
     *     sizeId > 1
     *
     * when pred_mode_flag == true.
     */
    std::int32_t dc_coef_minus8 = 0;

    /*
     * Decoded DC coefficient.
     *
     * Defaults to 8 as specified by the scaling-list
     * initialization process.
     */
    std::int32_t dc_coef = 8;

    /*
     * Number of transform coefficients represented by
     * this matrix.
     *
     * 4x4  -> 16
     * 8x8  -> 64
     * 16x16 -> 64
     * 32x32 -> 64
     */
    std::uint8_t coefficient_count = 0;

    /*
     * Final decoded scaling-list coefficients.
     *
     * Values are stored as uint8_t because the H.265
     * derivation wraps the coefficient using modulo 256.
     */
    std::array<std::uint8_t, kScalingListMaxCoefficients> coefficients{};

    [[nodiscard]]
    constexpr std::size_t size() const noexcept {
        return coefficient_count;
    }

    [[nodiscard]]
    constexpr std::uint8_t operator[](std::size_t index) const noexcept {
        return coefficients[index];
    }
};

/*
 * Complete scaling_list_data().
 */
struct ScalingListData {
    /*
     * [sizeId][matrixId]
     *
     * Only valid matrixId values for a particular sizeId
     * should be consumed by the parser.
     */
    std::array<std::array<ScalingListMatrix, kScalingListMatrixIds>, kScalingListSizeIds>
        matrices{};

    [[nodiscard]]
    constexpr ScalingListMatrix& matrix(std::size_t size_id, std::size_t matrix_id) noexcept {
        return matrices[size_id][matrix_id];
    }

    [[nodiscard]]
    constexpr const ScalingListMatrix& matrix(
        std::size_t size_id, std::size_t matrix_id
    ) const noexcept {
        return matrices[size_id][matrix_id];
    }
};

/*
 * Return the number of coefficients for sizeId.
 *
 * H.265:
 *
 *     coefNum = Min(64, 1 << (4 + (sizeId << 1)))
 */
[[nodiscard]]
constexpr std::size_t scaling_list_coefficient_count(std::size_t size_id) noexcept {
    if (size_id >= kScalingListSizeIds) {
        return 0;
    }

    const std::size_t count = std::size_t{1} << (4 + (size_id << 1));

    return count < 64 ? count : 64;
}

/*
 * Return the actual matrix dimension.
 */
[[nodiscard]]
constexpr std::size_t scaling_list_dimension(std::size_t size_id) noexcept {
    switch (size_id) {
        case 0:
            return 4;

        case 1:
            return 8;

        case 2:
            return 16;

        case 3:
            return 32;

        default:
            return 0;
    }
}

/*
 * Return whether a matrixId is signaled for the
 * specified sizeId.
 *
 * H.265:
 *
 *     for(sizeId = 0; sizeId < 4; sizeId++)
 *         for(matrixId = 0;
 *             matrixId < 6;
 *             matrixId += (sizeId == 3) ? 3 : 1)
 */
[[nodiscard]]
constexpr bool valid_scaling_list_matrix_id(std::size_t size_id, std::size_t matrix_id) noexcept {
    if (size_id >= 4 || matrix_id >= 6) {
        return false;
    }

    if (size_id == 3) {
        return matrix_id == 0 || matrix_id == 3;
    }

    return true;
}

/*
 * Return the number of matrices signaled for sizeId.
 */
[[nodiscard]]
constexpr std::size_t scaling_list_matrix_count(std::size_t size_id) noexcept {
    if (size_id >= 4) {
        return 0;
    }

    return size_id == 3 ? 2 : 6;
}

/*
 * Return the next matrixId according to the H.265 syntax.
 */
[[nodiscard]]
constexpr std::size_t next_scaling_list_matrix_id(
    std::size_t size_id, std::size_t matrix_id
) noexcept {
    return matrix_id + ((size_id == 3) ? 3 : 1);
}

/*
 * H.265 default scaling lists.
 *
 * These are useful when scaling_list_data() isn't present
 * and the decoder needs the standard default matrices.
 *
 * 4x4:
 *
 *     16 coefficients
 *
 * 8x8 / 16x16 / 32x32:
 *
 *     64 coefficients
 *
 * For larger matrices the first 8x8 matrix is used as the
 * basis according to the scaling-list derivation rules.
 */
inline constexpr std::array<std::uint8_t, 16> kDefaultScalingList4x4 = {
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16
};

/*
 * Default intra 8x8 scaling list.
 */
inline constexpr std::array<std::uint8_t, 64> kDefaultScalingList8x8Intra = {
    16, 16, 16, 16, 17, 18, 21, 24, 16, 16, 16, 16, 17, 18, 21, 24, 17, 17, 17, 17, 18, 21,
    24, 25, 18, 18, 18, 18, 21, 24, 25, 28, 18, 18, 18, 18, 24, 25, 28, 30, 21, 21, 21, 21,
    25, 28, 30, 32, 24, 24, 24, 24, 28, 30, 32, 35, 25, 25, 25, 25, 30, 32, 35, 36
};

/*
 * Default inter 8x8 scaling list.
 */
inline constexpr std::array<std::uint8_t, 64> kDefaultScalingList8x8Inter = {
    16, 16, 16, 16, 17, 18, 20, 24, 16, 16, 16, 16, 17, 18, 20, 24, 17, 17, 17, 17, 18, 20,
    24, 25, 18, 18, 18, 18, 20, 24, 25, 28, 18, 18, 18, 18, 24, 25, 28, 30, 20, 20, 20, 20,
    25, 28, 30, 32, 24, 24, 24, 24, 28, 30, 32, 35, 25, 25, 25, 25, 30, 32, 35, 36
};

/*
 * Return the scan-position coefficient index.
 *
 * The scaling-list syntax uses diagonal scan order rather
 * than simple raster order.
 *
 * For the syntax data model we keep coefficients in scan
 * order. A decoder that needs raster order can perform the
 * diagonal scan mapping separately.
 */
[[nodiscard]]
constexpr std::size_t scaling_list_scan_index(
    std::size_t size_id, std::size_t coefficient
) noexcept {
    /*
     * 4x4 and 8x8 have straightforward fixed dimensions.
     *
     * The complete diagonal scan table belongs to the
     * transform/scanning implementation rather than this
     * metadata structure.
     *
     * For now this function returns the syntax coefficient
     * index unchanged.
     */
    if (size_id >= 4 || coefficient >= 64) {
        return 0;
    }

    return coefficient;
}

/*
 * Initialize a matrix to its default state.
 */
inline void initialize_scaling_list_matrix(ScalingListMatrix& matrix, std::size_t size_id) {
    matrix = {};

    matrix.coefficient_count = static_cast<std::uint8_t>(scaling_list_coefficient_count(size_id));

    matrix.dc_coef = 8;

    for (std::size_t i = 0; i < matrix.coefficient_count; ++i) {
        matrix.coefficients[i] = 16;
    }
}

/*
 * Initialize all scaling-list matrices.
 */
inline void initialize_scaling_list_data(ScalingListData& data) {
    for (std::size_t size_id = 0; size_id < 4; ++size_id) {
        for (std::size_t matrix_id = 0; matrix_id < 6; ++matrix_id) {
            initialize_scaling_list_matrix(data.matrix(size_id, matrix_id), size_id);
        }
    }
}

/*
 * Apply the H.265 modulo-256 coefficient derivation.
 *
 * scaling_list_data() does:
 *
 *     nextCoef =
 *         (nextCoef + scaling_list_delta_coef + 256)
 *         % 256
 */
[[nodiscard]]
constexpr std::uint8_t scaling_list_next_coefficient(
    std::int32_t current, std::int32_t delta
) noexcept {
    const std::int32_t value = (current + delta + 256) % 256;

    return static_cast<std::uint8_t>(value);
}

/*
 * Decode scaling_list_dc_coef_minus8.
 */
[[nodiscard]]
constexpr std::int32_t scaling_list_dc_coefficient(std::int32_t dc_coef_minus8) noexcept {
    return dc_coef_minus8 + 8;
}

/*
 * Return the prediction source matrix ID.
 *
 * H.265:
 *
 *     scaling_list_pred_matrix_id_delta
 *
 * is subtracted from matrixId.
 */
[[nodiscard]]
constexpr std::size_t scaling_list_prediction_matrix_id(
    std::size_t matrix_id, std::uint32_t delta
) noexcept {
    if (delta > matrix_id) {
        return 0;
    }

    return matrix_id - delta;
}

}  // namespace bs