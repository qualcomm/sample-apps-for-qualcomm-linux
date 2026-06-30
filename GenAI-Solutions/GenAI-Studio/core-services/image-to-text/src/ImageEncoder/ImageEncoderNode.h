// =============================================================================
// Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// =============================================================================

#pragma once
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <filesystem>

#include "Utils/GenieCheck.h"
#include "Utils/GenieUtils.h"

#include "GenieNode.h"
#include "GeniePipelineRunner.h"  
#include "GenieUtils.h"
#include "GeniePipeline.h"
#include "GenieNode.h"
#include "GenieCommon.h"
#include "GenieDialog.h"
#include "GenieSampler.h"



namespace fs = std::filesystem;

class ImageEncoderNode {
public:
    explicit ImageEncoderNode(const fs::path& imgEncJsonPath);
    ~ImageEncoderNode();

    GenieNode_Handle_t handle() const { return node_; }

    // Load + bind all 5 inputs per request
    void loadAndSetInputs(const fs::path& pixelValues,
                          const fs::path& posCos,
                          const fs::path& posSin,
                          const fs::path& winMask,
                          const fs::path& fullMask);

    // Unbind inputs from GENIE (safe dummy buffers) and/or clear stored buffers
    void releaseInputs(bool unbindFromGenie = false, bool freeHostBuffers = false);

    

private:
    // Helper: safely bind a buffer to an input
    void setData_(GenieNode_IOName_t inputName, const std::vector<uint8_t>& buf);

    // Helper: unbind a specific input using a 1-byte dummy
    void unbind_(GenieNode_IOName_t inputName);

private:
    GenieNodeConfig_Handle_t cfg_ = nullptr;
    GenieNode_Handle_t node_ = nullptr;

    // IMPORTANT: these must live across execute()
    // std::vector<uint8_t> pixelValues_;
    // std::vector<uint8_t> posCos_;
    // std::vector<uint8_t> posSin_;
    // std::vector<uint8_t> winMask_;
    // std::vector<uint8_t> fullMask_;

    mutable std::mutex mu_;
};