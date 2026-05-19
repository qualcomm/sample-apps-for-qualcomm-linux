// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#include "Genie.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>

Genie::Genie(std::string config_path, std::string base_dir, uint32_t max_tokens)
    : config_path_(std::move(config_path)),
      base_dir_(std::move(base_dir)),
      max_tokens_(max_tokens) {}

Genie::~Genie() {
    cleanup();
}

bool Genie::isReady() const noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    return cfg_ != nullptr && dlg_ != nullptr;
}

void Genie::_cleanupUnlocked() noexcept {
    if (dlg_) {
        if (GENIE_STATUS_SUCCESS != GenieDialog_free(dlg_)) {
            std::cerr << "[Genie] Warning: GenieDialog_free failed." << std::endl;
        }
        dlg_ = nullptr;
    }
    if (cfg_) {
        if (GENIE_STATUS_SUCCESS != GenieDialogConfig_free(cfg_)) {
            std::cerr << "[Genie] Warning: GenieDialogConfig_free failed." << std::endl;
        }
        cfg_ = nullptr;
    }
}

void Genie::_initializeUnlocked() {
    std::filesystem::current_path(base_dir_);

    // GenieDialogConfig_createFromJson expects JSON content, not a file path.
    // Passing a path like "/opt/..." causes JSON parse errors (first char is '/').
    std::ifstream in(config_path_);
    if (!in) {
        throw std::runtime_error("Failed to open genie_config.json: " + config_path_);
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string json = ss.str();

    if (GENIE_STATUS_SUCCESS !=
        GenieDialogConfig_createFromJson(json.c_str(), &cfg_) || !cfg_) {
        throw std::runtime_error(
            "GenieDialogConfig_createFromJson failed (invalid JSON?) for: " + config_path_);
    }

    if (GENIE_STATUS_SUCCESS != GenieDialog_create(cfg_, &dlg_) || !dlg_) {
        _cleanupUnlocked();
        throw std::runtime_error(
            "GenieDialog_create failed for: " + config_path_);
    }
}

void Genie::initialize() {
    std::lock_guard<std::mutex> lock(mu_);
    _initializeUnlocked();
}

void Genie::cleanup() noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    _cleanupUnlocked();
}

void Genie::reload() {
    std::lock_guard<std::mutex> lock(mu_);
    _cleanupUnlocked();
    _initializeUnlocked();
}

bool Genie::reset() noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    if (!dlg_) return false;
    return GENIE_STATUS_SUCCESS == GenieDialog_reset(dlg_);
}

bool Genie::setMaxTokens(uint32_t max_tokens) noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    max_tokens_ = max_tokens;
    return true;
}

void Genie::applySamplerConfig(const std::string& samplerBlock) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!dlg_) return;

    GenieSamplerConfig_Handle_t samplerConfigHandle = nullptr;
    if (GENIE_STATUS_SUCCESS !=
        GenieSamplerConfig_createFromJson(samplerBlock.c_str(), &samplerConfigHandle)) {
        std::cerr << "[Genie] GenieSamplerConfig_createFromJson failed." << std::endl;
        return;
    }

    GenieSampler_Handle_t sampler = nullptr;
    if (GENIE_STATUS_SUCCESS != GenieDialog_getSampler(dlg_, &sampler) || !sampler) {
        std::cerr << "[Genie] GenieDialog_getSampler failed." << std::endl;
        GenieSamplerConfig_free(samplerConfigHandle);
        return;
    }

    if (GENIE_STATUS_SUCCESS != GenieSampler_applyConfig(sampler, samplerConfigHandle)) {
        std::cerr << "[Genie] GenieSampler_applyConfig failed." << std::endl;
    }

    GenieSamplerConfig_free(samplerConfigHandle);
}

std::string Genie::query(const std::string& prompt,
                         GenieDialog_SentenceCode_t sentence_code) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!dlg_) throw std::runtime_error("[Genie] Not initialized.");

    std::string response;
    if (GENIE_STATUS_SUCCESS !=
        GenieDialog_query(dlg_, prompt.c_str(), sentence_code, Callback,
                          static_cast<void*>(&response))) {
        throw std::runtime_error("[Genie] GenieDialog_query failed.");
    }
    return response;
}

// Static callback — appends each token chunk to the response string
void Genie::Callback(const char* response_back,
                     const GenieDialog_SentenceCode_t sentence_code,
                     const void* context) {
    if (response_back && context) {
        auto* response = static_cast<std::string*>(const_cast<void*>(context));
        response->append(response_back);
    }
}
