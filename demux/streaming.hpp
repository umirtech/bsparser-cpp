// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "detect.hpp"
#include "file_source.hpp"
#include "index.hpp"
#include "stream.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bs {
namespace demux {

namespace detail {

class DemuxerImpl {
   public:
    bool open_path(const std::string& path) {
        if (!source_.open(path)) {
            return false;
        }
        return build();
    }

    bool open_fd(int fd, bool close_on_destroy = false) {
        if (!source_.open_fd(fd, close_on_destroy)) {
            return false;
        }
        return build();
    }

    bool open_span(std::span<const std::uint8_t> data) {
        if (!source_.open_span(data)) {
            return false;
        }
        return build();
    }

    bool build() {
        if (!source_.ok() || source_.data().empty()) {
            return false;
        }

        container_ = sniff(source_.data());

        /*
         * Container detail parsers read fields straight out of the input
         * buffer.  A malformed file must make Demuxer::init() return false,
         * never throw into the caller (no exceptions cross this API).
         */
        try {
            bool ok = false;
            switch (container_) {
                case Container::Mp4:
                    ok = build_mp4(source_, index_);
                    break;
                case Container::Mkv:
                    ok = build_mkv(source_, index_);
                    break;
                case Container::MpegTs:
                    ok = build_ts(source_, index_);
                    break;
                case Container::Flv:
                    ok = build_flv(source_, index_);
                    break;
                case Container::Avi:
                    ok = build_avi(source_, index_);
                    break;
                case Container::Ivf:
                    ok = build_ivf(source_, index_);
                    break;
                case Container::Ogg:
                    ok = build_ogg(source_, index_);
                    break;
                case Container::Ps:
                    ok = build_ps(source_, index_);
                    break;
                case Container::Wav:
                    ok = build_wav(source_, index_);
                    break;
                default:
                    return false;
            }
            if (ok) {
                fill_missing_durations();
            }
            return ok;
        } catch (...) {
            index_ = IndexBuildResult{};
            container_ = Container::Unknown;
            return false;
        }
    }

    /* Some containers carry no per-sample duration (TS/FLV/MKV/IVF/PS).
     * Derive it from consecutive PTS deltas in the same track so callers
     * see a meaningful value; the last sample keeps its predecessor's. */
    void fill_missing_durations() {
        for (std::size_t t = 0; t < index_.samples.size(); ++t) {
            auto& samples = index_.samples[t];
            bool any_missing = false;
            for (const auto& s : samples) {
                if (s.duration == 0) {
                    any_missing = true;
                    break;
                }
            }
            if (!any_missing || samples.size() < 2) {
                continue;
            }
            for (std::size_t i = 0; i + 1 < samples.size(); ++i) {
                if (samples[i].duration == 0) {
                    const std::int64_t d = samples[i + 1].pts - samples[i].pts;
                    samples[i].duration = d > 0 ? d : 0;
                }
            }
            if (samples.back().duration == 0 && samples.size() >= 2) {
                samples.back().duration = samples[samples.size() - 2].duration;
            }
        }
    }

    [[nodiscard]]
    bool ok() const noexcept {
        return container_ != Container::Unknown && !index_.tracks.empty();
    }

    [[nodiscard]]
    Container container() const noexcept {
        return container_;
    }

    [[nodiscard]]
    const ContainerMetadata& metadata() const noexcept {
        return index_.metadata;
    }

    [[nodiscard]]
    FileSource& source() noexcept {
        return source_;
    }

    [[nodiscard]]
    const FileSource& source() const noexcept {
        return source_;
    }

    FileSource source_{};
    Container container_ = Container::Unknown;
    IndexBuildResult index_{};
};

}  // namespace detail

/*
 * -----------------------------------------------------------
 * Track handle
 * -----------------------------------------------------------
 *
 * A lightweight handle into a DemuxerImpl.  It owns no payload;
 * next() returns spans into the demuxer's mmap (or internal
 * buffer) and advances one sample/access unit at a time.
 */
class Track : public TrackView {
   public:
    Track() = default;

    Track(std::shared_ptr<detail::DemuxerImpl> impl, std::size_t index)
        : impl_(std::move(impl)), index_(index) {
        if (valid()) {
            static_cast<TrackView&>(*this) = impl_->index_.tracks[index_];
        }
    }

    [[nodiscard]]
    bool valid() const noexcept {
        return impl_ != nullptr && index_ < impl_->index_.tracks.size();
    }

    [[nodiscard]]
    const TrackView& view() const noexcept {
        return *this;
    }

    [[nodiscard]]
    bool hasNext() const noexcept {
        return valid() && position_ < impl_->index_.samples[index_].size();
    }

    [[nodiscard]]
    bool hasnext() const noexcept {
        return hasNext();
    }

