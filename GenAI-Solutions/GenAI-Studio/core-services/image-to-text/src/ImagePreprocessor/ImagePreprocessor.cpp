// =============================================================================
// Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// =============================================================================
// ImagePreprocessor.cpp
// =============================================================================
// STB image is a single-header library. We define the implementation here
// (only once across the whole project).
// =============================================================================

#ifndef I2T_BASE64_ONLY
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO          // we feed bytes directly, not FILE*
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#include "stb_image.h"
#endif

#include "ImagePreprocessor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <chrono>

// =============================================================================
// base64 decode
// =============================================================================

static const std::string kB64Chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static inline bool isB64(unsigned char c)
{
    return (c == '=') || (kB64Chars.find(c) != std::string::npos);
}

std::vector<uint8_t> ImagePreprocessor::base64Decode(const std::string& encoded)
{
    std::vector<uint8_t> out;
    out.reserve(encoded.size() * 3 / 4);

    int i = 0;
    unsigned char char4[4], char3[3];
    int idx = 0;

    for (unsigned char c : encoded) {
        if (c == '\n' || c == '\r' || c == ' ') continue;
        if (c == '=') {
            char4[i++] = 0;
            if (i == 4) {
                for (int j = 0; j < 4; ++j)
                    char4[j] = static_cast<unsigned char>(kB64Chars.find(char4[j]));
                char3[0] = (char4[0] << 2) | (char4[1] >> 4);
                char3[1] = (char4[1] << 4) | (char4[2] >> 2);
                char3[2] = (char4[2] << 6) | char4[3];
                // Only push non-padding bytes
                out.push_back(char3[0]);
                if (encoded[idx - 2] != '=') out.push_back(char3[1]);
                if (encoded[idx - 1] != '=') out.push_back(char3[2]);
                i = 0;
            }
        } else {
            char4[i++] = c;
            if (i == 4) {
                for (int j = 0; j < 4; ++j)
                    char4[j] = static_cast<unsigned char>(kB64Chars.find(char4[j]));
                char3[0] = (char4[0] << 2) | (char4[1] >> 4);
                char3[1] = (char4[1] << 4) | (char4[2] >> 2);
                char3[2] = (char4[2] << 6) | char4[3];
                for (int j = 0; j < 3; ++j) out.push_back(char3[j]);
                i = 0;
            }
        }
        ++idx;
    }

    return out;
}

std::string ImagePreprocessor::stripDataUrlPrefix(const std::string& dataUrl)
{
    // "data:image/jpeg;base64,<data>"
    auto comma = dataUrl.find(',');
    if (comma != std::string::npos && dataUrl.rfind("data:", 0) == 0) {
        return dataUrl.substr(comma + 1);
    }
    return dataUrl;
}

// =============================================================================
// Bilinear resize  (HWC uint8, 3 channels)
// =============================================================================

std::vector<uint8_t> ImagePreprocessor::bilinearResize(
    const uint8_t* src, int srcW, int srcH)
{
    const int dstW = TARGET_W;
    const int dstH = TARGET_H;
    std::vector<uint8_t> dst(dstW * dstH * 3);

    const float scaleX = static_cast<float>(srcW) / dstW;
    const float scaleY = static_cast<float>(srcH) / dstH;

    for (int dy = 0; dy < dstH; ++dy) {
        float fy = (dy + 0.5f) * scaleY - 0.5f;
        int   y0 = static_cast<int>(std::floor(fy));
        int   y1 = y0 + 1;
        float wy = fy - y0;
        y0 = std::max(0, std::min(y0, srcH - 1));
        y1 = std::max(0, std::min(y1, srcH - 1));

        for (int dx = 0; dx < dstW; ++dx) {
            float fx = (dx + 0.5f) * scaleX - 0.5f;
            int   x0 = static_cast<int>(std::floor(fx));
            int   x1 = x0 + 1;
            float wx = fx - x0;
            x0 = std::max(0, std::min(x0, srcW - 1));
            x1 = std::max(0, std::min(x1, srcW - 1));

            for (int c = 0; c < 3; ++c) {
                float v00 = src[(y0 * srcW + x0) * 3 + c];
                float v01 = src[(y0 * srcW + x1) * 3 + c];
                float v10 = src[(y1 * srcW + x0) * 3 + c];
                float v11 = src[(y1 * srcW + x1) * 3 + c];

                float v = v00 * (1 - wx) * (1 - wy)
                        + v01 * wx       * (1 - wy)
                        + v10 * (1 - wx) * wy
                        + v11 * wx       * wy;

                dst[(dy * dstW + dx) * 3 + c] =
                    static_cast<uint8_t>(std::round(std::max(0.f, std::min(255.f, v))));
            }
        }
    }
    return dst;
}

