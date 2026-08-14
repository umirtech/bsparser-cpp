#pragma once

#include "hevc_vps.hpp"
#include "hevc_sps.hpp"
#include "hevc_pps.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace bs {

/*
 * -----------------------------------------------------------
 * Parameter-set manager
 * -----------------------------------------------------------
 *
 * HEVC parameter-set relationships:
 *
 *     Slice
 *       |
 *       v
 *      PPS
 *       |
 *       v
 *      SPS
 *       |
 *       v
 *      VPS
 *
 * The manager provides ID-based lookup and lifetime management
 * for parsed parameter sets.
 *
 * It intentionally contains no bitstream/parser state.
 */

/*
 * -----------------------------------------------------------
 * ID limits
 * -----------------------------------------------------------
 */

inline constexpr std::size_t kMaxVpsCount = 16;

inline constexpr std::size_t kMaxSpsCount = 16;

inline constexpr std::size_t kMaxPpsCount = 64;

/*
 * -----------------------------------------------------------
 * Parameter-set manager
 * -----------------------------------------------------------
 */

class ParameterSetManager {
   private:
    std::array<std::optional<VideoParameterSet>, kMaxVpsCount> vps_{};

    std::array<std::optional<SequenceParameterSet>, kMaxSpsCount> sps_{};

    std::array<std::optional<PictureParameterSet>, kMaxPpsCount> pps_{};

   public:
    ParameterSetManager() = default;

    ParameterSetManager(const ParameterSetManager&) = default;

    ParameterSetManager(ParameterSetManager&&) noexcept = default;

    ParameterSetManager& operator=(const ParameterSetManager&) = default;

    ParameterSetManager& operator=(ParameterSetManager&&) noexcept = default;

    ~ParameterSetManager() = default;

    /*
     * -------------------------------------------------------
     * Clear
     * -------------------------------------------------------
     */

    void clear() noexcept {
        for (auto& value : vps_) {
            value.reset();
        }

        for (auto& value : sps_) {
            value.reset();
        }

        for (auto& value : pps_) {
            value.reset();
        }
    }

    void clear_vps() noexcept {
        for (auto& value : vps_) {
            value.reset();
        }
    }

    void clear_sps() noexcept {
        for (auto& value : sps_) {
            value.reset();
        }
    }

    void clear_pps() noexcept {
        for (auto& value : pps_) {
            value.reset();
        }
    }

    /*
     * -------------------------------------------------------
     * VPS
     * -------------------------------------------------------
     */

    [[nodiscard]]
    bool store_vps(VideoParameterSet vps) {
        const auto id = static_cast<std::size_t>(vps.vps_video_parameter_set_id);

        if (id >= vps_.size()) {
            return false;
        }

        vps_[id] = std::move(vps);

        return true;
    }

    [[nodiscard]]
    bool set_vps(const VideoParameterSet& vps) {
        const auto id = static_cast<std::size_t>(vps.vps_video_parameter_set_id);

        if (id >= vps_.size()) {
            return false;
        }

        vps_[id] = vps;

        return true;
    }

    [[nodiscard]]
    VideoParameterSet* find_vps(std::uint8_t id) noexcept {
        if (id >= vps_.size()) {
            return nullptr;
        }

        auto& value = vps_[id];

        if (!value) {
            return nullptr;
        }

        return &*value;
    }

    [[nodiscard]]
    const VideoParameterSet* find_vps(std::uint8_t id) const noexcept {
        if (id >= vps_.size()) {
            return nullptr;
        }

        const auto& value = vps_[id];

        if (!value) {
            return nullptr;
        }

        return &*value;
    }

    [[nodiscard]]
    bool has_vps(std::uint8_t id) const noexcept {
        return id < vps_.size() && vps_[id].has_value();
    }

    [[nodiscard]]
    bool remove_vps(std::uint8_t id) noexcept {
        if (id >= vps_.size()) {
            return false;
        }

        if (!vps_[id]) {
            return false;
        }

        vps_[id].reset();
        return true;
    }

    /*
     * -------------------------------------------------------
     * SPS
     * -------------------------------------------------------
     */

    [[nodiscard]]
    bool store_sps(SequenceParameterSet sps) {
        const auto id = static_cast<std::size_t>(sps.sps_seq_parameter_set_id);

        if (id >= sps_.size()) {
            return false;
        }

        sps_[id] = std::move(sps);

        return true;
    }

