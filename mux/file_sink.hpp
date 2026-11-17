// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace bs {
namespace mux {

/*
 * -----------------------------------------------------------
 * File sink
 * -----------------------------------------------------------
 *
 * Write-side mirror of demux::FileSource.  Bytes handed to write()
 * accumulate in an internal buffer.  An in-memory sink needs only
 * open_buffer(), after which data() exposes the accumulated result.
 *
 * For a path/fd sink the OS write is deferred until flush(): open()
 * and open_fd() merely record the destination, so a muxer can build
 * its bytes first.  flush() pushes only the bytes added since the
 * previous flush, making it safe to call repeatedly.  No implicit
 * flush happens on close() or destruction -- the caller decides when
 * buffered bytes reach the OS handle.
 */
class FileSink {
    enum class Destination : std::uint8_t { None, Buffer, Path, Fd };

   public:
    FileSink() = default;

    FileSink(const FileSink&) = delete;
    FileSink& operator=(const FileSink&) = delete;

    FileSink(FileSink&& other) noexcept {
        *this = std::move(other);
    }

    FileSink& operator=(FileSink&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        close();

        buffer_ = std::move(other.buffer_);
        path_ = std::move(other.path_);
        fd_ = other.fd_;
        close_fd_ = other.close_fd_;
        flushed_ = other.flushed_;
        kind_ = other.kind_;

        other.fd_ = -1;
        other.close_fd_ = false;
        other.flushed_ = 0;
        other.kind_ = Destination::None;

        return *this;
    }

    ~FileSink() {
        close();
    }

    /* Buffered: the file is created/truncated only when flush() runs. */
    bool open(const std::string& path) {
        close();

        if (path.empty()) {
            return false;
        }

        path_ = path;
        kind_ = Destination::Path;
        return true;
    }

    /* Buffered: writes go to fd on flush(), which never closes it. */
    bool open_fd(int fd, bool close_on_destroy = false) {
        close();

        if (fd < 0) {
            return false;
        }

        fd_ = fd;
        close_fd_ = close_on_destroy;
        kind_ = Destination::Fd;
        return true;
    }

    bool open_buffer() {
        close();
        kind_ = Destination::Buffer;
        return true;
    }

    /* Append bytes to the buffer; no-op with no destination or empty span. */
    void write(std::span<const std::uint8_t> bytes) {
        if (kind_ == Destination::None || bytes.empty()) {
            return;
        }

        buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    }

    bool flush() {
        if (kind_ == Destination::None || kind_ == Destination::Buffer) {
            return true;
        }

        // A path sink has no handle until the first flush; creating it here
        // keeps open() from truncating a file before any bytes are written.
        if (kind_ == Destination::Path && fd_ < 0) {
#if defined(_WIN32)
            fd_ = ::_open(
                path_.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY,
                _S_IREAD | _S_IWRITE
            );
#else
            fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
#endif
            if (fd_ < 0) {
                return false;
            }
            close_fd_ = true;
            flushed_ = 0;
        }

        if (fd_ < 0) {
            return false;
        }

        std::size_t off = flushed_;
        while (off < buffer_.size()) {
            const std::size_t remaining = buffer_.size() - off;
#if defined(_WIN32)
            /* _write takes unsigned int; cap each call so a >4 GiB buffer
             * cannot truncate the length argument. */
            const std::size_t chunk = std::min<std::size_t>(remaining, 0x7FFFFFFFu);
            const int n = ::_write(
                fd_, buffer_.data() + off, static_cast<unsigned int>(chunk)
            );
            if (n <= 0) {
                return false;
            }
#else
            const ssize_t n = ::write(fd_, buffer_.data() + off, remaining);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (n == 0) {
                return false;
            }
#endif
            off += static_cast<std::size_t>(n);
            flushed_ = off;
        }

        return true;
    }

    [[nodiscard]]
    bool ok() const noexcept {
        return kind_ != Destination::None;
    }

    [[nodiscard]]
    std::size_t size() const noexcept {
        return buffer_.size();
    }

    [[nodiscard]]
    std::span<const std::uint8_t> data() const noexcept {
        return std::span<const std::uint8_t>(buffer_.data(), buffer_.size());
    }

    [[nodiscard]]
    const std::string& path() const noexcept {
        return path_;
    }

    /* Releases resources; buffered bytes are NOT flushed automatically. */
    void close() noexcept {
        if (fd_ >= 0 && close_fd_) {
#if defined(_WIN32)
            ::_close(fd_);
#else
            ::close(fd_);
#endif
        }

        buffer_.clear();
        path_.clear();
        fd_ = -1;
        close_fd_ = false;
        flushed_ = 0;
        kind_ = Destination::None;
    }

   private:
    std::vector<std::uint8_t> buffer_{};
    std::string path_;
    int fd_ = -1;
    bool close_fd_ = false;
    std::size_t flushed_ = 0;
    Destination kind_ = Destination::None;
};

}  // namespace mux
}  // namespace bs
