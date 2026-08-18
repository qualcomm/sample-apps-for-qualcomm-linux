<<<<<<< HEAD
// =============================================================================
// Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// =============================================================================

=======
// Copyright (c) 2024-2026 Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
>>>>>>> 03aa127 (GenAI Studio: update startup, model generation, and setup docs)
#include "VlmService.h"
#include <algorithm>
#include <cstdlib>
#include <fcntl.h>
#include <stdexcept>
#include <iostream>
#include <chrono>
#include <cmath>
#include <sstream>
#include <vector>
#include <sys/file.h>
#include <thread>
#include <unistd.h>

namespace {
double ms_since(const std::chrono::steady_clock::time_point& t0) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
}

double round3(double v) {
    return std::round(v * 1000.0) / 1000.0;
}

int env_int_or_default(const char* key, int fallback) {
    const char* raw = std::getenv(key);
    if (!raw || !*raw) return fallback;
    try {
        return std::max(0, std::stoi(raw));
    } catch (...) {
        return fallback;
    }
}

bool is_retryable_execute_status4(const std::string& msg) {
    return msg.find("GeniePipeline_execute") != std::string::npos &&
           msg.find("status=4") != std::string::npos;
}

int vision_retry_attempts() {
    // Default behavior: tolerate short NPU contention windows.
    return std::max(1, env_int_or_default("I2T_VISION_RETRY_ATTEMPTS", 8));
}

int vision_retry_backoff_ms() {
    return std::max(0, env_int_or_default("I2T_VISION_RETRY_BACKOFF_MS", 1200));
}

std::string npu_lock_file() {
    const char* raw = std::getenv("I2T_NPU_LOCK_FILE");
    if (raw && *raw) return std::string(raw);
    return "/opt/genai-lock/npu.lock";
}

int npu_lock_timeout_ms() {
    return std::max(1000, env_int_or_default("I2T_NPU_LOCK_TIMEOUT_MS", 300000));
}

