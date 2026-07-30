// =============================================================================
// Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// =============================================================================

#pragma once
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <condition_variable>
#include <memory>
#include <stdexcept>

#include "VlmService.h"

namespace fs = std::filesystem;

class GenieWorker {
public:
    explicit GenieWorker(const fs::path& baseDir)
        : baseDir_(baseDir) {}

    ~GenieWorker() { stop(); }

    // Start the worker thread and block until the service is ready
    void start();

    // Stop the worker thread, join, and free resources
    void stop();

    // Convenience restart: fully stop and then start
    void restart();

    std::future<std::string> resetSession();

    // Submit a job; throws if not running
    std::future<std::string> submitVision(
        fs::path pixelValuesPath, 
        std::string prompt);
        
    std::future<std::string> submitText(std::string prompt);
    std::future<std::string> submitContinueText(std::string prompt);

    using OnDelta = TextGeneratorNode::OnDelta;
    using OnDone  = std::function<void()>;
    using OnError = std::function<void(const std::string&)>;

    std::future<std::string> submitrunTextContinuous(
        const std::string userPrompt,
        OnDelta onDelta,
        OnDone onDone,
        OnError onError);

    std::future<std::string> submitContinueTextContinuous(
        const std::string userPrompt,
        OnDelta onDelta,
        OnDone onDone,
        OnError onError);

    std::future<std::string> submitrunVisionContinuous(
        fs::path pixelValuesPath,
        const std::string userPrompt,
        OnDelta onDelta,
        OnDone onDone,
        OnError onError);

    // Atomically reset KV cache then run vision inference in a single queued job.
    // Prevents interleaving of reset and inference from concurrent requests.
    std::future<std::string> submitResetAndRunVisionContinuous(
        fs::path pixelValuesPath,
        const std::string userPrompt,
        OnDelta onDelta,
        OnDone onDone,
        OnError onError);

    // Atomically reset KV cache then run text inference in a single queued job.
    std::future<std::string> submitResetAndRunTextContinuous(
        const std::string userPrompt,
        OnDelta onDelta,
        OnDone onDone,
        OnError onError);

    bool isStarted() const;
    bool isReady() const;

private:
    void threadMain();

private:
    fs::path baseDir_;

    std::unique_ptr<VlmService> service_;

    std::thread worker_;
    mutable std::mutex m_;
    std::condition_variable cv_;       
    std::condition_variable init_cv_;  

    bool stop_   = false;
    bool started_ = false; 
    bool ready_   = false; 

    std::queue<std::packaged_task<std::string()>> jobs_;
};
