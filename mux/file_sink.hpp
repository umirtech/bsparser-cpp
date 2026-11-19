// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
// windows.h otherwise defines min()/max() macros that break std::min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
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
 * Write-side mirror of demux::FileSource.  Streaming backends only:
 *
 *   Mapped    path/fd where the OS can map a writable file.  write()
 *             memcpy()s straight into shared pages; the kernel handles
 *             paging and writeback.  patch() edits already-written
 *             bytes in place (header fixups), and grow uses ftruncate +
 *             mremap so previously written pages are never copied.
 *             Mirrors demux::FileSource's mmap-backed zero-copy path.
 *
 *   Buffered  fallback when mapping is unavailable (non-regular fd,
 *             mapping failure, or forced by try_mmap=false).  A small
 *             userspace chunk buffer is written through with write();
 *             patch() uses positional writes.  Memory stays bounded.
 *
 * For path/fd sinks the file is created when the sink is opened, because
 * streaming writes begin at add_sample() time rather than at finalize().
 */
class FileSink {
    enum class Backend : std::uint8_t { None, Buffered, Mapped };

   public:
    static constexpr std::size_t kDefaultChunk = 4u << 20; // 4 MiB
    // Buffered-fallback flush size.  256 KiB keeps the userspace buffer
    // bounded while cutting write() calls by 4x versus 64 KiB.
    static constexpr std::size_t kBufferedFlush = 256u << 10;

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
        total_ = other.total_;
        chunk_ = other.chunk_;
        map_ = other.map_;
        map_size_ = other.map_size_;
        map_offset_ = other.map_offset_;
        finished_ = other.finished_;
        backend_ = other.backend_;

        other.fd_ = -1;
        other.close_fd_ = false;
        other.total_ = 0;
        other.map_ = nullptr;
        other.map_size_ = 0;
        other.map_offset_ = 0;
        other.finished_ = false;
        other.backend_ = Backend::None;
        return *this;
    }

    ~FileSink() {
        close();
    }

    bool open(const std::string& path, std::size_t expected_size = 0, bool try_mmap = true) {
        close();
        if (path.empty()) {
            return false;
        }
#if defined(_WIN32)
        (void)try_mmap; // Windows uses the buffered backend only
#endif
        path_ = path;
        chunk_ = expected_size != 0u ? align_up(std::max(expected_size, std::size_t{4096}))
                                     : kDefaultChunk;

#if defined(_WIN32)
        // MSVC deprecates the POSIX spelling; _open is the conformant name.
        // O_BINARY is essential: without it _write translates 0x0A to CRLF
        // and silently corrupts every container written on Windows.
        fd_ = ::_open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_BINARY, 0644);
#else
        fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
#endif
        if (fd_ < 0) {
            return false;
        }
        close_fd_ = true;

#if !defined(_WIN32)
        if (try_mmap && map_file(chunk_)) {
            backend_ = Backend::Mapped;
            return true;
        }
