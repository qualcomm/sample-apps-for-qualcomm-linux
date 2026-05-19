// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#include "ChatApp.hpp"

#include <algorithm>
#include <filesystem>
#include <vector>

#include <nlohmann/json.hpp>

#include "Logger.hpp"
#include "crow.h"

namespace fs = std::filesystem;

namespace App {

// -----------------------------------------------------------------
// Model discovery helpers
// -----------------------------------------------------------------
std::unordered_map<std::string, ModelSpec> ChatApp::DiscoverModels(const std::string& bundle_dir)
{
    std::unordered_map<std::string, ModelSpec> models;

    if (bundle_dir.empty() || !fs::exists(bundle_dir) || !fs::is_directory(bundle_dir))
        return models;

    for (const auto& entry : fs::directory_iterator(bundle_dir)) {
        if (!entry.is_directory())
            continue;

        const auto bundle_path = entry.path();
        const auto bundle_name = bundle_path.filename().string();
        const auto config_path = (bundle_path / "genie_config.json").string();

        if (!fs::exists(config_path) || !fs::is_regular_file(config_path))
            continue;

        models.emplace(bundle_name,
                       ModelSpec{
                           /*name=*/bundle_name,
                           /*config_path=*/config_path,
                           /*base_dir=*/bundle_path.string(),
                       });
    }

    return models;
}

std::string ChatApp::ChooseAlphabeticalFirstModel(const std::unordered_map<std::string, ModelSpec>& models)
{
    if (models.empty())
        return {};

    std::vector<std::string> names;
    names.reserve(models.size());
    for (const auto& kv : models)
        names.push_back(kv.first);

    std::sort(names.begin(), names.end());
    return names.front();
}

// -----------------------------------------------------------------
// Constructor / Destructor
// -----------------------------------------------------------------
ChatApp::ChatApp(std::string bundle_dir, std::string initial_model_bundle)
    : m_bundle_dir(std::move(bundle_dir))
{
    Logger::instance().setFile("chat_app.txt", /*append=*/true);
    Logger::instance().rotateOnSize(5 * 1024 * 1024, 3);
    Logger::instance().enableConsole(true);
    Logger::instance().setLevel(LogLevel::Debug);

    m_models = DiscoverModels(m_bundle_dir);
    if (m_models.empty()) {
        APP_LOG_ERROR() << "No models discovered in bundle dir: " << m_bundle_dir;
        return;
    }

    if (initial_model_bundle.empty()) {
        initial_model_bundle = ChooseAlphabeticalFirstModel(m_models);
        APP_LOG_INFO() << "Initial model not specified; selecting alphabetical first: " << initial_model_bundle;
    }

    std::string err;
    if (!LoadModel(initial_model_bundle, err)) {
        APP_LOG_ERROR() << "Failed to load initial model '" << initial_model_bundle << "': " << err;

        // Fallback: try alphabetical first (in case user passed a bad model name)
        const auto fallback = ChooseAlphabeticalFirstModel(m_models);
        if (!fallback.empty() && fallback != initial_model_bundle) {
            std::string err2;
            if (LoadModel(fallback, err2)) {
                APP_LOG_INFO() << "Loaded fallback initial model: " << fallback;
            } else {
                APP_LOG_ERROR() << "Failed to load fallback initial model '" << fallback << "': " << err2;
            }
        }
    }
}

ChatApp::~ChatApp()
{
    if (m_active_genie_owner) {
        m_active_genie_owner->cleanup();
        m_active_genie_owner.reset();
    }
    m_active_genie = nullptr;
}

// -----------------------------------------------------------------
// Internal: load/switch model
// -----------------------------------------------------------------
bool ChatApp::LoadModel(const std::string& model_bundle, std::string& out_error)
{
    out_error.clear();

    auto it = m_models.find(model_bundle);
    if (it == m_models.end()) {
        out_error = "Unknown model_bundle: " + model_bundle;
        return false;
    }

    // If already active, no-op.
    if (m_active_genie && m_active_model_bundle == model_bundle && m_active_genie_owner && m_active_genie_owner->isReady()) {
        return true;
    }

    const auto& spec = it->second;
    APP_LOG_INFO() << "Loading model bundle='" << spec.name << "' config='" << spec.config_path << "' base_dir='" << spec.base_dir << "'";

    try {
        // Cleanup current model first (Genie init is memory heavy on target)
        if (m_active_genie_owner) {
            APP_LOG_DEBUG() << "Cleaning up currently active model: " << m_active_model_bundle;
            m_active_genie_owner->cleanup();
            m_active_genie_owner.reset();
            m_active_genie = nullptr;
            m_active_model_bundle.clear();
        }

        m_active_genie_owner = std::make_unique<Genie>(spec.config_path, spec.base_dir, /*max_tokens=*/200);
        m_active_genie_owner->initialize();

        m_active_genie = m_active_genie_owner.get();
        m_active_model_bundle = spec.name;

        APP_LOG_INFO() << "Active model set to: " << m_active_model_bundle;
        return true;
    } catch (const std::exception& e) {
        out_error = e.what();

        // Ensure we don't keep a partially initialized object
        if (m_active_genie_owner) {
            m_active_genie_owner->cleanup();
            m_active_genie_owner.reset();
        }
        m_active_genie = nullptr;
        m_active_model_bundle.clear();
        return false;
    }
}

// -----------------------------------------------------------------
// Run
// -----------------------------------------------------------------
void ChatApp::Run(uint16_t port)
{
    crow::SimpleApp app;

    // POST /process
    //
    // Accepts:
    //  - prompt (string) [required]
    //  - prompt_is_formatted (bool) [optional] (ignored; prompt is always treated as final)
    //  - model_bundle (string) [optional] -> auto-switch if different from active
    //  - max_tokens (int) [optional]
    //  - sampler_block (string) [optional] (JSON for GenieSamplerConfig_createFromJson)
    //
    CROW_ROUTE(app, "/process").methods(crow::HTTPMethod::Post)([this](const crow::request& req) {
        crow::json::wvalue json_response;
        APP_LOG_INFO() << "Incoming /process body size=" << req.body.size();

        auto body_params = crow::json::load(req.body);
        if (!body_params || !body_params.has("prompt")) {
            return crow::response(400, "Invalid JSON: missing 'prompt'");
        }

        if (!m_active_genie) {
            json_response["status"] = "failure";
            json_response["answer"] = "No active model loaded.";
            return crow::response(500, json_response);
        }

        // Optional: auto switch model per request
        if (body_params.has("model_bundle")) {
            const std::string requested_bundle = body_params["model_bundle"].s();
            if (!requested_bundle.empty() && requested_bundle != m_active_model_bundle) {
                std::string err;
                if (!LoadModel(requested_bundle, err)) {
                    json_response["status"] = "failure";
                    json_response["answer"] = "Failed to load model_bundle '" + requested_bundle + "': " + err;
                    return crow::response(400, json_response);
                }
            }
        }

        // Optional: store backend system prompt (frontend can still pre-format prompts)
        if (body_params.has("system_prompt")) {
            try {
                const std::string sys = body_params["system_prompt"].s();
                if (!sys.empty()) m_system_prompt = sys;
            } catch (...) {
                // ignore invalid types
            }
        }

        // Apply per-request settings (optional)
        if (body_params.has("max_tokens")) {
            try {
                const auto mt = static_cast<uint32_t>(body_params["max_tokens"].i());
                m_active_genie->setMaxTokens(mt);
            } catch (...) {
                // ignore invalid type/value
            }
        }
        if (body_params.has("sampler_block")) {
            try {
                const std::string sampler_block = body_params["sampler_block"].s();
                if (!sampler_block.empty()) {
                    m_active_genie->applySamplerConfig(sampler_block);
                }
            } catch (...) {
                // ignore invalid type/value
            }
        } else {
            // Back-compat: allow passing basic sampler fields, build sampler_block
            // (temperature/top_k/top_p/seed/greedy)
            try {
                const bool has_any =
                    body_params.has("temperature") || body_params.has("top_k") || body_params.has("top_p") || body_params.has("seed") ||
                    body_params.has("greedy");
                if (has_any) {
                    nlohmann::json j;
                    j["sampler"]["version"] = 1;
                    j["sampler"]["type"] = "basic";
                    if (body_params.has("seed")) j["sampler"]["seed"] = body_params["seed"].i();
                    if (body_params.has("temperature")) j["sampler"]["temp"] = body_params["temperature"].d();
                    if (body_params.has("top_k")) j["sampler"]["top-k"] = body_params["top_k"].i();
                    if (body_params.has("top_p")) j["sampler"]["top-p"] = body_params["top_p"].d();
                    if (body_params.has("greedy")) j["sampler"]["greedy"] = body_params["greedy"].b();
                    m_active_genie->applySamplerConfig(j.dump());
                }
            } catch (...) {
            }
        }

        const std::string prompt = body_params["prompt"].s();
        APP_LOG_DEBUG() << "Prompt (no PromptHandler): " << prompt;

        try {
            std::string answer = m_active_genie->query(prompt, GenieDialog_SentenceCode_t::GENIE_DIALOG_SENTENCE_COMPLETE);
            json_response["answer"] = answer;
            json_response["status"] = "success";
            return crow::response(json_response);
        } catch (const std::exception& e) {
            APP_LOG_ERROR() << "Genie query failed: " << e.what();
            try {
                m_active_genie->reload();
                std::string answer = m_active_genie->query(prompt, GenieDialog_SentenceCode_t::GENIE_DIALOG_SENTENCE_COMPLETE);
                json_response["answer"] = answer;
                json_response["status"] = "success";
                return crow::response(json_response);
            } catch (const std::exception& e2) {
                json_response["answer"] = std::string("Genie query failed: ") + e2.what();
                json_response["status"] = "failure";
                return crow::response(json_response);
            }
        }
    });

    // POST /reset_model
    CROW_ROUTE(app, "/reset_model").methods(crow::HTTPMethod::Post)([this](const crow::request& /*req*/) {
        crow::json::wvalue json_response;
        APP_LOG_INFO() << "Incoming /reset_model";

        if (!m_active_genie) {
            json_response["status"] = "failure";
            json_response["message"] = "No active model loaded.";
            return crow::response(500, json_response);
        }

        const bool ok = m_active_genie->reset();
        if (ok) {
            json_response["status"] = "success";
            json_response["message"] = "Model reset successfully";
        } else {
            json_response["status"] = "failure";
            json_response["message"] = "Model reset failed";
        }
        return crow::response(json_response);
    });

    // POST /reload_model
    //
    // Supports:
    //  - model_bundle (string) OR model (string)  [optional] switch model
    //  - max_tokens (int)                         [optional] apply max tokens
    //  - sampler_block (string)                   [optional] apply sampler config
    //
    CROW_ROUTE(app, "/reload_model").methods(crow::HTTPMethod::Post)([this](const crow::request& req) {
        crow::json::wvalue json_response;
        APP_LOG_INFO() << "Incoming /reload_model";

        auto body_params = crow::json::load(req.body);
        if (!body_params) {
            return crow::response(400, "Invalid JSON body");
        }

        // Optional: store backend system prompt (frontend can still pre-format prompts)
        if (body_params.has("system_prompt")) {
            try {
                const std::string sys = body_params["system_prompt"].s();
                if (!sys.empty()) m_system_prompt = sys;
            } catch (...) {
                // ignore invalid types
            }
        }

        // Model switch (optional)
        std::string requested_bundle;
        if (body_params.has("model_bundle")) {
            requested_bundle = body_params["model_bundle"].s();
        } else if (body_params.has("model")) { // legacy key
            requested_bundle = body_params["model"].s();
        }

        if (!requested_bundle.empty() && requested_bundle != m_active_model_bundle) {
            APP_LOG_DEBUG() << "Requested model_bundle: " << requested_bundle;
            std::string err;
            if (!LoadModel(requested_bundle, err)) {
                json_response["status"] = "failure";
                json_response["message"] = "Failed to load model_bundle '" + requested_bundle + "': " + err;
                return crow::response(400, json_response);
            }
        }

        if (!m_active_genie) {
            json_response["status"] = "failure";
            json_response["message"] = "No active model loaded.";
            return crow::response(500, json_response);
        }

        // Apply config changes (optional)
        if (body_params.has("max_tokens")) {
            try {
                const auto mt = static_cast<uint32_t>(body_params["max_tokens"].i());
                m_active_genie->setMaxTokens(mt);
            } catch (...) {
            }
        }
        if (body_params.has("sampler_block")) {
            try {
                const std::string sampler_block = body_params["sampler_block"].s();
                if (!sampler_block.empty()) {
                    m_active_genie->applySamplerConfig(sampler_block);
                }
            } catch (...) {
            }
        }

        // Always reload the model instance if payload contains no model switch but wants reload semantics:
        // For now, do a Genie reload if no model switch requested and no settings provided.
        const bool no_switch = requested_bundle.empty() || requested_bundle == m_active_model_bundle;
        const bool has_settings = body_params.has("max_tokens") || body_params.has("sampler_block");
        if (no_switch && !has_settings) {
            try {
                m_active_genie->reload();
            } catch (...) {
            }
        }

        json_response["status"] = "success";
        json_response["message"] = requested_bundle.empty() ? "Model reloaded" : ("Active model: " + m_active_model_bundle);
        return crow::response(json_response);
    });

    // -----------------------------------------------------------------
    // Start server
    // -----------------------------------------------------------------
    APP_LOG_INFO() << "Starting HTTP server on port " << port;
    app.port(port).multithreaded().run();
}

} // namespace App