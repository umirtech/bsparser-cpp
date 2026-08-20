// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "rbsp_bitstream_reader.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace bs {
namespace avc {

/*
 * Shared AVC syntax parse error.
 */
class ParseError : public std::runtime_error {
   public:
    explicit ParseError(const char* message) : std::runtime_error(message) {}

    explicit ParseError(const std::string& message) : std::runtime_error(message) {}
};

namespace detail {

/*
 * Bounded ue(v) read.
 *
 * The value must fit in [0, max_value].  This is both a
 * robustness guard against pathological streams and a check
 * that the syntax value is within the range the H.264 spec
 * permits for the given field.
 */
template <typename Reader>
[[nodiscard]]
inline std::uint32_t read_ue_max(Reader& reader, const char* field, std::uint32_t max_value) {
    const auto value = reader.read_ue();

    if (value > max_value) {
        throw ParseError(std::string(field) + " out of range: " + std::to_string(value));
    }

    return value;
}

/*
 * Bounded se(v) read.
 */
template <typename Reader>
[[nodiscard]]
inline std::int32_t read_se_bounded(Reader& reader, const char* field, std::int32_t max_abs) {
    const auto value = reader.read_se();

    if (value < -max_abs || value > max_abs) {
        throw ParseError(std::string(field) + " out of range: " + std::to_string(value));
    }

    return value;
}

/*
 * Ceil(log2(v)) for v >= 1.
 */
[[nodiscard]]
inline unsigned ceil_log2(std::uint32_t value) noexcept {
    unsigned bits = 0;
    std::uint32_t power = 1;

    while (power < value) {
        power <<= 1;
        ++bits;
    }

    return bits;
}

}  // namespace detail

}  // namespace avc
}  // namespace bs