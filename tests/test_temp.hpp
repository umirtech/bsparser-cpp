// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

/*
 * Portable temporary-file helpers for tests.
 *
 * Tests must not assume a sandbox-specific directory such as /tmp/opencode
 * exists: CI runners do not create it.  Use the system temp directory and
 * unique names so the tests also work under parallel ctest (-j).
 */
namespace bs_test {

[[nodiscard]]
inline std::string temp_dir() {
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    return ec ? std::string(".") : dir.string();
}

[[nodiscard]]
inline std::string temp_path(const std::string& prefix, const std::string& ext) {
    static std::size_t counter = 0;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string name = "bs_" + prefix + "_" + std::to_string(stamp) + "_" +
                             std::to_string(counter++) + ext;
    return (std::filesystem::path(temp_dir()) / name).string();
}

inline bool write_bytes(const std::string& path, const std::vector<std::uint8_t>& data) {
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return false;
        }
    }
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

inline void remove_file(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

}  // namespace bs_test
