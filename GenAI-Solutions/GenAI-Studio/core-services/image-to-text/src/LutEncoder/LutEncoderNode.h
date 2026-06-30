// =============================================================================
// Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// =============================================================================

#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <stdexcept>

#include "GenieNode.h"
#include "GenieUtils.h"
#include "GenieCheck.h"





namespace fs = std::filesystem;

class LutEncoderNode {
public:
    explicit LutEncoderNode(const fs::path& textEncJsonPath);
    ~LutEncoderNode();

    GenieNode_Handle_t handle() const { return node_; }

    // Overwrite prompt buffer
    void setTextInput(const std::string& text, const char* dataConfig = nullptr);

    // Append prompt buffer and resend full prompt (emulates "set text twice" behavior)
    void appendTextInput(const std::string& moreText, const char* dataConfig = nullptr);

    // Optional: for debugging
    void enableEmbeddingCallback(GenieNode_IOName_t outputIoName);

    void clearText();
    void clearEmbedding(); 

private:
    static void embeddingCallback(const unsigned int* meta,
                                  unsigned int metaCount,
                                  size_t dataSize,
                                  const void* data,
                                  const void* userOrReserved);

private:
    GenieNodeConfig_Handle_t cfg_ = nullptr;
    GenieNode_Handle_t node_ = nullptr;

    // Must remain alive while node runs (GenieNode_setData uses raw pointer)
    std::string text_;

    // Optional embedding capture
    mutable std::mutex m_;
    std::condition_variable cv_;
    bool ready_ = false;
    bool gotEmpty_ = false;
    

    static LutEncoderNode* s_activeInstance_;
};