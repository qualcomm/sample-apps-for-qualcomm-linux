// =============================================================================
// Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// =============================================================================

#include "TextGeneratorNode.h"
#include <iostream>

TextGeneratorNode* TextGeneratorNode::s_activeInstance_ = nullptr;

TextGeneratorNode::TextGeneratorNode(const fs::path& textGenJsonPath) {
    mustExistFile(textGenJsonPath);
    std::string json = readTextFile(textGenJsonPath);

    GENIE_CHECK(GenieNodeConfig_createFromJson(json.c_str(), &cfg_));
    GENIE_CHECK(GenieNode_create(cfg_, &node_));
}

TextGeneratorNode::~TextGeneratorNode() {
    if (node_) GenieNode_free(node_);
    if (cfg_)  GenieNodeConfig_free(cfg_);
}

void TextGeneratorNode::enableTextCallback(GenieNode_IOName_t outputIoName) {
    {
        std::lock_guard<std::mutex> lk(m_);
        gotFirst_ = false;
        done_ = false;
        text_.clear();
        lastChunkTime_ = std::chrono::steady_clock::now();
    }

    s_activeInstance_ = this;

    GENIE_CHECK(GenieNode_setTextCallback(node_, outputIoName, &TextGeneratorNode::textCallback));
}

void TextGeneratorNode::setOnDelta(OnDelta cb) {
    std::lock_guard<std::mutex> lk(cb_m_);
    onDelta_ = std::move(cb);
}

void TextGeneratorNode::setCallbackReturn(int v) {
    cbRet_.store(v, std::memory_order_relaxed);
}

bool TextGeneratorNode::waitForDoneOrIdle(int totalTimeoutSeconds, int idleTimeoutMs) {
    using namespace std::chrono;

    std::unique_lock<std::mutex> lk(m_);
    const auto deadline = steady_clock::now() + seconds(totalTimeoutSeconds);

    while (!done_) {
        cv_.wait_for(lk, milliseconds(200));
        const auto now = steady_clock::now();

        if (gotFirst_) {
            auto idleFor = duration_cast<milliseconds>(now - lastChunkTime_).count();
            if (idleFor >= idleTimeoutMs) {
                done_ = true;
                break;
            }
        }

        if (now >= deadline) break;
    }
    return done_;
}

std::string TextGeneratorNode::getText() const {
    std::lock_guard<std::mutex> lk(m_);
    return text_;
}

void TextGeneratorNode::clear() {
    std::lock_guard<std::mutex> lk(m_);
    text_.clear();
    gotFirst_ = false;
    done_ = false;
    lastChunkTime_ = std::chrono::steady_clock::now();
}


int TextGeneratorNode::textCallback(const char* text,
                                   GenieNode_TextOutput_SentenceCode_t sentenceCode,
                                   const void* userData) {
    (void)userData;
    (void)sentenceCode;

    auto* self = s_activeInstance_;
    if (!self) return 0;

    
    TextGeneratorNode::OnDelta cb;
    {
        std::lock_guard<std::mutex> lk(self->cb_m_);
        cb = self->onDelta_;
    }

    if (text && *text) {
        {
            std::lock_guard<std::mutex> lk(self->m_);
            self->text_ += text;
            self->gotFirst_ = true;
            self->lastChunkTime_ = std::chrono::steady_clock::now();
        }

        self->cv_.notify_one();

      
        if (cb) cb(std::string_view{text});
    }

    return self->cbRet_.load(std::memory_order_relaxed);
}