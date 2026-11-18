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

    void set_options(const MuxerOptions& options) {
        options_ = options;
    }

    [[nodiscard]]
    const MuxerOptions& options() const noexcept {
        return options_;
    }

    void report_progress() {
        if (options_.on_progress != nullptr) {
            options_.on_progress(
                options_.ctx,
                static_cast<std::uint64_t>(sink_.size()),
                static_cast<std::uint64_t>(options_.expected_size)
            );
        }
    }

    void report_error(const char* message) {
        if (options_.on_error != nullptr) {
            options_.on_error(options_.ctx, message);
        }
    }

    bool open_path(Container container, const std::string& path,
                   std::size_t expected_size = 0, bool try_mmap = true) {
        const std::size_t size = expected_size != 0u ? expected_size : options_.expected_size;
        const bool mmap = try_mmap && options_.try_mmap;
        if (!sink_.open(path, size, mmap)) {
            return false;
        }
        container_ = container;
        return true;
    }

    bool open_fd(Container container, int fd, bool close_on_destroy = false,
                 std::size_t expected_size = 0, bool try_mmap = true) {
        const std::size_t size = expected_size != 0u ? expected_size : options_.expected_size;
        const bool mmap = try_mmap && options_.try_mmap;
        if (!sink_.open_fd(fd, close_on_destroy, size, mmap)) {
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
        stream_records_.resize(tracks_.size());
        pending_param_sets_.resize(tracks_.size());
        pending_mkv_param_.resize(tracks_.size());
        finalized_ = false;
        return index;
    }

    bool valid_track(std::size_t index) const noexcept {
        return index < tracks_.size();
    }

    [[nodiscard]]
    std::size_t sample_count(std::size_t index) const noexcept {
        return index < stream_records_.size() ? stream_records_[index].size() : 0u;
    }

    /* Start streaming output (path/fd sinks only). */
    bool start_stream() {
        if (!sink_.streaming() || stream_started_) {
            return false;
        }
        switch (container_) {
            case Container::Mp4: {
                stream_records_.assign(tracks_.size(), {});
                const std::size_t reserve = options_.fast_start ? 1u << 20 : 0u;
                mdat_header_offset_ = detail::start_mp4_stream(
                    sink_, reserve, reserve_offset_
                );
                break;
            }
            case Container::Mkv:
                segment_data_offset_ = detail::start_mkv_stream(
                    sink_, metadata_, tracks_
                );
                break;
            case Container::Ivf:
                if (tracks_.empty()) {
                    return false;
                }
                detail::start_ivf_stream(sink_, tracks_.front());
                break;
            case Container::Wav:
                if (tracks_.empty() ||
                    !detail::start_wav_stream(sink_, tracks_.front(), block_align_)) {
                    return false;
                }
                break;
            default:
                return false;
        }
        stream_started_ = true;
        return true;
    }

    /* Each sample is framed and written immediately; only tiny records are
     * retained.  There is no in-memory/buffered accumulation path. */
    void add_sample(std::size_t index, Sample sample) {
        if (finalized_ || index >= tracks_.size()) {
            return;
        }

        if (sample.track_id == 0) {
            sample.track_id = tracks_[index].id ? tracks_[index].id
                                                  : static_cast<std::uint32_t>(index + 1);
        }
        if (sample.index == 0) {
            sample.index = sample_count(index);
        }
        if (sample.framing == NalFramingMode::AnnexB && is_h26x(tracks_[index].codec)) {
            sample.framing = tracks_[index].framing;
        }

        if (!stream_started_) {
            /* MKV writes TrackEntry headers up front, before any samples are
             * seen.  Derive H.26x CodecPrivate from the first keyframe now. */
            if (container_ == Container::Mkv && tracks_[index].codec_config.empty() &&
                is_h26x(tracks_[index].codec) && sample.is_key && !sample.data.empty()) {
                std::vector<Sample> one{sample};
                const auto cfg = detail::mkv_derive_codec_config(tracks_[index], one);
                if (!cfg.empty()) {
                    tracks_[index].codec_config = cfg;
                }
            }
            start_stream();
        }

        if (sample.data.empty()) {
            return;
        }
        switch (container_) {
            case Container::Mp4: {
                if (index >= stream_records_.size()) {
                    return;
                }
                Mp4Bytes framed = detail::frame_mp4_sample(
                    tracks_[index].codec, sample, tracks_[index]
                );
                /* A leading parameter-set pseudo-sample from the demuxer is
                 * not an access unit: fold it into the next sample. */
                if (detail::is_param_set_only_sample(tracks_[index].codec, sample.data)) {
                    if (pending_param_sets_.size() <= index) {
                        pending_param_sets_.resize(index + 1u);
                    }
                    auto& pending = pending_param_sets_[index];
                    pending.insert(pending.end(), framed.begin(), framed.end());
                    return;
                }
                if (index < pending_param_sets_.size() &&
                    !pending_param_sets_[index].empty()) {
                    auto& pending = pending_param_sets_[index];
                    framed.insert(framed.begin(), pending.begin(), pending.end());
                    pending.clear();
                }
                if (framed.size() > 0xFFFFFFFFull) {
                    return;
                }
                SampleRecord rec;
                rec.offset = sink_.size();
                rec.size = framed.size();
                rec.pts = sample.pts;
                rec.dts = sample.dts;
                rec.duration = sample.duration;
                rec.is_key = sample.is_key;
                sink_.write(framed);
                stream_records_[index].push_back(rec);
                if (chunks_.empty() || chunks_.back().track != index) {
                    ChunkRecord c;
                    c.track = index;
                    c.first_sample = stream_records_[index].size() - 1u;
                    c.sample_count = 1u;
                    c.offset = rec.offset;
                    chunks_.push_back(c);
                } else {
                    ++chunks_.back().sample_count;
                }
                break;
            }
            case Container::Mkv: {
                /* A parameter-set-only sample was already written into this
                 * track's CodecPrivate at stream start; skip the duplicate. */
                if (detail::is_param_set_only_sample(tracks_[index].codec, sample.data) &&
                    !tracks_[index].codec_config.empty()) {
                    return;
                }
                Sample out = sample;
                if (detail::is_param_set_only_sample(tracks_[index].codec, sample.data)) {
                    if (pending_mkv_param_.size() <= index) {
                        pending_mkv_param_.resize(index + 1u);
                    }
                    const Mp4Bytes framed = detail::frame_mp4_sample(
                        tracks_[index].codec, sample, tracks_[index]
                    );
                    auto& p = pending_mkv_param_[index];
                    p.insert(p.end(), framed.begin(), framed.end());
                    return;
                }
                if (index < pending_mkv_param_.size() &&
                    !pending_mkv_param_[index].empty()) {
                    const Mp4Bytes framed = detail::frame_mp4_sample(
                        tracks_[index].codec, sample, tracks_[index]
                    );
                    auto& p = pending_mkv_param_[index];
                    p.insert(p.end(), framed.begin(), framed.end());
                    out.data = p;
                    p.clear();
                }
                detail::append_mkv_stream_sample(
                    sink_, tracks_[index], static_cast<std::uint32_t>(index), out
                );
                if (out.is_key) {
                    stream_keyframes_.push_back(out);
                }
                break;
            }
            case Container::Ivf:
                detail::append_ivf_stream_sample(sink_, sample);
                ++stream_frame_count_;
                break;
            case Container::Wav:
                if (!detail::append_wav_stream_sample(sink_, sample, block_align_)) {
                    return;
                }
                stream_data_bytes_ += sample.data.size();
                break;
            default:
                return;
        }
        report_progress();
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

        // A stream that never received a sample has no valid container to
        // finish.  Start it now so headers at least exist, then finish.
        if (!stream_started_) {
            start_stream();
        }
        bool written = false;
        switch (container_) {
            case Container::Mp4:
                written = detail::finish_mp4_stream(
                    sink_, mdat_header_offset_, final_metadata,
                    final_tracks, stream_records_, chunks_,
                    options_.fast_start ? 1u << 20 : 0u, reserve_offset_
                );
                break;
            case Container::Mkv:
                written = detail::finish_mkv_stream(
                    sink_, final_metadata, final_tracks,
                    segment_data_offset_, stream_keyframes_
                );
                break;
            case Container::Ivf:
                written = detail::finish_ivf_stream(sink_, stream_frame_count_);
                break;
            case Container::Wav:
                detail::append_wav_stream_pad(sink_, stream_data_bytes_);
                written = detail::finish_wav_stream(sink_, stream_data_bytes_);
                break;
            default:
                return false;
        }

        if (!written || !sink_.flush() || !sink_.finish()) {
            report_error("mux finalize failed");
            return false;
        }

        report_progress();
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

   private:
    FileSink sink_{};
    Container container_ = Container::Unknown;
    std::vector<TrackConfig> tracks_{};
    std::vector<std::vector<SampleRecord>> stream_records_{};
    std::vector<Mp4Bytes> pending_param_sets_{};
    std::vector<ChunkRecord> chunks_{};
    std::vector<Sample> stream_keyframes_{};
    std::vector<std::vector<std::uint8_t>> pending_mkv_param_{};
    std::uint32_t stream_frame_count_ = 0;
    std::uint64_t stream_data_bytes_ = 0;
    std::uint32_t block_align_ = 0;
    std::uint64_t segment_data_offset_ = 0;
    std::size_t mdat_header_offset_ = 0;
    std::size_t reserve_offset_ = 0;
    bool stream_started_ = false;
    Metadata metadata_{};
    MuxerOptions options_{};
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
        return valid() ? impl_->sample_count(index_) : 0;
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

    /* Apply muxer options (expected size, callbacks, fast-start, backend). */
    Muxer& with_options(const MuxerOptions& options) {
        if (impl_) {
            impl_->set_options(options);
        }
        return *this;
    }

    [[nodiscard]]
    const MuxerOptions& options() const noexcept {
        static const MuxerOptions empty{};
        return impl_ ? impl_->options() : empty;
    }

    [[nodiscard]]
    static Muxer create(Container container, const std::string& path,
                        std::size_t expected_size = 0, bool try_mmap = true) {
        Muxer m;
        m.impl_ = std::make_shared<detail::MuxerImpl>();
        m.impl_->open_path(container, path, expected_size, try_mmap);
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
