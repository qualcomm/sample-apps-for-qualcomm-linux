// =============================================================================
// Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// =============================================================================
#pragma once
#include <filesystem>
#include <mutex>
#include <string>
#include <functional>
#include <thread>




#include "GeniePipelineRunner.h"
#include "ImageEncoderNode.h"
#include "LutEncoderNode.h"
#include "TextGeneratorNode.h"
#include "GenieUtils.h"



namespace fs = std::filesystem;

class VlmService {
public:
    explicit VlmService(const fs::path& baseDir);

    // per request: only pixel_values_path is dynamic
    std::string runVision(const fs::path& pixelValuesPath,
                        const std::string& userPrompt);
    std::string runText(const std::string& userPrompt,
                        bool reset_before_run = true,
                        bool followup_turn = false);

    using OnDelta = TextGeneratorNode::OnDelta;
    using OnDone  = std::function<void()>;
    using OnError = std::function<void(const std::string&)>;

    std::string runTextContinuous(const std::string userPrompt,
                                OnDelta onDelta,
                                OnDone onDone = {},
                                OnError onError = {},
                                bool reset_before_run = true,
                                bool followup_turn = false);

    std::string runVisionContinuous(const fs::path& pixelValuesPath,
                                    const std::string& userPrompt,
                                    OnDelta onDelta,
                                    OnDone onDone = {},
                                    OnError onError = {});

    std::string resetSession();

private:
    fs::path safeJoinUnderBase(const fs::path& p) const;
    void resetPipelineOrThrow_(const char* context);
    void clearStateForRun_(bool release_image_inputs);
    void prepareVisionInputs_(const fs::path& pixelValuesPath,
                              const std::string& userPrompt);


private:
    fs::path baseDir_;
    fs::path inputsDir_;

    GeniePipelineRunner pipeline_;
    ImageEncoderNode imageEnc_;
    LutEncoderNode lutEnc_;
    TextGeneratorNode textGen_;

    std::mutex m_;
};
