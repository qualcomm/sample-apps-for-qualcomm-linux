// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <chrono>
#include <sstream>
#include <thread>
#include <iostream>

enum class LogLevel {
    Trace, Debug, Info, Warn, Error, Fatal, Off
};

class Logger {
public:
    static Logger& instance();

    void setLevel(LogLevel level) noexcept;
    LogLevel level() const noexcept;

    void setFile(const std::string& path, bool append = true);
    void enableConsole(bool enable) noexcept;

    // Size-based rotation
    void rotateOnSize(std::size_t maxBytes, std::size_t maxBackups) noexcept;

    // Core logging
    void log(LogLevel level,
             const std::string& msg,
             const char* file,
             int line,
             const char* func);

    static const char* levelToString(LogLevel) noexcept;

private:
    Logger();
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void writeLineUnlocked(LogLevel level, const std::string& line);
    std::string makeTimestamp() const;
    std::string formatHeader(LogLevel level, const char* file, int line, const char* func);
    void rotateIfNeededUnlocked();

    mutable std::mutex mu_;
    std::ofstream ofs_;
    std::string filePath_;
    bool console_ = false;
    LogLevel currentLevel_ = LogLevel::Info;
    std::size_t rotateMaxBytes_ = 0;
    std::size_t rotateMaxBackups_ = 0;
};

// ---------- RAII logging line ----------
class LogLine {
public:
    LogLine(LogLevel level, const char* file, int line, const char* func)
        : level_(level), file_(file), line_(line), func_(func) {}

    ~LogLine() {
        // Emit on destruction
        Logger::instance().log(level_, oss_.str(), file_, line_, func_);
    }

    template <typename T>
    LogLine& operator<<(const T& v) {
        oss_ << v;
        return *this;
    }

    // Support for manipulators like std::endl
    using StreamManip = std::ostream& (*)(std::ostream&);
    LogLine& operator<<(StreamManip m) {
        m(oss_);
        return *this;
    }

private:
    std::ostringstream oss_;
    LogLevel level_;
    const char* file_;
    int line_;
    const char* func_;
};

// ---------- Project‑specific macros (no __VA_ARGS__) ----------
#define APP_LOG_TRACE() LogLine(LogLevel::Trace, __FILE__, __LINE__, __func__)
#define APP_LOG_DEBUG() LogLine(LogLevel::Debug, __FILE__, __LINE__, __func__)
#define APP_LOG_INFO()  LogLine(LogLevel::Info,  __FILE__, __LINE__, __func__)
#define APP_LOG_WARN()  LogLine(LogLevel::Warn,  __FILE__, __LINE__, __func__)
#define APP_LOG_ERROR() LogLine(LogLevel::Error, __FILE__, __LINE__, __func__)
#define APP_LOG_FATAL() LogLine(LogLevel::Fatal, __FILE__, __LINE__, __func__)