class ScopedNpuLock {
public:
    ScopedNpuLock(const std::string& lock_file, int timeout_ms) {
        std::error_code ec;
        const fs::path lock_path(lock_file);
        if (lock_path.has_parent_path()) {
            fs::create_directories(lock_path.parent_path(), ec);
        }
        fd_ = ::open(lock_file.c_str(), O_CREAT | O_RDWR, 0666);
        if (fd_ < 0) {
            throw std::runtime_error("npu_lock_open_failed");
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        while (true) {
            if (::flock(fd_, LOCK_EX | LOCK_NB) == 0) {
                locked_ = true;
                return;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("npu_lock_timeout");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    ~ScopedNpuLock() {
        if (fd_ >= 0) {
            if (locked_) {
                ::flock(fd_, LOCK_UN);
            }
            ::close(fd_);
        }
    }

    ScopedNpuLock(const ScopedNpuLock&) = delete;
    ScopedNpuLock& operator=(const ScopedNpuLock&) = delete;

private:
    int fd_{-1};
    bool locked_{false};
};

std::string build_initial_text_prompt(const std::string& userPrompt) {
    const std::string prefix =
    "<|im_start|>system\n"
    "You are a helpful assistant.<|im_end|>\n"
    "<|im_start|>user\n";

    const std::string suffix =
    "<|im_end|>\n"
    "<|im_start|>assistant\n";

    return prefix + userPrompt + suffix;
}

std::string build_followup_text_prompt(const std::string& userPrompt) {
    // Continue from the previous assistant turn in the same KV-cache session.
    return std::string("<|im_end|>\n<|im_start|>user\n") +
           userPrompt +
           "<|im_end|>\n<|im_start|>assistant\n";
}

std::string build_vision_prefix() {
    return std::string("<|im_start|>system\n")
        + "You are a helpful detailed visual assistant.<|im_end|>\n"
        + "<|im_start|>user\n"
        + "<|vision_start|>";
}

std::string build_vision_suffix(const std::string& userPrompt) {
    return std::string("<|vision_end|>") + userPrompt +
           "<|im_end|>\n"
           "<|im_start|>assistant\n";
}

std::string env_string_or_empty(const char* key) {
    const char* raw = std::getenv(key);
    return (raw && *raw) ? std::string(raw) : std::string();
}

fs::path resolve_file_from_env_or_fallbacks(const fs::path& baseDir,
                                            const char* env_key,
                                            const std::vector<std::string>& fallbacks) {
    const std::string env_value = env_string_or_empty(env_key);
    std::vector<std::string> tried;

    auto try_candidate = [&](const std::string& candidate) -> fs::path {
        const fs::path p = resolvePath(baseDir, candidate);
        tried.push_back(p.string());
        if (fs::exists(p) && fs::is_regular_file(p)) {
            return p;
        }
        return fs::path();
    };

    if (!env_value.empty()) {
        fs::path p = try_candidate(env_value);
        if (!p.empty()) return p;
        throw std::runtime_error(
            std::string("Configured file from ") + env_key + " not found: " + env_value);
    }

    for (const auto& candidate : fallbacks) {
        fs::path p = try_candidate(candidate);
        if (!p.empty()) return p;
    }

    std::ostringstream oss;
    oss << "Unable to resolve required config file for " << env_key << ". Tried:";
    for (const auto& path : tried) {
        oss << "\n  - " << path;
    }
    throw std::runtime_error(oss.str());
}

fs::path resolve_dir_from_env_or_fallbacks(const fs::path& baseDir,
                                           const char* env_key,
                                           const std::vector<std::string>& fallbacks) {
    const std::string env_value = env_string_or_empty(env_key);
    std::vector<std::string> tried;

    auto try_candidate = [&](const std::string& candidate) -> fs::path {
        const fs::path p = resolvePath(baseDir, candidate);
        tried.push_back(p.string());
        if (fs::exists(p) && fs::is_directory(p)) {
            return p;
        }
        return fs::path();
    };

    if (!env_value.empty()) {
        fs::path p = try_candidate(env_value);
        if (!p.empty()) return p;
        throw std::runtime_error(
            std::string("Configured directory from ") + env_key + " not found: " + env_value);
    }

    for (const auto& candidate : fallbacks) {
        fs::path p = try_candidate(candidate);
        if (!p.empty()) return p;
    }

    std::ostringstream oss;
    oss << "Unable to resolve required directory for " << env_key << ". Tried:";
    for (const auto& path : tried) {
        oss << "\n  - " << path;
    }
    throw std::runtime_error(oss.str());
}
}

VlmService::VlmService(const fs::path& baseDir)
    : baseDir_(baseDir),
      inputsDir_(resolve_dir_from_env_or_fallbacks(
          baseDir, "I2T_INPUTS_DIR", {"inputs", "sample_inputs"})),
      pipeline_(),
      imageEnc_(resolve_file_from_env_or_fallbacks(
          baseDir, "I2T_IMAGE_ENCODER_CONFIG",
          {"img-enc-htp.json", "image_encoder.json"})),
      lutEnc_(resolve_file_from_env_or_fallbacks(
          baseDir, "I2T_TEXT_ENCODER_CONFIG",
          {"text-encoder.json", "lut_encoder.json"})),
      textGen_(resolve_file_from_env_or_fallbacks(
          baseDir, "I2T_TEXT_DECODER_CONFIG",
          {"text-dec-htp.json", "text-generator.json"})) {

    // callback once
    textGen_.enableTextCallback(GENIE_NODE_TEXT_GENERATOR_TEXT_OUTPUT);

    
    // add nodes once
    pipeline_.addNode(imageEnc_.handle());
    pipeline_.addNode(lutEnc_.handle());
    pipeline_.addNode(textGen_.handle());

    // connect once
    pipeline_.connect(imageEnc_.handle(),
                      GENIE_NODE_IMAGE_ENCODER_EMBEDDING_OUTPUT,
                      textGen_.handle(),
                      GENIE_NODE_TEXT_GENERATOR_EMBEDDING_INPUT);

    pipeline_.connect(lutEnc_.handle(),
                      GENIE_NODE_TEXT_ENCODER_EMBEDDING_OUTPUT,
                      textGen_.handle(),
                      GENIE_NODE_TEXT_GENERATOR_EMBEDDING_INPUT);
}

fs::path VlmService::safeJoinUnderBase(const fs::path& p) const {
    // optional safety: force under baseDir_
    if (p.is_absolute()) return p; // if you want to allow absolute paths
    return (baseDir_ / p).lexically_normal();
}

void VlmService::resetPipelineOrThrow_(const char* context) {
    int reset_rc = GeniePipeline_reset(pipeline_.pipeline_handle_);
    if (reset_rc != 0) {
        throw std::runtime_error(std::string(context) + ", status=" +
                                 std::to_string(reset_rc));
    }
}

void VlmService::clearStateForRun_(bool release_image_inputs) {
    textGen_.clear();
    lutEnc_.clearText();
    if (release_image_inputs) {
        imageEnc_.releaseInputs(true, false);
    }
}

void VlmService::prepareVisionInputs_(const fs::path& pixelValuesPath,
                                      const std::string& userPrompt) {
    fs::path pv      = safeJoinUnderBase(pixelValuesPath);
    fs::path posCos  = resolvePath(inputsDir_, "position_ids_cos.raw");
    fs::path posSin  = resolvePath(inputsDir_, "position_ids_sin.raw");
    fs::path winMask = resolvePath(inputsDir_, "window_attention_mask.raw");
    fs::path fullMask= resolvePath(inputsDir_, "full_attention_mask.raw");

    const std::string prefix = build_vision_prefix();
    const std::string suffix = build_vision_suffix(userPrompt);

    lutEnc_.setTextInput(prefix);
    imageEnc_.loadAndSetInputs(pv, posCos, posSin, winMask, fullMask);
    lutEnc_.appendTextInput(suffix);
}



std::string VlmService::resetSession(){

    auto start = std::chrono::steady_clock::now();
    std::string status="success";

    int rc = GeniePipeline_reset(pipeline_.pipeline_handle_);
    if (rc != 0) {
        std::cerr << "GeniePipeline_reset failed with code: " << rc << "\n";
        status="failed";
    }

    auto end = std::chrono::steady_clock::now();
    auto elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    std::cout << "GeniePipeline_reset took " << elapsed_us << " microseconds ("
              << elapsed_us / 1000.0 << " ms).\n";

    
    return status;
}

std::string VlmService::runText(const std::string& userPrompt,
                                bool reset_before_run,
                                bool followup_turn) {
    const auto t_total_start = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(m_);
    const auto t_prepare_start = std::chrono::steady_clock::now();

    if (reset_before_run) {
        resetPipelineOrThrow_("Failed to reset pipeline before runText");
    }
    clearStateForRun_(false);

    std::string finalPrompt = followup_turn
                                ? build_followup_text_prompt(userPrompt)
                                : build_initial_text_prompt(userPrompt);

    lutEnc_.setTextInput(finalPrompt);
    const double prepare_ms = round3(ms_since(t_prepare_start));
    
    const auto t_execute_start = std::chrono::steady_clock::now();
    pipeline_.execute(nullptr);
    const double execute_ms = round3(ms_since(t_execute_start));

    const auto t_wait_start = std::chrono::steady_clock::now();
    textGen_.waitForDoneOrIdle(240, 10000);
    const double wait_ms = round3(ms_since(t_wait_start));
    const auto t_collect_start = std::chrono::steady_clock::now();
    auto out = textGen_.getText();
    const double collect_ms = round3(ms_since(t_collect_start));
    const double total_ms = round3(ms_since(t_total_start));
    std::cout << "[I2T][timing] runText total_ms=" << total_ms
              << " reset_before_run=" << (reset_before_run ? 1 : 0)
              << " followup_turn=" << (followup_turn ? 1 : 0)
              << " prepare_ms=" << prepare_ms
              << " execute_ms=" << execute_ms
              << " wait_ms=" << wait_ms
              << " collect_ms=" << collect_ms << "\n";
    return out;
}


std::string VlmService::runVision(const fs::path& pixelValuesPath,
                                const std::string& userPrompt) {
    const auto t_total_start = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(m_);
    ScopedNpuLock npu_lock(npu_lock_file(), npu_lock_timeout_ms());
    const int max_attempts = vision_retry_attempts();
    const int backoff_ms = vision_retry_backoff_ms();
    std::string last_error;

    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        try {
            const auto t_prepare_start = std::chrono::steady_clock::now();

            resetPipelineOrThrow_("Failed to reset pipeline before runVision");

            // Clear previous output (important in server)
            clearStateForRun_(true);

            const auto t_load_inputs_start = std::chrono::steady_clock::now();
            prepareVisionInputs_(pixelValuesPath, userPrompt);
            const double load_inputs_ms = round3(ms_since(t_load_inputs_start));
            const double prepare_ms = round3(ms_since(t_prepare_start));

            const auto t_execute_start = std::chrono::steady_clock::now();
            pipeline_.execute(nullptr);
            const double execute_ms = round3(ms_since(t_execute_start));

            const auto t_wait_start = std::chrono::steady_clock::now();
            textGen_.waitForDoneOrIdle(240, 10000);
            const double wait_ms = round3(ms_since(t_wait_start));

            const auto t_release_start = std::chrono::steady_clock::now();
            imageEnc_.releaseInputs(true, false);
            const double release_ms = round3(ms_since(t_release_start));

            const auto t_collect_start = std::chrono::steady_clock::now();
            auto out = textGen_.getText();
            const double collect_ms = round3(ms_since(t_collect_start));
            const double total_ms = round3(ms_since(t_total_start));
            std::cout << "[I2T][timing] runVision total_ms=" << total_ms
                      << " attempts=" << attempt
                      << " prepare_ms=" << prepare_ms
                      << " load_inputs_ms=" << load_inputs_ms
                      << " execute_ms=" << execute_ms
                      << " wait_ms=" << wait_ms
                      << " release_inputs_ms=" << release_ms
                      << " collect_ms=" << collect_ms << "\n";
            return out;
        } catch (const std::exception& e) {
            last_error = e.what();
            try { imageEnc_.releaseInputs(true, false); } catch (...) {}
            const bool retryable = is_retryable_execute_status4(last_error);
            if (!retryable || attempt >= max_attempts) {
                throw;
            }
            std::cerr << "[I2T][retry] runVision transient execute failure (attempt "
                      << attempt << "/" << max_attempts << "): " << last_error
                      << " ; backing off " << backoff_ms << "ms\n";
            if (backoff_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
            }
        }
    }

    throw std::runtime_error(last_error.empty()
                                 ? "runVision failed after retries"
                                 : last_error);
}

std::string VlmService::runTextContinuous(const std::string userPrompt,
                                         OnDelta onDelta,
                                         OnDone onDone,
                                         OnError onError,
                                         bool reset_before_run,
                                         bool followup_turn) {
    const auto t_total_start = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lk(m_);
    try {
        const auto t_prepare_start = std::chrono::steady_clock::now();
        if (reset_before_run) {
            resetPipelineOrThrow_("Failed to reset pipeline before runTextContinuous");
        }
        clearStateForRun_(false);

        // IMPORTANT: make sure callback doesn't stop generation
        textGen_.setCallbackReturn(0);

        // Install per-request callback (if your TextGeneratorNode supports it)
        textGen_.setOnDelta(std::move(onDelta));

        std::string finalPrompt = followup_turn
                                    ? build_followup_text_prompt(userPrompt)
                                    : build_initial_text_prompt(userPrompt);
        lutEnc_.setTextInput(finalPrompt);
        const double prepare_ms = round3(ms_since(t_prepare_start));

        const auto t_execute_start = std::chrono::steady_clock::now();
        pipeline_.execute(nullptr);
        const double execute_ms = round3(ms_since(t_execute_start));

        const auto t_wait_start = std::chrono::steady_clock::now();
        textGen_.waitForDoneOrIdle(240, 10000);
        const double wait_ms = round3(ms_since(t_wait_start));

        textGen_.setOnDelta(nullptr);

        const auto t_collect_start = std::chrono::steady_clock::now();
        std::string finalText = textGen_.getText();
        const double collect_ms = round3(ms_since(t_collect_start));
        const double total_ms = round3(ms_since(t_total_start));
        std::cout << "[I2T][timing] runTextContinuous total_ms=" << total_ms
                  << " reset_before_run=" << (reset_before_run ? 1 : 0)
                  << " followup_turn=" << (followup_turn ? 1 : 0)
                  << " prepare_ms=" << prepare_ms
                  << " execute_ms=" << execute_ms
                  << " wait_ms=" << wait_ms
                  << " collect_ms=" << collect_ms << "\n";

        lk.unlock();
        if (onDone) onDone();
        return finalText;
    }
    catch (const std::exception& e) {
        // Clear callback, best effort
        textGen_.setOnDelta(nullptr);

        lk.unlock();
        if (onError) onError(e.what());
        return {};
    }
}

std::string VlmService::runVisionContinuous(const fs::path& pixelValuesPath,
                                           const std::string& userPrompt,
                                           OnDelta onDelta,
                                           OnDone onDone,
                                           OnError onError) {
    const auto t_total_start = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lk(m_);
    ScopedNpuLock npu_lock(npu_lock_file(), npu_lock_timeout_ms());
    try {
        textGen_.setCallbackReturn(0);
        textGen_.setOnDelta(std::move(onDelta));
        const int max_attempts = vision_retry_attempts();
        const int backoff_ms = vision_retry_backoff_ms();
        std::string finalText;
        std::string last_error;
        bool succeeded = false;

        for (int attempt = 1; attempt <= max_attempts; ++attempt) {
            try {
                const auto t_prepare_start = std::chrono::steady_clock::now();
                resetPipelineOrThrow_("Failed to reset pipeline before runVisionContinuous");
                clearStateForRun_(true);

                const auto t_load_inputs_start = std::chrono::steady_clock::now();
                prepareVisionInputs_(pixelValuesPath, userPrompt);
                const double load_inputs_ms = round3(ms_since(t_load_inputs_start));
                const double prepare_ms = round3(ms_since(t_prepare_start));

                const auto t_execute_start = std::chrono::steady_clock::now();
                pipeline_.execute(nullptr);
                const double execute_ms = round3(ms_since(t_execute_start));

                const auto t_wait_start = std::chrono::steady_clock::now();
                textGen_.waitForDoneOrIdle(240, 10000);
                const double wait_ms = round3(ms_since(t_wait_start));

                const auto t_release_start = std::chrono::steady_clock::now();
                imageEnc_.releaseInputs(true, false);
                const double release_ms = round3(ms_since(t_release_start));

                const auto t_collect_start = std::chrono::steady_clock::now();
                finalText = textGen_.getText();
                const double collect_ms = round3(ms_since(t_collect_start));
                const double total_ms = round3(ms_since(t_total_start));
                std::cout << "[I2T][timing] runVisionContinuous total_ms=" << total_ms
                          << " attempts=" << attempt
                          << " prepare_ms=" << prepare_ms
                          << " load_inputs_ms=" << load_inputs_ms
                          << " execute_ms=" << execute_ms
                          << " wait_ms=" << wait_ms
                          << " release_inputs_ms=" << release_ms
                          << " collect_ms=" << collect_ms << "\n";
                succeeded = true;
                break;
            } catch (const std::exception& e) {
                last_error = e.what();
                try { imageEnc_.releaseInputs(true, false); } catch (...) {}
                const bool retryable = is_retryable_execute_status4(last_error);
                if (!retryable || attempt >= max_attempts) {
                    throw;
                }
                std::cerr << "[I2T][retry] runVisionContinuous transient execute failure (attempt "
                          << attempt << "/" << max_attempts << "): " << last_error
                          << " ; backing off " << backoff_ms << "ms\n";
                if (backoff_ms > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
                }
            }
        }

        textGen_.setOnDelta(nullptr);
        if (!succeeded && !last_error.empty()) {
            throw std::runtime_error(last_error);
        }

        lk.unlock();
        if (onDone) onDone();
        return finalText;
    }
    catch (const std::exception& e) {
        try { imageEnc_.releaseInputs(true, false); } catch (...) {}

        textGen_.setOnDelta(nullptr);

        lk.unlock();
        if (onError) onError(e.what());
        return {};
    }
}
