// =============================================================================
// Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// =============================================================================
// ImagePreprocessor.h
// Converts JPEG/PNG/BMP images to pixel_values.raw for Qwen2.5-VL
//
// Output format: float32, CHW layout, shape [3, 448, 448]
// Normalization: (pixel/255 - mean) / std
//   mean = [0.48145466, 0.4578275,  0.40821073]
//   std  = [0.26862954, 0.26130258, 0.27577711]
// =============================================================================
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

class ImagePreprocessor {
public:
    // Target spatial dimensions expected by the model
    static constexpr int TARGET_W = 448;
    static constexpr int TARGET_H = 448;

    // Qwen2.5-VL normalisation constants (ImageNet-style)
    static constexpr float MEAN_R = 0.48145466f;
    static constexpr float MEAN_G = 0.4578275f;
    static constexpr float MEAN_B = 0.40821073f;
    static constexpr float STD_R  = 0.26862954f;
    static constexpr float STD_G  = 0.26130258f;
    static constexpr float STD_B  = 0.27577711f;

    // -------------------------------------------------------------------------
    // Preprocess a JPEG/PNG/BMP file on disk.
    // Writes pixel_values.raw into outputDir and returns its path.
    // -------------------------------------------------------------------------
    static fs::path preprocessFile(const fs::path& imagePath,
                                    const fs::path& outputDir);

    // -------------------------------------------------------------------------
    // Preprocess raw image bytes already in memory (e.g. decoded from base64).
    // stem is used to name the output file (e.g. "upload_abc123").
    // -------------------------------------------------------------------------
    static fs::path preprocessBytes(const std::vector<uint8_t>& imageBytes,
                                     const std::string&           stem,
                                     const fs::path&              outputDir);

    // -------------------------------------------------------------------------
    // Decode a standard base64 string (no data-URL prefix) to raw bytes.
    // -------------------------------------------------------------------------
    static std::vector<uint8_t> base64Decode(const std::string& encoded);

    // -------------------------------------------------------------------------
    // Strip the "data:image/xxx;base64," prefix and return the raw base64 part.
    // Returns the original string unchanged if no prefix is found.
    // -------------------------------------------------------------------------
    static std::string stripDataUrlPrefix(const std::string& dataUrl);

private:
    // Internal: resize src (srcW x srcH, 3 channels, HWC uint8) to
    // (TARGET_W x TARGET_H) using bilinear interpolation.
    static std::vector<uint8_t> bilinearResize(const uint8_t* src,
                                                int srcW, int srcH);

    // Internal: convert resized HWC uint8 → CHW float32 with normalisation.
    static std::vector<float> hwcToChwNormalized(const std::vector<uint8_t>& hwc);

    // Internal: write float32 vector to a .raw file.
    static void writeRaw(const std::vector<float>& data, const fs::path& outPath);
};
