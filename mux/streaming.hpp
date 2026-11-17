// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "mux/file_sink.hpp"
#include "mux/ivf_muxer.hpp"
#include "mux/mkv_muxer.hpp"
#include "mux/mp4_muxer.hpp"
#include "mux/stream.hpp"
#include "mux/wav_muxer.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bs {
namespace mux {

namespace detail {

class MuxerImpl {
   public:
    MuxerImpl() = default;

    bool open_buffer(Container container) {
        if (!sink_.open_buffer()) {
            return false;
        }
        container_ = container;
        return true;
    }

    bool open_path(Container container, const std::string& path) {
        if (!sink_.open(path)) {
            return false;
        }
        container_ = container;
        return true;
    }

    bool open_fd(Container container, int fd, bool close_on_destroy = false) {
        if (!sink_.open_fd(fd, close_on_destroy)) {
            return false;
        }
        container_ = container;
        return true;
    }

    [[nodiscard]]
    bool ok() const noexcept {
        return sink_.ok() && container_ != Container::Unknown;
    }

    [[nodiscard]]
    bool finalized() const noexcept {
        return finalized_;
    }

    void set_metadata(const Metadata& metadata) {
        metadata_ = metadata;
        finalized_ = false;
    }

    [[nodiscard]]
    const Metadata& metadata() const noexcept {
        return metadata_;
    }

    /* Append a track description; samples get their own parallel bucket. */
    std::size_t add_track(const TrackConfig& config) {
        const std::size_t index = tracks_.size();
        tracks_.push_back(config);
        samples_.emplace_back();
        finalized_ = false;
        return index;
    }

    bool valid_track(std::size_t index) const noexcept {
        return index < tracks_.size();
    }

    /* Queue one sample.  The payload span is only borrowed. */
    void add_sample(std::size_t index, Sample sample) {
        if (finalized_ || index >= samples_.size()) {
            return;
        }

        if (sample.track_id == 0) {
            sample.track_id = tracks_[index].id ? tracks_[index].id
                                                  : static_cast<std::uint32_t>(index + 1);
        }
        if (sample.index == 0) {
            sample.index = samples_[index].size();
        }
        if (sample.framing == NalFramingMode::AnnexB && is_h26x(tracks_[index].codec)) {
            sample.framing = tracks_[index].framing;
        }

        samples_[index].push_back(sample);
    }

    /* Serialize all queued samples into the sink and flush it. */
    bool finalize() {
        if (finalized_ || !ok()) {
            return false;
        }

        if (tracks_.empty()) {
            return false;
        }

        for (std::size_t i = 0; i < tracks_.size(); ++i) {
            if (!container_supports_codec(container_, tracks_[i].codec)) {
                return false;
            }
        }

        /*
         * The encoder tag is owned by the muxer.  Drop any caller-supplied
         * tag that tries to impersonate it before invoking the writers.
         */
        Metadata final_metadata = metadata_;
        final_metadata.custom.erase(
            std::remove_if(
                final_metadata.custom.begin(), final_metadata.custom.end(),
                [](const auto& kv) { return is_reserved_encoder_key(kv.first); }
            ),
            final_metadata.custom.end()
        );

        std::vector<TrackConfig> final_tracks = tracks_;
        for (auto& cfg : final_tracks) {
            cfg.metadata.custom.erase(
                std::remove_if(
                    cfg.metadata.custom.begin(), cfg.metadata.custom.end(),
                    [](const auto& kv) { return is_reserved_encoder_key(kv.first); }
                ),
                cfg.metadata.custom.end()
            );
        }

        bool written = false;
        switch (container_) {
            case Container::Mp4:
                written = write_mp4(sink_, final_metadata, final_tracks, samples_);
                break;
            case Container::Mkv:
                written = write_mkv(sink_, final_metadata, final_tracks, samples_);
                break;
            case Container::Ivf:
                written = write_ivf(sink_, final_tracks, samples_);
                break;
            case Container::Wav:
                written = write_wav(sink_, final_tracks, samples_);
                break;
            default:
                return false;
        }

        if (!written || !sink_.flush()) {
            return false;
        }

        finalized_ = true;
        return true;
    }

    [[nodiscard]]
    FileSink& sink() noexcept {
        return sink_;
    }

    [[nodiscard]]
    const FileSink& sink() const noexcept {
        return sink_;
    }

    [[nodiscard]]
    Container container() const noexcept {
        return container_;
    }

    [[nodiscard]]
    const std::vector<TrackConfig>& tracks() const noexcept {
        return tracks_;
    }

    [[nodiscard]]
    const std::vector<std::vector<Sample>>& samples() const noexcept {
        return samples_;
    }

   private:
    FileSink sink_{};
    Container container_ = Container::Unknown;
    std::vector<TrackConfig> tracks_{};
    std::vector<std::vector<Sample>> samples_{};
    Metadata metadata_{};
    bool finalized_ = false;
};

}  // namespace detail

/*
 * -----------------------------------------------------------
 * Track handle
 * -----------------------------------------------------------
 *
 * A lightweight, copyable handle into a MuxerImpl.  It owns no
 * payload; addSample() only borrows the caller's span until
 * Muxer::finalize() serializes it.
 */
class Track {
   public:
    Track() = default;

    Track(std::shared_ptr<detail::MuxerImpl> impl, std::size_t index)
        : impl_(std::move(impl)), index_(index) {}

    [[nodiscard]]
    bool valid() const noexcept {
        return impl_ != nullptr && impl_->valid_track(index_);
    }

