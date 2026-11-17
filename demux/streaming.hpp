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
            switch (container_) {
                case Container::Mp4:
                    return build_mp4(source_, index_);

                case Container::Mkv:
                    return build_mkv(source_, index_);

                case Container::MpegTs:
                    return build_ts(source_, index_);

                case Container::Flv:
                    return build_flv(source_, index_);

                case Container::Avi:
                    return build_avi(source_, index_);

                case Container::Ivf:
                    return build_ivf(source_, index_);

                case Container::Ogg:
                    return build_ogg(source_, index_);

                case Container::Ps:
                    return build_ps(source_, index_);

                default:
                    return false;
            }
        } catch (...) {
            index_ = IndexBuildResult{};
            container_ = Container::Unknown;
            return false;
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