    [[nodiscard]]
    bool set_sps(const SequenceParameterSet& sps) {
        const auto id = static_cast<std::size_t>(sps.sps_seq_parameter_set_id);

        if (id >= sps_.size()) {
            return false;
        }

        sps_[id] = sps;

        return true;
    }

    [[nodiscard]]
    SequenceParameterSet* find_sps(std::uint8_t id) noexcept {
        if (id >= sps_.size()) {
            return nullptr;
        }

        auto& value = sps_[id];

        if (!value) {
            return nullptr;
        }

        return &*value;
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
        if (id >= sps_.size()) {
            return false;
        }

        if (!sps_[id]) {
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
        const auto id = static_cast<std::size_t>(pps.pps_pic_parameter_set_id);

        if (id >= pps_.size()) {
            return false;
        }

        pps_[id] = std::move(pps);

        return true;
    }

    [[nodiscard]]
    bool set_pps(const PictureParameterSet& pps) {
        const auto id = static_cast<std::size_t>(pps.pps_pic_parameter_set_id);

        if (id >= pps_.size()) {
            return false;
        }

        pps_[id] = pps;

        return true;
    }

    [[nodiscard]]
    PictureParameterSet* find_pps(std::uint8_t id) noexcept {
        if (id >= pps_.size()) {
            return nullptr;
        }

        auto& value = pps_[id];

        if (!value) {
            return nullptr;
        }

        return &*value;
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
        if (id >= pps_.size()) {
            return false;
        }

        if (!pps_[id]) {
            return false;
        }

        pps_[id].reset();
        return true;
    }

    /*
     * -------------------------------------------------------
     * Slice dependency resolution
     * -------------------------------------------------------
     *
     * Resolve:
     *
     *     PPS -> SPS -> VPS
     *
     * This is useful immediately before slice parsing.
     */

    struct ResolvedParameterSets {
        const PictureParameterSet* pps = nullptr;
        const SequenceParameterSet* sps = nullptr;
        const VideoParameterSet* vps = nullptr;

        [[nodiscard]]
        bool valid() const noexcept {
            return pps != nullptr && sps != nullptr;
        }
    };

    [[nodiscard]]
    ResolvedParameterSets resolve_pps(std::uint8_t pps_id) const noexcept {
        ResolvedParameterSets result{};

        result.pps = find_pps(pps_id);

        if (result.pps == nullptr) {
            return result;
        }

        /*
         * PPS references SPS using:
         *
         *     pps_seq_parameter_set_id
         */
        const auto sps_id = result.pps->pps_seq_parameter_set_id;

        if (sps_id >= sps_.size()) {
            result.pps = nullptr;
            return result;
        }

        result.sps = find_sps(static_cast<std::uint8_t>(sps_id));

        if (result.sps == nullptr) {
            result.pps = nullptr;
            return result;
        }

        /*
         * SPS references VPS using:
         *
         *     sps_video_parameter_set_id
         */
        const auto vps_id = result.sps->sps_video_parameter_set_id;

        if (vps_id >= vps_.size()) {
            return result;
        }

        result.vps = find_vps(vps_id);

        return result;
    }

    /*
     * -------------------------------------------------------
     * Counts
     * -------------------------------------------------------
     */

    [[nodiscard]]
    std::size_t vps_count() const noexcept {
        std::size_t count = 0;

        for (const auto& value : vps_) {
            if (value) {
                ++count;
            }
        }

        return count;
    }

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

    /*
     * -------------------------------------------------------
     * Capacity
     * -------------------------------------------------------
     */

    [[nodiscard]]
    static constexpr std::size_t vps_capacity() noexcept {
        return kMaxVpsCount;
    }

    [[nodiscard]]
    static constexpr std::size_t sps_capacity() noexcept {
        return kMaxSpsCount;
    }

    [[nodiscard]]
    static constexpr std::size_t pps_capacity() noexcept {
        return kMaxPpsCount;
    }
};

/*
 * -----------------------------------------------------------
 * Convenience aliases
 * -----------------------------------------------------------
 */

using BsParameterSetManager = ParameterSetManager;

/*
 * -----------------------------------------------------------
 * Standalone resolution helper
 * -----------------------------------------------------------
 */

[[nodiscard]]
inline ParameterSetManager::ResolvedParameterSets resolve_parameter_sets(
    const ParameterSetManager& manager, std::uint8_t pps_id
) noexcept {
    return manager.resolve_pps(pps_id);
}

}  // namespace bs