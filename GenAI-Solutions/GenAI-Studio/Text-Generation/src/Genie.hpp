// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#pragma once

#include <string>
#include <mutex>
#include <stdexcept>
#include <iostream>

#include "GenieCommon.h"
#include "GenieDialog.h"
#include "GenieSampler.h"

class Genie {
public:
    explicit Genie(std::string config_path,
                   std::string base_dir,
                   uint32_t max_tokens = 200);
    ~Genie();

    void initialize();
    void cleanup() noexcept;
    void reload();
    bool reset() noexcept;

    bool setMaxTokens(uint32_t max_tokens) noexcept;
    uint32_t maxTokens() const noexcept { return max_tokens_; }
    bool isReady() const noexcept;

    void applySamplerConfig(const std::string& samplerBlock);

    std::string query(const std::string& prompt,
                      GenieDialog_SentenceCode_t sentence_code =
                          GenieDialog_SentenceCode_t::GENIE_DIALOG_SENTENCE_COMPLETE);

private:
    std::string config_path_;
    std::string base_dir_;
    uint32_t    max_tokens_{200};

    GenieDialogConfig_Handle_t cfg_{nullptr};
    GenieDialog_Handle_t       dlg_{nullptr};

    mutable std::mutex mu_;

    void _cleanupUnlocked() noexcept;
    void _initializeUnlocked();

    static void Callback(const char* response_back,
                         const GenieDialog_SentenceCode_t sentence_code,
                         const void* user_data);
};