// =============================================================================
// Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// =============================================================================

#include "ImageEncoderNode.h"
#include <iostream>

ImageEncoderNode::ImageEncoderNode(const fs::path& imgEncJsonPath) {
    mustExistFile(imgEncJsonPath);
    std::string json = readTextFile(imgEncJsonPath);

    GENIE_CHECK(GenieNodeConfig_createFromJson(json.c_str(), &cfg_));
    GENIE_CHECK(GenieNode_create(cfg_, &node_));
}

ImageEncoderNode::~ImageEncoderNode() {
    // Best-effort: unbind before freeing node (avoids backend touching stale pointers)
    try {
        releaseInputs(true, true);
    } catch (...) {}

    if (node_) GenieNode_free(node_);
    if (cfg_)  GenieNodeConfig_free(cfg_);

    node_ = nullptr;
    cfg_  = nullptr;
}





void ImageEncoderNode::unbind_(GenieNode_IOName_t inputName) {
    // IMPORTANT: use size=1 (not 0). Some backends mis-handle 0.
    static uint8_t dummy = 0;
    GENIE_CHECK(GenieNode_setData(node_, inputName, &dummy, 1, nullptr));
}

void ImageEncoderNode::setData_(GenieNode_IOName_t inputName, const std::vector<uint8_t>& buf) {
    if (buf.empty()) {
        // If empty, still bind dummy to keep runtime stable
        unbind_(inputName);
        return;
    }
    GENIE_CHECK(GenieNode_setData(node_, inputName, buf.data(), buf.size(), nullptr));
}

void ImageEncoderNode::releaseInputs(bool unbindFromGenie, bool freeHostBuffers) {
    std::lock_guard<std::mutex> lk(mu_);

    // if (unbindFromGenie) {
    //     unbind_(GENIE_NODE_IMAGE_ENCODER_IMAGE_INPUT);
    //     unbind_(GENIE_NODE_IMAGE_ENCODER_IMAGE_POS_COS);
    //     unbind_(GENIE_NODE_IMAGE_ENCODER_IMAGE_POS_SIN);
    //     unbind_(GENIE_NODE_IMAGE_ENCODER_IMAGE_WINDOW_ATTN_MASK);
    //     unbind_(GENIE_NODE_IMAGE_ENCODER_IMAGE_FULL_ATTN_MASK);
    // }

    // if (freeHostBuffers) {
    //     std::vector<uint8_t>().swap(pixelValues_);
    //     std::vector<uint8_t>().swap(posCos_);
    //     std::vector<uint8_t>().swap(posSin_);
    //     std::vector<uint8_t>().swap(winMask_);
    //     std::vector<uint8_t>().swap(fullMask_);
    // }
}

void ImageEncoderNode::loadAndSetInputs(const fs::path& pixelValues_path,
                                        const fs::path& posCos_path,
                                        const fs::path& posSin_path,
                                        const fs::path& winMask_path,
                                        const fs::path& fullMask_path) {
    mustExistFile(pixelValues_path);
    mustExistFile(posCos_path);
    mustExistFile(posSin_path);
    mustExistFile(winMask_path);
    mustExistFile(fullMask_path);

    std::lock_guard<std::mutex> lk(mu_);

    // CRITICAL: unbind previous pointers BEFORE overwriting vectors.
    // This prevents backend from holding stale pointers across runs.
    unbind_(GENIE_NODE_IMAGE_ENCODER_IMAGE_INPUT);
    unbind_(GENIE_NODE_IMAGE_ENCODER_IMAGE_POS_COS);
    unbind_(GENIE_NODE_IMAGE_ENCODER_IMAGE_POS_SIN);
    unbind_(GENIE_NODE_IMAGE_ENCODER_IMAGE_WINDOW_ATTN_MASK);
    unbind_(GENIE_NODE_IMAGE_ENCODER_IMAGE_FULL_ATTN_MASK);

    // Load into member buffers (lifetime must exceed execute())
    std::vector<uint8_t> pixelValues_;
    std::vector<uint8_t> posCos_;
    std::vector<uint8_t> posSin_;
    std::vector<uint8_t> winMask_;
    std::vector<uint8_t> fullMask_;
    pixelValues_ = readBinFile(pixelValues_path);
    posCos_      = readBinFile(posCos_path);
    posSin_      = readBinFile(posSin_path);
    winMask_     = readBinFile(winMask_path);
    fullMask_    = readBinFile(fullMask_path);

    std::cout << "[IMG] pixel_values bytes=" << pixelValues_.size() << "\n";
    std::cout << "[IMG] pos_cos      bytes=" << posCos_.size() << "\n";
    std::cout << "[IMG] pos_sin      bytes=" << posSin_.size() << "\n";
    std::cout << "[IMG] win_mask     bytes=" << winMask_.size() << "\n";
    std::cout << "[IMG] full_mask    bytes=" << fullMask_.size() << "\n";

    // Bind pointers backed by member vectors
    setData_(GENIE_NODE_IMAGE_ENCODER_IMAGE_INPUT,            pixelValues_);
    setData_(GENIE_NODE_IMAGE_ENCODER_IMAGE_POS_COS,          posCos_);
    setData_(GENIE_NODE_IMAGE_ENCODER_IMAGE_POS_SIN,          posSin_);
    setData_(GENIE_NODE_IMAGE_ENCODER_IMAGE_WINDOW_ATTN_MASK, winMask_);
    setData_(GENIE_NODE_IMAGE_ENCODER_IMAGE_FULL_ATTN_MASK,   fullMask_);
}