    [[nodiscard]]
    const TrackConfig& config() const noexcept {
        static const TrackConfig empty{};
        return valid() ? impl_->tracks()[index_] : empty;
    }

    [[nodiscard]]
    const TrackConfig& view() const noexcept {
        return config();
    }

    void add_sample(const Sample& sample) {
        if (valid()) {
            impl_->add_sample(index_, sample);
        }
    }

    void addSample(const Sample& sample) {
        add_sample(sample);
    }

    void add_sample(
        std::span<const std::uint8_t> data, std::int64_t pts = 0, std::int64_t dts = 0,
        std::int64_t duration = 0, bool is_key = true,
        NalFramingMode framing = NalFramingMode::AnnexB
    ) {
        Sample s;
        s.data = data;
        s.pts = pts;
        s.dts = dts;
        s.duration = duration;
        s.is_key = is_key;
        s.framing = framing;
        add_sample(s);
    }

    void addSample(
        std::span<const std::uint8_t> data, std::int64_t pts = 0, std::int64_t dts = 0,
        std::int64_t duration = 0, bool is_key = true,
        NalFramingMode framing = NalFramingMode::AnnexB
    ) {
        add_sample(data, pts, dts, duration, is_key, framing);
    }

    [[nodiscard]]
    std::size_t sampleCount() const noexcept {
        return valid() ? impl_->samples()[index_].size() : 0;
    }

    [[nodiscard]]
    std::size_t sample_count() const noexcept {
        return sampleCount();
    }

   private:
    std::shared_ptr<detail::MuxerImpl> impl_{};
    std::size_t index_ = 0;
};

/*
 * -----------------------------------------------------------
 * Universal muxer
 * -----------------------------------------------------------
 */
class Muxer {
   public:
    Muxer() = default;

    /* In-memory destination; result is available through data() after finalize(). */
    [[nodiscard]]
    static Muxer create(Container container) {
        Muxer m;
        m.impl_ = std::make_shared<detail::MuxerImpl>();
        m.impl_->open_buffer(container);
        return m;
    }

    [[nodiscard]]
    static Muxer create(Container container, const std::string& path) {
        Muxer m;
        m.impl_ = std::make_shared<detail::MuxerImpl>();
        m.impl_->open_path(container, path);
        return m;
    }

    [[nodiscard]]
    static Muxer create(Container container, int fd, bool close_on_destroy = false) {
        Muxer m;
        m.impl_ = std::make_shared<detail::MuxerImpl>();
        m.impl_->open_fd(container, fd, close_on_destroy);
        return m;
    }

    /* Container inferred from the file extension. */
    [[nodiscard]]
    static Muxer open(const std::string& path) {
        return create(container_from_extension(path), path);
    }

    [[nodiscard]]
    static Muxer open(int fd, Container container, bool close_on_destroy = false) {
        return create(container, fd, close_on_destroy);
    }

    /* Non-static form for callers that reuse an object. */
    bool load(Container container) {
        impl_ = std::make_shared<detail::MuxerImpl>();
        return impl_->open_buffer(container);
    }

    bool load(Container container, const std::string& path) {
        impl_ = std::make_shared<detail::MuxerImpl>();
        return impl_->open_path(container, path);
    }

    bool load(Container container, int fd, bool close_on_destroy = false) {
        impl_ = std::make_shared<detail::MuxerImpl>();
        return impl_->open_fd(container, fd, close_on_destroy);
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

    /* File-level tags; written into MP4 udta/meta/ilst or MKV Info/Tags. */
    void set_metadata(const Metadata& metadata) {
        if (impl_) {
            impl_->set_metadata(metadata);
        }
    }

    [[nodiscard]]
    const Metadata& metadata() const noexcept {
        static const Metadata empty{};
        return impl_ ? impl_->metadata() : empty;
    }

    /* Add a track and return its handle. */
    Track add_track(const TrackConfig& config) {
        if (!impl_) {
            return Track();
        }
        return Track(impl_, impl_->add_track(config));
    }

    Track addTrack(const TrackConfig& config) {
        return add_track(config);
    }

    [[nodiscard]]
    std::vector<Track> tracks() const {
        std::vector<Track> out;
        if (!impl_) {
            return out;
        }
        for (std::size_t i = 0; i < impl_->tracks().size(); ++i) {
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
        for (std::size_t i = 0; i < impl_->tracks().size(); ++i) {
            if (impl_->tracks()[i].id == track_id) {
                return Track(impl_, i);
            }
        }
        return Track();
    }

    /* Serialize everything into the sink.  Returns false if already finalized. */
    bool finalize() {
        return impl_ != nullptr && impl_->finalize();
    }

    [[nodiscard]]
    bool finalized() const noexcept {
        return impl_ != nullptr && impl_->finalized();
    }

    [[nodiscard]]
    bool done() const noexcept {
        return finalized();
    }

    /* Valid for an in-memory sink after finalize(). */
    [[nodiscard]]
    std::span<const std::uint8_t> data() const noexcept {
        return impl_ ? impl_->sink().data() : std::span<const std::uint8_t>{};
    }

    [[nodiscard]]
    std::size_t size() const noexcept {
        return impl_ ? impl_->sink().size() : 0;
    }

    [[nodiscard]]
    const FileSink* sink() const noexcept {
        return impl_ ? &impl_->sink() : nullptr;
    }

   private:
    std::shared_ptr<detail::MuxerImpl> impl_{};
};

}  // namespace mux
}  // namespace bs