#endif
        backend_ = Backend::Buffered;
        buffer_.clear();
        buffer_.reserve(std::min<std::size_t>(chunk_, kBufferedFlush));
        return true;
    }

    bool open_fd(int fd, bool close_on_destroy = false, std::size_t expected_size = 0,
                 bool try_mmap = true) {
        close();
        if (fd < 0) {
            return false;
        }
#if defined(_WIN32)
        (void)try_mmap; // Windows uses the buffered backend only
#endif
        fd_ = fd;
        close_fd_ = close_on_destroy;
        chunk_ = expected_size != 0u ? align_up(std::max(expected_size, std::size_t{4096}))
                                     : kDefaultChunk;

#if !defined(_WIN32)
        if (try_mmap && map_file(chunk_)) {
            backend_ = Backend::Mapped;
            return true;
        }
#endif
        backend_ = Backend::Buffered;
        buffer_.clear();
        buffer_.reserve(std::min<std::size_t>(chunk_, kBufferedFlush));
        return true;
    }

    /* Append bytes.  The bytes reach the OS here. */
    void write(std::span<const std::uint8_t> bytes) {
        if (bytes.empty()) {
            return;
        }
        switch (backend_) {
            case Backend::Mapped:
                ensure_capacity(bytes.size());
                if (map_ != nullptr && map_offset_ + bytes.size() <= map_size_) {
                    std::memcpy(map_ + map_offset_, bytes.data(), bytes.size());
                    map_offset_ += bytes.size();
                    total_ += bytes.size();
                }
                break;
            case Backend::Buffered: {
                buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
                total_ += bytes.size();
                const std::size_t limit = std::min<std::size_t>(chunk_, kBufferedFlush);
                if (buffer_.size() >= limit) {
                    flush_buffered();
                }
                break;
            }
            default:
                break;
        }
    }

    /* Edit bytes already written. Used for MP4/IVF/WAV header fixups. */
    bool patch(std::size_t offset, std::span<const std::uint8_t> bytes) {
        if (bytes.empty()) {
            return true;
        }
        switch (backend_) {
            case Backend::Mapped:
                if (offset > map_size_ || bytes.size() > map_size_ - offset) {
                    return false;
                }
                std::memcpy(map_ + offset, bytes.data(), bytes.size());
                return true;
            case Backend::Buffered: {
                if (!flush_buffered()) {
                    return false;
                }
                return positional_write(offset, bytes);
            }
            default:
                return false;
        }
    }

    /* Push any userspace-buffered bytes to the OS. */
    bool flush() {
        if (backend_ == Backend::Buffered) {
            return flush_buffered();
        }
        return true;
    }

    /* Finalize the file: truncate to the exact size and unmap. */
    bool finish() {
        if (finished_) {
            return true;
        }
        bool ok = true;
        if (backend_ == Backend::Mapped) {
#if !defined(_WIN32)
            if (::ftruncate(fd_, static_cast<off_t>(total_)) != 0) {
                ok = false;
            }
            if (map_ != nullptr) {
                // Sync only the bytes actually written; the mapping is
                // usually larger than the finished file.
                const std::size_t sync_size =
                        total_ < map_size_ ? total_ : map_size_;
                if (sync_size != 0u) {
                    ::msync(map_, sync_size, MS_ASYNC);
                }
                ::munmap(map_, map_size_);
            }
#else
            if (map_ != nullptr) {
                (void)::UnmapViewOfFile(map_);
            }
#endif
            map_ = nullptr;
            map_size_ = 0;
        } else if (backend_ == Backend::Buffered) {
            ok = flush_buffered();
        }

        // A finalized path sink is done writing.  Release the OS handle so
        // the file can be reopened or removed on Windows (which forbids
        // deleting a file that still has a writer handle).  Caller-owned fd
        // sinks are left alone.
        if (ok && close_fd_ && fd_ >= 0) {
#if defined(_WIN32)
            (void)::_close(fd_);
#else
            (void)::close(fd_);
#endif
            fd_ = -1;
            close_fd_ = false;
        }
        finished_ = true;
        return ok;
    }

    [[nodiscard]] bool ok() const noexcept {
        return backend_ != Backend::None;
    }

    [[nodiscard]] bool mapped() const noexcept {
        return backend_ == Backend::Mapped;
    }

    [[nodiscard]] bool streaming() const noexcept {
        return backend_ == Backend::Mapped || backend_ == Backend::Buffered;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return total_;
    }

    /* A read-only view of the mapped output, valid until finish(). */
    [[nodiscard]] std::span<const std::uint8_t> data() const noexcept {
        if (backend_ == Backend::Mapped && map_ != nullptr) {
            return {map_, map_offset_};
        }
        return {};
    }

    [[nodiscard]] const std::string& path() const noexcept {
        return path_;
    }

    void close() noexcept {
        if (backend_ == Backend::Mapped && map_ != nullptr) {
#if !defined(_WIN32)
            ::munmap(map_, map_size_);
#else
            (void)::UnmapViewOfFile(map_);
#endif
        }
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
        total_ = 0;
        map_ = nullptr;
        map_size_ = 0;
        map_offset_ = 0;
        finished_ = false;
        backend_ = Backend::None;
    }

   private:
    static std::size_t align_up(std::size_t v) noexcept {
        constexpr std::size_t page = 4096u;
        return (v + page - 1u) & ~(page - 1u);
    }

