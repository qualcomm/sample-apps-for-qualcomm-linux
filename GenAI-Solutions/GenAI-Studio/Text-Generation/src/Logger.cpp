// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#include "Logger.hpp"
#include <iomanip>
#include <filesystem>

namespace fs = std::filesystem;

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

Logger::Logger() = default;

Logger::~Logger() {
    std::lock_guard<std::mutex> lock(mu_);
    if (ofs_.is_open()) ofs_.close();
}

void Logger::setLevel(LogLevel level) noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    currentLevel_ = level;
}

LogLevel Logger::level() const noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    return currentLevel_;
}

void Logger::setFile(const std::string& path, bool append) {
    std::lock_guard<std::mutex> lock(mu_);
    if (ofs_.is_open()) ofs_.close();
    filePath_ = path;

    std::ios::openmode mode = std::ios::out | (append ? std::ios::app : std::ios::trunc);
    ofs_.open(filePath_, mode);
    if (!ofs_) {
        console_ = true;
        std::cerr << "[Logger] Failed to open log file: " << filePath_ << std::endl;
    }
}

void Logger::enableConsole(bool enable) noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    console_ = enable;
}

void Logger::rotateOnSize(std::size_t maxBytes, std::size_t maxBackups) noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    rotateMaxBytes_ = maxBytes;
    rotateMaxBackups_ = maxBackups;
}

const char* Logger::levelToString(LogLevel lv) noexcept {
    switch (lv) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
        case LogLevel::Off:   return "OFF";
    }
    return "UNKNOWN";
}

std::string Logger::makeTimestamp() const {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto t   = system_clock::to_time_t(now);
    const auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.'
        << std::setw(3) << std::setfill('0') << ms.count();
    return oss.str();
}

std::string Logger::formatHeader(LogLevel level, const char* file, int line, const char* func) {
    std::ostringstream oss;
    oss << makeTimestamp()
        << " [" << levelToString(level) << "]"
        << " [tid:" << std::this_thread::get_id() << "]"
        << " [" << file << ":" << line << " " << func << "] ";
    return oss.str();
}

void Logger::rotateIfNeededUnlocked() {
    if (rotateMaxBytes_ == 0 || filePath_.empty()) return;

    std::error_code ec;
    const auto sz = fs::exists(filePath_, ec) ? fs::file_size(filePath_, ec) : std::uintmax_t(0);

    if (!ec && sz >= rotateMaxBytes_) {
        if (ofs_.is_open()) {
            ofs_.flush();
            ofs_.close();
        }

        for (std::size_t i = rotateMaxBackups_; i >= 1; --i) {
            const auto src = filePath_ + "." + std::to_string(i);
            const auto dst = filePath_ + "." + std::to_string(i + 1);
            if (fs::exists(src, ec)) {
                fs::remove(dst, ec);
                fs::rename(src, dst, ec);
            }
        }

        const auto first = filePath_ + ".1";
        fs::remove(first, ec);
        fs::rename(filePath_, first, ec);

        ofs_.open(filePath_, std::ios::out | std::ios::trunc);
        if (!ofs_) {
            std::cerr << "[Logger] Rotation reopen failed: " << filePath_ << std::endl;
            console_ = true;
        }
    }
}

void Logger::writeLineUnlocked(LogLevel level, const std::string& line) {
    rotateIfNeededUnlocked();

    if (ofs_.is_open()) {
        ofs_ << line << '\n';
        ofs_.flush();
    }

    if (console_) {
        if (level >= LogLevel::Error) {
            std::cerr << line << std::endl;
        } else {
            std::cout << line << std::endl;
        }
    }
}

void Logger::log(LogLevel level,
                 const std::string& msg,
                 const char* file,
                 int line,
                 const char* func) {
    std::lock_guard<std::mutex> lock(mu_);
    if (level < currentLevel_) return;

    const std::string header = formatHeader(level, file, line, func);
    writeLineUnlocked(level, header + msg);
}

