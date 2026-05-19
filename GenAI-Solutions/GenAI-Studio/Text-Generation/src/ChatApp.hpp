// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "Genie.hpp"

namespace App {

struct ModelSpec {
    std::string name;        // bundle folder name
    std::string config_path; // <bundle>/genie_config.json
    std::string base_dir;    // <bundle>
};

class ChatApp {
public:
    explicit ChatApp(std::string bundle_dir, std::string initial_model_bundle = "");
    ~ChatApp();

    void Run(uint16_t port);

private:
    std::string m_bundle_dir;

    // Registry of models discovered on disk (key: bundle name)
    std::unordered_map<std::string, ModelSpec> m_models;

    // Owning pointer to the currently active model instance
    std::unique_ptr<Genie> m_active_genie_owner;
    Genie* m_active_genie{nullptr};
    std::string m_active_model_bundle;

    // Backend-owned system prompt (can be set via /reload_model or /process)
    std::string m_system_prompt{"You are a helpful assistant."};

    static std::unordered_map<std::string, ModelSpec> DiscoverModels(const std::string& bundle_dir);
    static std::string ChooseAlphabeticalFirstModel(const std::unordered_map<std::string, ModelSpec>& models);

    bool LoadModel(const std::string& model_bundle, std::string& out_error);
};

} // namespace App