#if !defined(_WIN32)
    bool map_file(std::size_t size) {
        if (fd_ < 0 || ::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
            return false;
        }
        void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (p == MAP_FAILED) {
            return false;
        }
        map_ = static_cast<std::uint8_t*>(p);
        map_size_ = size;
        map_offset_ = 0;
        return true;
    }

    bool grow_map(std::size_t need) {
        std::size_t want = map_size_ != 0u ? map_size_ : chunk_;
        while (want < need) {
            want *= 2u;
        }
        if (::ftruncate(fd_, static_cast<off_t>(want)) != 0) {
            return false;
        }
#if defined(__linux__) && defined(MREMAP_MAYMOVE)
        void* p = ::mremap(map_, map_size_, want, MREMAP_MAYMOVE);
        if (p != MAP_FAILED) {
            map_ = static_cast<std::uint8_t*>(p);
            map_size_ = want;
            return true;
        }
#endif
        // Portable fallback: sync, unmap, remap (file contents are intact).
        if (total_ != 0u) {
            ::msync(map_, total_ < map_size_ ? total_ : map_size_, MS_SYNC);
        }
        ::munmap(map_, map_size_);
        void* q = ::mmap(nullptr, want, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (q == MAP_FAILED) {
            map_ = nullptr;
            map_size_ = 0;
            return false;
        }
        map_ = static_cast<std::uint8_t*>(q);
        map_size_ = want;
        return true;
    }
#endif

    void ensure_capacity(std::size_t extra) {
#if defined(_WIN32)
        (void)extra; // no growable mapping on Windows
#else
        if (backend_ == Backend::Mapped && map_offset_ + extra > map_size_) {
            (void)grow_map(map_offset_ + extra);
        }
#endif
    }

    bool flush_buffered() {
        if (buffer_.empty()) {
            return true;
        }
        std::size_t off = 0;
        while (off < buffer_.size()) {
#if defined(_WIN32)
            const std::size_t n = std::min<std::size_t>(buffer_.size() - off, 0x7FFFFFFFu);
            const int written = ::_write(fd_, buffer_.data() + off, static_cast<unsigned int>(n));
            if (written <= 0) {
                return false;
            }
            off += static_cast<std::size_t>(written);
#else
            const ssize_t n = ::write(fd_, buffer_.data() + off, buffer_.size() - off);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (n == 0) {
                return false;
            }
            off += static_cast<std::size_t>(n);
#endif
        }
        buffer_.clear();
        return true;
    }

    bool positional_write(std::size_t offset, std::span<const std::uint8_t> bytes) {
        std::size_t off = 0;
#if defined(_WIN32)
        while (off < bytes.size()) {
            const DWORD pos = static_cast<DWORD>(offset + off);
            if (::SetFilePointer(reinterpret_cast<HANDLE>(_get_osfhandle(fd_)),
                                 static_cast<LONG>(pos), nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
                return false;
            }
            const std::size_t n = std::min<std::size_t>(bytes.size() - off, 0x7FFFFFFFu);
            const int written = ::_write(fd_, bytes.data() + off, static_cast<unsigned int>(n));
            if (written <= 0) {
                return false;
            }
            off += static_cast<std::size_t>(written);
        }
        // SetFilePointer + _write leave the file position at the patched
        // offset, whereas POSIX pwrite() leaves it untouched.  Restore it to
        // EOF so later appends (e.g. moov after patching mdat) do not
        // overwrite data that was already written.
        if (::_lseek(fd_, 0, SEEK_END) == -1L) {
            return false;
        }
#else
        while (off < bytes.size()) {
            const ssize_t n = ::pwrite(fd_, bytes.data() + off, bytes.size() - off,
                                       static_cast<off_t>(offset + off));
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (n == 0) {
                return false;
            }
            off += static_cast<std::size_t>(n);
        }
#endif
        return true;
    }

    std::vector<std::uint8_t> buffer_{};
    std::string path_;
    int fd_ = -1;
    bool close_fd_ = false;
    std::size_t total_ = 0;
    std::size_t chunk_ = kDefaultChunk;
    std::uint8_t* map_ = nullptr;
    std::size_t map_size_ = 0;
    std::size_t map_offset_ = 0;
    bool finished_ = false;
    Backend backend_ = Backend::None;
};

}  // namespace mux
}  // namespace bs
