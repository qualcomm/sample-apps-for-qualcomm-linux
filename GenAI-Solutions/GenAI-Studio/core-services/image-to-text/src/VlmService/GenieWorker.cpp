// =============================================================================
// Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// =============================================================================

#include "GenieWorker.h"

void GenieWorker::start() {
    std::unique_lock<std::mutex> lk(m_);
    if (started_) return;

    stop_   = false;
    ready_  = false;

    worker_ = std::thread([this]{ this->threadMain(); });
    started_ = true;

    // Wait until service_ is constructed and ready
    init_cv_.wait(lk, [this]{ return ready_ || stop_ == true; });

    if (!ready_) {
        lk.unlock();
        stop();
        throw std::runtime_error("GenieWorker failed to start (service not ready)");
    }
}

void GenieWorker::stop() {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!started_) {
            service_.reset();
            ready_ = false;
            return;
        }
        stop_ = true;
    }
    cv_.notify_all();

    if (worker_.joinable()) worker_.join();

    {
        std::lock_guard<std::mutex> lk(m_);
        while (!jobs_.empty()) {
            jobs_.pop();
        }

        // Free the service (frees GPU/engine resources)
        service_.reset();
        ready_   = false;
        started_ = false;
    }
}

void GenieWorker::restart() {
    stop();
    start();
}

std::future<std::string> GenieWorker::resetSession(){

    std::unique_lock<std::mutex> lk(m_);
        if (!started_ || !ready_ || stop_) {
            throw std::runtime_error("Worker not running or not ready");
        }

        std::packaged_task<std::string()> task([this] {
                return service_->resetSession();
            });

        auto fut = task.get_future();
        jobs_.push(std::move(task));
        lk.unlock();
        cv_.notify_one();
        return fut;
}

std::future<std::string> GenieWorker::submitVision(fs::path pixelValuesPath, std::string prompt) {
    std::unique_lock<std::mutex> lk(m_);
    if (!started_ || !ready_ || stop_) {
        throw std::runtime_error("Worker not running or not ready");
    }

    std::packaged_task<std::string()> task([this, pv = std::move(pixelValuesPath), p = std::move(prompt)] {
            return service_->runVision(pv, p);
        });

    auto fut = task.get_future();
    jobs_.push(std::move(task));
    lk.unlock();
    cv_.notify_one();
    return fut;
}

std::future<std::string> GenieWorker::submitText(std::string prompt) {
    std::unique_lock<std::mutex> lk(m_);
    if (!started_ || !ready_ || stop_) {
        throw std::runtime_error("Worker not running or not ready");
    }

    std::packaged_task<std::string()> task([this,p = std::move(prompt)] {
            return service_->runText(p, true, false);
        });

    auto fut = task.get_future();
    jobs_.push(std::move(task));
    lk.unlock();
    cv_.notify_one();
    return fut;
}

std::future<std::string> GenieWorker::submitContinueText(std::string prompt) {
    std::unique_lock<std::mutex> lk(m_);
    if (!started_ || !ready_ || stop_) {
        throw std::runtime_error("Worker not running or not ready");
    }

    std::packaged_task<std::string()> task([this,p = std::move(prompt)] {
            return service_->runText(p, false, true);
        });

    auto fut = task.get_future();
    jobs_.push(std::move(task));
    lk.unlock();
    cv_.notify_one();
    return fut;
}

std::future<std::string> GenieWorker::submitrunTextContinuous(const std::string prompt,
                                         OnDelta onDelta,
                                         OnDone onDone,
                                         OnError onError) {
    std::unique_lock<std::mutex> lk(m_);
    if (!started_ || !ready_ || stop_) {
        throw std::runtime_error("Worker not running or not ready");
    }

    std::packaged_task<std::string()> task([this,
        p = std::move(prompt),
        onDelta=std::move(onDelta),
        onDone=std::move(onDone),
        onError=std::move(onError)] {
            return service_->runTextContinuous(p, onDelta, onDone, onError, true, false);
        });

    auto fut = task.get_future();
    jobs_.push(std::move(task));
    lk.unlock();
    cv_.notify_one();
    return fut;
}