    [[nodiscard]]
    bool has_next() const noexcept {
        return hasNext();
    }

    bool next(SampleView& out) {
        if (!hasNext()) {
            return false;
        }

        const SampleEntry& e = impl_->index_.samples[index_][position_];

        SampleView sv;
        sv.track_id = impl_->index_.tracks[index_].id;
        sv.index = position_;
        sv.pts = e.pts;
        sv.dts = e.dts;
        sv.duration = e.duration;
        sv.is_key = e.is_key;
        sv.framing = e.framing;

        if (e.is_param_set) {
            const auto& ps = impl_->index_.param_sets[index_];
            sv.data = std::span<const std::uint8_t>(ps.data(), e.size);
            sv.mmap_backed = false;
        } else {
            sv.data = impl_->source_.data().subspan(e.offset, e.size);
            sv.mmap_backed = impl_->source_.mmap_backed();
        }

        ++position_;
        out = sv;
        return true;
    }

    SampleView next() {
        SampleView sv;
        if (!next(sv)) {
            return {};
        }
        return sv;
    }

    bool seek(std::size_t sample_index) {
        if (!valid()) {
            return false;
        }

        const auto& samples = impl_->index_.samples[index_];

        if (sample_index >= samples.size()) {
            position_ = samples.size();
            return false;
        }

        position_ = sample_index;
        return true;
    }

    bool seek_time(std::int64_t pts) {
        if (!valid()) {
            return false;
        }

        const auto& samples = impl_->index_.samples[index_];
        if (samples.empty()) {
            position_ = 0;
            return false;
        }

        std::size_t i = 0;
        for (; i < samples.size(); ++i) {
            if (samples[i].pts >= pts) {
                break;
            }
        }

        position_ = i;
        return i < samples.size();
    }

    /* Keyframe-aware seek: find the last sample with pts <= target that is
     * marked as a keyframe, so decoding can start there.  Falls back to the
     * first sample when no keyframe precedes the target. */
    bool seek_time_keyframe(std::int64_t pts) {
        if (!valid()) {
            return false;
        }
        const auto& samples = impl_->index_.samples[index_];
        if (samples.empty()) {
            position_ = 0;
            return false;
        }

        // Binary search for the first sample with pts > target.
        std::size_t lo = 0;
        std::size_t hi = samples.size();
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2u;
            if (samples[mid].pts <= pts) {
                lo = mid + 1u;
            } else {
                hi = mid;
            }
        }
        std::size_t candidate = lo != 0u ? lo - 1u : 0u;

        // Walk back to the nearest keyframe at or before the target.
        while (candidate > 0u && !samples[candidate].is_key) {
            --candidate;
        }
        if (!samples[candidate].is_key) {
            // No keyframe at/before target: use the first keyframe, if any.
            candidate = 0u;
            while (candidate < samples.size() && !samples[candidate].is_key) {
                ++candidate;
            }
            if (candidate == samples.size()) {
                position_ = 0;
                return false;
            }
        }
        position_ = candidate;
        return true;
    }

    [[nodiscard]]
    std::size_t position() const noexcept {
        return position_;
    }

    [[nodiscard]]
    std::size_t sampleCount() const noexcept {
        return valid() ? impl_->index_.samples[index_].size() : 0;
    }

   private:
    std::shared_ptr<detail::DemuxerImpl> impl_{};
    std::size_t index_ = 0;
    std::size_t position_ = 0;
};

/*
 * InterleavedReader walks all tracks at once and returns the next sample in
 * decode (DTS) order.  Each track keeps its own cursor, so consumers do not
 * have to implement A/V synchronization themselves.
 */
class InterleavedReader {
   public:
    explicit InterleavedReader(std::shared_ptr<detail::DemuxerImpl> impl)
        : impl_(std::move(impl)) {
        if (impl_) {
            cursors_.assign(impl_->index_.samples.size(), 0u);
        }
    }

    [[nodiscard]] bool valid() const noexcept {
        return impl_ != nullptr;
    }

    [[nodiscard]] bool hasNext() const noexcept {
        if (!impl_) {
            return false;
        }
        for (std::size_t i = 0; i < cursors_.size(); ++i) {
            if (cursors_[i] < impl_->index_.samples[i].size()) {
                return true;
            }
        }
        return false;
    }

