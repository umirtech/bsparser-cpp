// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
// windows.h otherwise defines min()/max() macros that break std::min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace bs {
namespace demux {

/*
 * -----------------------------------------------------------
 * File source
 * -----------------------------------------------------------
 *
 * Owns the bytes a demuxer reads from.  When the platform has mmap
 * the file is mapped and every SampleView can be a true zero-copy
 * span into the mapped file.  Otherwise the file is read once into a
 * vector; that fallback still keeps the demuxer from loading a second
 * full elementary-stream copy, but SampleView::mmap_backed is false
 * because those spans live in a reusable buffer.
 */
class FileSource {
   public:
    FileSource() = default;

    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;

    FileSource(FileSource&& other) noexcept {
        *this = std::move(other);
    }

    FileSource& operator=(FileSource&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        close();

        data_ = other.data_;
        size_ = other.size_;
        mapped_ = other.mapped_;
        fd_ = other.fd_;
        close_fd_ = other.close_fd_;
        path_ = std::move(other.path_);
        buffer_ = std::move(other.buffer_);

        other.data_ = {};
        other.size_ = 0;
        other.mapped_ = false;
        other.fd_ = -1;
        other.close_fd_ = false;

        return *this;
    }

    ~FileSource() {
        close();
    }

    bool open(const std::string& path) {
        close();

#if defined(_WIN32)
        HANDLE file = CreateFileA(
            path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr
        );

        if (file == INVALID_HANDLE_VALUE) {
            return false;
        }

        LARGE_INTEGER n;
        if (!GetFileSizeEx(file, &n)) {
            CloseHandle(file);
            return false;
        }

        size_ = static_cast<std::size_t>(n.QuadPart);

        if (size_ == 0) {
            CloseHandle(file);
            return true;
        }

        HANDLE mapping = CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping == nullptr) {
            CloseHandle(file);
            return false;
        }

        void* p = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, size_);
        CloseHandle(mapping);
        CloseHandle(file);

        if (p == nullptr) {
            size_ = 0;
            return false;
        }

        data_ = std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(p), size_);
        mapped_ = true;
        path_ = path;
        return true;
#else
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            return false;
        }

        struct stat st {};
        if (fstat(fd, &st) != 0) {
            ::close(fd);
            return false;
        }

        size_ = static_cast<std::size_t>(st.st_size);

        if (size_ == 0) {
            fd_ = fd;
            close_fd_ = true;
            path_ = path;
            return true;
        }

        void* p = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);

        if (p != MAP_FAILED) {
            data_ = std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(p), size_);
            mapped_ = true;
            fd_ = fd;
            close_fd_ = true;
            path_ = path;
            return true;
        }

        // mmap unavailable: read the file once into a reusable buffer.
        buffer_.resize(size_);
        std::size_t got = 0;
        while (got < size_) {
            const ssize_t r = ::pread(fd, buffer_.data() + got, size_ - got, static_cast<off_t>(got));
            if (r <= 0) {
                ::close(fd);
                buffer_.clear();
                size_ = 0;
                return false;
            }
            got += static_cast<std::size_t>(r);
        }

        data_ = buffer_;
        mapped_ = false;
        fd_ = fd;
        close_fd_ = true;
        path_ = path;
        return true;
#endif
    }

    bool open_fd(int fd, bool close_on_destroy = false) {
        close();

        if (fd < 0) {
            return false;
        }

#if defined(_WIN32)
        (void)close_on_destroy;
        return false;
#else
        struct stat st {};
        if (fstat(fd, &st) != 0) {
            return false;
        }

        size_ = static_cast<std::size_t>(st.st_size);

        if (size_ == 0) {
            fd_ = fd;
            close_fd_ = close_on_destroy;
            return true;
        }

        void* p = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);

        if (p != MAP_FAILED) {
            data_ = std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(p), size_);
            mapped_ = true;
            fd_ = fd;
            close_fd_ = close_on_destroy;
            return true;
        }

        buffer_.resize(size_);
        std::size_t got = 0;
        while (got < size_) {
            const ssize_t r = ::pread(fd, buffer_.data() + got, size_ - got, static_cast<off_t>(got));
            if (r <= 0) {
                buffer_.clear();
                size_ = 0;
                return false;
            }
            got += static_cast<std::size_t>(r);
        }

        data_ = buffer_;
        mapped_ = false;
        fd_ = fd;
        close_fd_ = close_on_destroy;
        return true;
#endif
    }

    bool open_span(std::span<const std::uint8_t> data) {
        close();

        data_ = data;
        size_ = data.size();
        mapped_ = false;
        return true;
    }

    [[nodiscard]]
    bool ok() const noexcept {
        return size_ > 0 || !data_.empty();
    }

    [[nodiscard]]
    std::span<const std::uint8_t> data() const noexcept {
        return data_;
    }

    [[nodiscard]]
    std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]]
    bool mmap_backed() const noexcept {
        return mapped_;
    }

    [[nodiscard]]
    const std::string& path() const noexcept {
        return path_;
    }

    void close() noexcept {
        if (!data_.empty()) {
#if defined(_WIN32)
            if (mapped_) {
                UnmapViewOfFile(data_.data());
            }
#else
            if (mapped_) {
                munmap(const_cast<void*>(static_cast<const void*>(data_.data())), size_);
            }
#endif
        }

        if (fd_ >= 0 && close_fd_) {
#if defined(_WIN32)
            CloseHandle(reinterpret_cast<HANDLE>(static_cast<intptr_t>(fd_)));
#else
            ::close(fd_);
#endif
        }

        data_ = {};
        buffer_.clear();
        size_ = 0;
        mapped_ = false;
        fd_ = -1;
        close_fd_ = false;
        path_.clear();
    }

   private:
    std::span<const std::uint8_t> data_{};
    std::vector<std::uint8_t> buffer_{};
    std::size_t size_ = 0;
    bool mapped_ = false;
    int fd_ = -1;
    bool close_fd_ = false;
    std::string path_;
};

}  // namespace demux
}  // namespace bs