// =============================================================================
// HWC uint8 → CHW float32 with normalisation
// =============================================================================

std::vector<float> ImagePreprocessor::hwcToChwNormalized(
    const std::vector<uint8_t>& hwc)
{
    const int N = TARGET_W * TARGET_H;
    std::vector<float> chw(3 * N);

    const float mean[3] = { MEAN_R, MEAN_G, MEAN_B };
    const float std_[3] = { STD_R,  STD_G,  STD_B  };

    for (int i = 0; i < N; ++i) {
        for (int c = 0; c < 3; ++c) {
            float v = hwc[i * 3 + c] / 255.0f;
            chw[c * N + i] = (v - mean[c]) / std_[c];
        }
    }
    return chw;
}

// =============================================================================
// Write float32 to .raw
// =============================================================================

void ImagePreprocessor::writeRaw(const std::vector<float>& data,
                                  const fs::path& outPath)
{
    std::ofstream ofs(outPath, std::ios::binary);
    if (!ofs) {
        throw std::runtime_error("Cannot write pixel_values.raw: " + outPath.string());
    }
    ofs.write(reinterpret_cast<const char*>(data.data()),
              data.size() * sizeof(float));
    if (!ofs) {
        throw std::runtime_error("Write failed: " + outPath.string());
    }
}

#ifndef I2T_BASE64_ONLY
// =============================================================================
// Core preprocessing from raw bytes
// =============================================================================

static fs::path preprocessBytesImpl(
    const uint8_t* data, int len,
    const std::string& stem,
    const fs::path& outputDir)
{
    // Decode image
    int w = 0, h = 0, channels = 0;
    uint8_t* pixels = stbi_load_from_memory(
        data, len, &w, &h, &channels, 3 /*force RGB*/);

    if (!pixels) {
        throw std::runtime_error(
            std::string("Failed to decode image: ") + stbi_failure_reason());
    }

    // Resize to 448x448
    auto resized = ImagePreprocessor::bilinearResize(pixels, w, h);
    stbi_image_free(pixels);

    // Normalise → CHW float32
    auto chw = ImagePreprocessor::hwcToChwNormalized(resized);

    // Write output
    fs::create_directories(outputDir);
    fs::path outPath = outputDir / (stem + "_pixel_values.raw");
    ImagePreprocessor::writeRaw(chw, outPath);

    return outPath;
}

// =============================================================================
// Public API
// =============================================================================

fs::path ImagePreprocessor::preprocessFile(
    const fs::path& imagePath,
    const fs::path& outputDir)
{
    // Read file into memory
    std::ifstream ifs(imagePath, std::ios::binary);
    if (!ifs) {
        throw std::runtime_error("Cannot open image file: " + imagePath.string());
    }
    std::vector<uint8_t> buf(
        (std::istreambuf_iterator<char>(ifs)),
        std::istreambuf_iterator<char>());

    std::string stem = imagePath.stem().string();
    return preprocessBytesImpl(buf.data(), static_cast<int>(buf.size()),
                                stem, outputDir);
}

fs::path ImagePreprocessor::preprocessBytes(
    const std::vector<uint8_t>& imageBytes,
    const std::string& stem,
    const fs::path& outputDir)
{
    return preprocessBytesImpl(imageBytes.data(),
                                static_cast<int>(imageBytes.size()),
                                stem, outputDir);
}
#endif
