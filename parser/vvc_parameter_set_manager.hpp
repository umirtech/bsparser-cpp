#pragma once

#include "vvc_ph.hpp"
#include "vvc_pps.hpp"
#include "vvc_sps.hpp"
#include "vvc_vps.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace bs {
namespace vvc {

/*
 * -----------------------------------------------------------
 * VVC parameter-set manager
 * -----------------------------------------------------------
 *
 * Stores the parameter-set-like NALs (DCI/OPI/VPS/SPS/PPS/PH)
 * by ID, mirroring the HEVC/AVC managers. IDs are small fixed
 * ranges, so the stores are small arrays.
 */
class ParameterSetManager {
   public:
    void store_dci(const Dci& dci) {
        dci_ = dci;
    }

    void store_opi(const Opi& opi) {
        opi_ = opi;
    }

    void store_vps(const VideoParameterSet& vps) {
        if (vps.vps_id < vps_.size()) {
            vps_[vps.vps_id] = vps;
        }
    }

    void store_sps(const SequenceParameterSet& sps) {
        if (sps.sps_id < sps_.size()) {
            sps_[sps.sps_id] = sps;
        }
    }

    void store_pps(const PictureParameterSet& pps) {
        if (pps.pps_id < pps_.size()) {
            pps_[pps.pps_id] = pps;
        }
    }

    void store_ph(const PictureHeader& ph) {
        ph_ = ph;
    }

    [[nodiscard]]
    const Dci* dci() const noexcept {
        return dci_ ? &*dci_ : nullptr;
    }

    [[nodiscard]]
    const Opi* opi() const noexcept {
        return opi_ ? &*opi_ : nullptr;
    }

    [[nodiscard]]
    const PictureHeader* ph() const noexcept {
        return ph_ ? &*ph_ : nullptr;
    }

    [[nodiscard]]
    const VideoParameterSet* find_vps(std::uint8_t id) const noexcept {
        return id < vps_.size() && vps_[id] ? &vps_[id].value() : nullptr;
    }

    [[nodiscard]]
    const SequenceParameterSet* find_sps(std::uint8_t id) const noexcept {
        return id < sps_.size() && sps_[id] ? &sps_[id].value() : nullptr;
    }

    [[nodiscard]]
    const PictureParameterSet* find_pps(std::uint8_t id) const noexcept {
        return id < pps_.size() && pps_[id] ? &pps_[id].value() : nullptr;
    }

    /*
     * Resolve a PPS and the SPS it references.
     */
    struct Resolved {
        const PictureParameterSet* pps = nullptr;
        const SequenceParameterSet* sps = nullptr;
        const VideoParameterSet* vps = nullptr;
    };

    [[nodiscard]]
    Resolved resolve(std::uint8_t pps_id) const noexcept {
        Resolved r;

        r.pps = find_pps(pps_id);

        if (r.pps != nullptr) {
            r.sps = find_sps(r.pps->sps_id);
            if (r.sps != nullptr) {
                r.vps = find_vps(r.sps->vps_id);
            }
        }

        return r;
    }

    void clear() noexcept {
        dci_.reset();
        opi_.reset();
        ph_.reset();

        for (auto& e : vps_)
            e.reset();
        for (auto& e : sps_)
            e.reset();
        for (auto& e : pps_)
            e.reset();
    }

   private:
    std::optional<Dci> dci_{};
    std::optional<Opi> opi_{};
    std::optional<PictureHeader> ph_{};

    std::array<std::optional<VideoParameterSet>, 16> vps_{};
    std::array<std::optional<SequenceParameterSet>, 16> sps_{};
    std::array<std::optional<PictureParameterSet>, 64> pps_{};
};

}  // namespace vvc
}  // namespace bs
