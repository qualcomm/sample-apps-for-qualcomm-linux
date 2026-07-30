// =============================================================================
// Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// =============================================================================

#pragma once
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <atomic>

class StreamChannel {
public:
    void push(std::string s) {
        {
            std::lock_guard<std::mutex> lk(m_);
            q_.push_back(std::move(s));
        }
        cv_.notify_one();
    }

    // blocks until message, done, or cancelled
    bool pop(std::string& out) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return cancelled_ || done_ || !q_.empty(); });

        if (!q_.empty()) {
            out = std::move(q_.front());
            q_.pop_front();
            return true;
        }
        return false;
    }

    void done() {
        {
            std::lock_guard<std::mutex> lk(m_);
            done_ = true;
        }
        cv_.notify_all();
    }

    void cancel() {
        {
            std::lock_guard<std::mutex> lk(m_);
            cancelled_ = true;
        }
        cv_.notify_all();
    }

    bool is_cancelled() const { return cancelled_; }

private:
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::string> q_;
    bool done_ = false;
    bool cancelled_ = false;
};