    bool next(SampleView& out) {
        if (!hasNext()) {
            return false;
        }
        // Pick the track whose next sample has the smallest DTS
        // (fall back to PTS when DTS is unavailable).
        std::size_t best = cursors_.size();
        std::int64_t best_time = 0;
        for (std::size_t i = 0; i < cursors_.size(); ++i) {
            if (cursors_[i] >= impl_->index_.samples[i].size()) {
                continue;
            }
            const auto& s = impl_->index_.samples[i][cursors_[i]];
            const std::int64_t time = s.dts != 0 ? s.dts : s.pts;
            if (best == cursors_.size() || time < best_time) {
                best = i;
                best_time = time;
            }
        }
        if (best == cursors_.size()) {
            return false;
        }
        const auto& e = impl_->index_.samples[best][cursors_[best]++];
        SampleView sv;
        sv.track_id = impl_->index_.tracks[best].id;
        sv.index = cursors_[best] - 1u;
        sv.pts = e.pts;
        sv.dts = e.dts;
        sv.duration = e.duration;
        sv.is_key = e.is_key;
        sv.framing = e.framing;
        if (e.is_param_set) {
            const auto& ps = impl_->index_.param_sets[best];
            sv.data = std::span<const std::uint8_t>(ps.data(), e.size);
            sv.mmap_backed = false;
        } else {
            sv.data = impl_->source_.data().subspan(e.offset, e.size);
            sv.mmap_backed = impl_->source_.mmap_backed();
        }
        out = sv;
        return true;
    }

    SampleView next() {
        SampleView sv;
        return next(sv) ? sv : SampleView{};
    }

   private:
    std::shared_ptr<detail::DemuxerImpl> impl_{};
    std::vector<std::size_t> cursors_{};
};

/*
 * -----------------------------------------------------------
 * Universal demuxer
 * -----------------------------------------------------------
 */
class Demuxer {
   public:
    Demuxer() = default;

    /*
     * Universal initializer: detects the container and returns an
     * initialized Demuxer.  The static form is the primary API; the
     * non-static load() is provided for callers that prefer to reuse
     * an existing object.
     */
    [[nodiscard]]
    static Demuxer init(const std::string& path) {
        Demuxer d;
        d.load(path);
        return d;
    }

    [[nodiscard]]
    static Demuxer init(int fd, bool close_on_destroy = false) {
        Demuxer d;
        d.load(fd, close_on_destroy);
        return d;
    }

    [[nodiscard]]
    static Demuxer init(std::span<const std::uint8_t> data) {
        Demuxer d;
        d.load(data);
        return d;
    }

    bool load(const std::string& path) {
        impl_ = std::make_shared<detail::DemuxerImpl>();
        return impl_->open_path(path);
    }

    bool load(int fd, bool close_on_destroy = false) {
        impl_ = std::make_shared<detail::DemuxerImpl>();
        return impl_->open_fd(fd, close_on_destroy);
    }

    bool load(std::span<const std::uint8_t> data) {
        impl_ = std::make_shared<detail::DemuxerImpl>();
        return impl_->open_span(data);
    }

    [[nodiscard]]
    static Demuxer open(const std::string& path) {
        return init(path);
    }

    [[nodiscard]]
    static Demuxer open(int fd, bool close_on_destroy = false) {
        return init(fd, close_on_destroy);
    }

    [[nodiscard]]
    static Demuxer open(std::span<const std::uint8_t> data) {
        return init(data);
    }

    [[nodiscard]]
    explicit operator bool() const noexcept {
        return ok();
    }

    [[nodiscard]]
    bool ok() const noexcept {
        return impl_ != nullptr && impl_->ok();
    }

    [[nodiscard]]
    Container container() const noexcept {
        return impl_ ? impl_->container() : Container::Unknown;
    }

    [[nodiscard]]
    std::span<const std::uint8_t> data() const noexcept {
        return impl_ ? impl_->source_.data() : std::span<const std::uint8_t>{};
    }

    [[nodiscard]]
    const FileSource* source() const noexcept {
        return impl_ ? &impl_->source_ : nullptr;
    }

    /* File-level tags; empty when the container has no metadata. */
    [[nodiscard]]
    const ContainerMetadata& metadata() const noexcept {
        static const ContainerMetadata empty{};
        return impl_ ? impl_->metadata() : empty;
    }

    [[nodiscard]]
    std::vector<Track> tracks() const {
        std::vector<Track> out;
        if (!impl_) {
            return out;
        }

        for (std::size_t i = 0; i < impl_->index_.tracks.size(); ++i) {
            out.emplace_back(impl_, i);
        }

        return out;
    }

    [[nodiscard]]
    Track track(std::size_t index) const {
        return Track(impl_, index);
    }

    [[nodiscard]]
    InterleavedReader interleaved() const {
        return InterleavedReader(impl_);
    }

    [[nodiscard]]
    Track select(std::uint32_t track_id) const {
        if (!impl_) {
            return Track();
        }

        for (std::size_t i = 0; i < impl_->index_.tracks.size(); ++i) {
            if (impl_->index_.tracks[i].id == track_id) {
                return Track(impl_, i);
            }
        }

        return Track();
    }

   private:
    std::shared_ptr<detail::DemuxerImpl> impl_{};
};

}  // namespace demux
}  // namespace bs
