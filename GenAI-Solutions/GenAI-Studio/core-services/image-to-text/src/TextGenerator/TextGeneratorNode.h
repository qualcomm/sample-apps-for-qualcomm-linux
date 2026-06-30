// =============================================================================
// Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// =============================================================================

#pragma once

#include <string>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <filesystem>
#include <atomic>
#include <functional>
#include <string_view>


#include "GenieNode.h"
#include "GenieUtils.h"
#include "GenieCheck.h"   // GENIE_CHECK macro (or include your macro header)



namespace fs = std::filesystem;

class TextGeneratorNode {
public:

    using OnDelta = std::function<void(std::string_view)>;

    explicit TextGeneratorNode(const fs::path& textGenJsonPath);
    ~TextGeneratorNode();

    GenieNode_Handle_t handle() const { return node_; }

    void enableTextCallback(GenieNode_IOName_t outputIoName);

    void setCallbackReturn(int v);

    // Wait until output becomes idle (no new text for idleTimeoutMs) or timeout
    bool waitForDoneOrIdle(int totalTimeoutSeconds, int idleTimeoutMs);

    std::string getText() const;
    void clear();
    void setOnDelta(OnDelta cb);

private:
    static int textCallback(const char* text,
                            GenieNode_TextOutput_SentenceCode_t sentenceCode,
                            const void* userData);

private:
    GenieNodeConfig_Handle_t cfg_ = nullptr;
    GenieNode_Handle_t node_ = nullptr;

    mutable std::mutex m_;
    std::condition_variable cv_;

    bool gotFirst_ = false;
    bool done_ = false;
    std::string text_;

    std::chrono::steady_clock::time_point lastChunkTime_{};

    std::atomic<int> cbRet_{0};  // default; you will set from main

    static TextGeneratorNode* s_activeInstance_;

    std::mutex cb_m_;
    OnDelta onDelta_;
};
