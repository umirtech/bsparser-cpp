// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "avc_common.hpp"
#include "avc_pps.hpp"
#include "avc_sps.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <utility>

namespace bs {
namespace avc {

/*
 * -----------------------------------------------------------
 * AVC parameter-set manager
 * -----------------------------------------------------------
 *
 *     Slice
 *       |
 *       v
 *      PPS
 *       |
 *       v
 *      SPS
 *
 * AVC has no VPS; slices reference a PPS which references an
 * SPS.  PPS ids range 0..255, SPS ids range 0..31.
 */

class ParameterSetManager {
    std::array<std::optional<SequenceParameterSet>, kMaxSpsCount> sps_{};

    std::array<std::optional<PictureParameterSet>, kMaxPpsCount> pps_{};

   public:
    ParameterSetManager() = default;

    ParameterSetManager(const ParameterSetManager&) = delete;

    ParameterSetManager& operator=(const ParameterSetManager&) = delete;

    /*
     * -------------------------------------------------------
     * Clear
     * -------------------------------------------------------
     */

    void clear() noexcept {
        for (auto& value : sps_) {
            value.reset();
        }

        for (auto& value : pps_) {
            value.reset();
        }
    }

    /*
     * -------------------------------------------------------
     * SPS
     * -------------------------------------------------------
     */

    [[nodiscard]]
    bool store_sps(SequenceParameterSet sps) {
        const auto id = static_cast<std::size_t>(sps.seq_parameter_set_id);

        if (id >= sps_.size()) {
            return false;
        }

        sps_[id] = std::move(sps);

        return true;
    }

    [[nodiscard]]
    const SequenceParameterSet* find_sps(std::uint8_t id) const noexcept {
        if (id >= sps_.size()) {
            return nullptr;
        }

        const auto& value = sps_[id];

        if (!value) {
            return nullptr;
        }

        return &*value;
    }

    [[nodiscard]]
    bool has_sps(std::uint8_t id) const noexcept {
        return id < sps_.size() && sps_[id].has_value();
    }

    [[nodiscard]]
    bool remove_sps(std::uint8_t id) noexcept {
        if (id >= sps_.size() || !sps_[id]) {
            return false;
        }

        sps_[id].reset();
        return true;
    }

    /*
     * -------------------------------------------------------
     * PPS
     * -------------------------------------------------------
     */

    [[nodiscard]]
    bool store_pps(PictureParameterSet pps) {
        const auto id = static_cast<std::size_t>(pps.pic_parameter_set_id);

        if (id >= pps_.size()) {
            return false;
        }

        pps_[id] = std::move(pps);

        return true;
    }

    [[nodiscard]]
    const PictureParameterSet* find_pps(std::uint8_t id) const noexcept {
        if (id >= pps_.size()) {
            return nullptr;
        }

        const auto& value = pps_[id];

        if (!value) {
            return nullptr;
        }

        return &*value;
    }

    [[nodiscard]]
    bool has_pps(std::uint8_t id) const noexcept {
        return id < pps_.size() && pps_[id].has_value();
    }

    [[nodiscard]]
    bool remove_pps(std::uint8_t id) noexcept {
        if (id >= pps_.size() || !pps_[id]) {
            return false;
        }

        pps_[id].reset();
        return true;
    }

    /*
     * -------------------------------------------------------
     * Slice dependency resolution
     * -------------------------------------------------------
     */

    struct ResolvedParameterSets {
        const PictureParameterSet* pps = nullptr;
        const SequenceParameterSet* sps = nullptr;

        [[nodiscard]]
        bool valid() const noexcept {
            return pps != nullptr && sps != nullptr;
        }
    };

    [[nodiscard]]
    ResolvedParameterSets resolve(std::uint8_t pps_id) const noexcept {
        ResolvedParameterSets result{};

        result.pps = find_pps(pps_id);

        if (result.pps == nullptr) {
            return result;
        }

        result.sps = find_sps(result.pps->seq_parameter_set_id);

        if (result.sps == nullptr) {
            result.pps = nullptr;
        }

        return result;
    }

    /*
     * -------------------------------------------------------
     * Counts
     * -------------------------------------------------------
     */

    [[nodiscard]]
    std::size_t sps_count() const noexcept {
        std::size_t count = 0;

        for (const auto& value : sps_) {
            if (value) {
                ++count;
            }
        }

        return count;
    }

    [[nodiscard]]
    std::size_t pps_count() const noexcept {
        std::size_t count = 0;

        for (const auto& value : pps_) {
            if (value) {
                ++count;
            }
        }

        return count;
    }
};

}  // namespace avc
}  // namespace bs