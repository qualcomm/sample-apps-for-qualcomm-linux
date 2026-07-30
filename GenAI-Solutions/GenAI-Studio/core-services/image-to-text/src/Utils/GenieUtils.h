// =============================================================================
// Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// =============================================================================

#pragma once
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

inline std::string readTextFile(const fs::path& p) {
    std::ifstream f(p);
    if (!f.is_open()) throw std::runtime_error("Failed to open text file: " + p.string());
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

inline std::vector<uint8_t> readBinFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) throw std::runtime_error("Failed to open bin file: " + p.string());
    f.seekg(0, std::ios::end);
    size_t sz = (size_t)f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(sz);
    if (sz) f.read(reinterpret_cast<char*>(buf.data()), (std::streamsize)sz);
    return buf;
}

inline void writeBinFile(const fs::path& p, const std::vector<uint8_t>& data) {
    std::ofstream f(p, std::ios::binary);
    if (!f.is_open()) throw std::runtime_error("Failed to write file: " + p.string());
    if (!data.empty())
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
}

inline fs::path resolvePath(const fs::path& baseDir, const fs::path& maybeRel) {
    fs::path p = maybeRel.is_absolute() ? maybeRel : (baseDir / maybeRel);
    return fs::weakly_canonical(p);
}

inline void mustExistFile(const fs::path& p) {
    if (!fs::exists(p)) throw std::runtime_error("File not found: " + p.string());
    if (!fs::is_regular_file(p)) throw std::runtime_error("Not a regular file: " + p.string());
    if (fs::file_size(p) == 0) throw std::runtime_error("File is empty: " + p.string());
}