std::future<std::string> GenieWorker::submitContinueTextContinuous(
    const std::string prompt,
    OnDelta onDelta,
    OnDone onDone,
    OnError onError) {

    std::unique_lock<std::mutex> lk(m_);
    if (!started_ || !ready_ || stop_) {
        throw std::runtime_error("Worker not running or not ready");
    }

    std::packaged_task<std::string()> task([this,
        p = std::move(prompt),
        onDelta=std::move(onDelta),
        onDone=std::move(onDone),
        onError=std::move(onError)] {
            return service_->runTextContinuous(p, onDelta, onDone, onError, false, true);
        });

    auto fut = task.get_future();
    jobs_.push(std::move(task));
    lk.unlock();
    cv_.notify_one();
    return fut;
}

std::future<std::string> GenieWorker::submitrunVisionContinuous(
    fs::path pixelValuesPath,
    const std::string prompt,
    OnDelta onDelta,
    OnDone onDone,
    OnError onError) {

    std::unique_lock<std::mutex> lk(m_);
    if (!started_ || !ready_ || stop_) {
        throw std::runtime_error("Worker not running or not ready");
    }

    std::packaged_task<std::string()> task([this,
        pv=std::move(pixelValuesPath),
        p = std::move(prompt),
        onDelta=std::move(onDelta),
        onDone=std::move(onDone),
        onError=std::move(onError)] {
            return service_->runVisionContinuous(pv,p,onDelta,onDone,onError);
        });

    auto fut = task.get_future();
    jobs_.push(std::move(task));
    lk.unlock();
    cv_.notify_one();
    return fut;
}

std::future<std::string> GenieWorker::submitResetAndRunVisionContinuous(
    fs::path pixelValuesPath,
    const std::string userPrompt,
    OnDelta onDelta,
    OnDone onDone,
    OnError onError) {

    std::unique_lock<std::mutex> lk(m_);
    if (!started_ || !ready_ || stop_) {
        throw std::runtime_error("Worker not running or not ready");
    }

    // Single packaged_task: reset KV cache then run vision inference atomically.
    // This prevents another request from interleaving between reset and inference.
    std::packaged_task<std::string()> task([this,
        pv = std::move(pixelValuesPath),
        p  = std::move(userPrompt),
        onDelta = std::move(onDelta),
        onDone  = std::move(onDone),
        onError = std::move(onError)] {
            service_->resetSession();
            return service_->runVisionContinuous(pv, p, onDelta, onDone, onError);
        });

    auto fut = task.get_future();
    jobs_.push(std::move(task));
    lk.unlock();
    cv_.notify_one();
    return fut;
}

std::future<std::string> GenieWorker::submitResetAndRunTextContinuous(
    const std::string userPrompt,
    OnDelta onDelta,
    OnDone onDone,
    OnError onError) {

    std::unique_lock<std::mutex> lk(m_);
    if (!started_ || !ready_ || stop_) {
        throw std::runtime_error("Worker not running or not ready");
    }

    // Single packaged_task: reset KV cache then run text inference atomically.
    std::packaged_task<std::string()> task([this,
        p       = std::move(userPrompt),
        onDelta = std::move(onDelta),
        onDone  = std::move(onDone),
        onError = std::move(onError)] {
            service_->resetSession();
            return service_->runTextContinuous(p, onDelta, onDone, onError);
        });

    auto fut = task.get_future();
    jobs_.push(std::move(task));
    lk.unlock();
    cv_.notify_one();
    return fut;
}

bool GenieWorker::isStarted() const {
    std::lock_guard<std::mutex> lk(m_);
    return started_ && !stop_;
}

bool GenieWorker::isReady() const {
    std::lock_guard<std::mutex> lk(m_);
    return ready_;
}

void GenieWorker::threadMain() {
    // Construct VlmService INSIDE worker thread (thread-affine safe)
    {
        std::lock_guard<std::mutex> lk(m_);
        try {
            service_ = std::make_unique<VlmService>(baseDir_);
            ready_ = true;
        } catch (...) {
            ready_ = false;
        }
        init_cv_.notify_all();
    }

    for (;;) {
        std::packaged_task<std::string()> job;

        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [this]{ return stop_ || !jobs_.empty(); });

            if (stop_ && jobs_.empty()) break;

            job = std::move(jobs_.front());
            jobs_.pop();
        }

        // Execute outside lock
        try {
            job();
        } catch (...) {
        }
    }
}
