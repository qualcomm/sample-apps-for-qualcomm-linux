// =============================================================================
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// =============================================================================
//
// Image-To-Text SSE Server  (cpp-httplib, OpenAI-compatible)
// ===========================================================
//
// Endpoints:
//   GET  /health                         – liveness / readiness probe
//   GET  /v1/models                      – list available models
//   POST /v1/responses                   – OpenAI-style responses API
//   POST /v1/session/reset               – reset KV-cache / session state
//
// Usage:
//   ./image2text_sse [--base-dir <path>] [--port <port>]
// =============================================================================

// Explicitly disable SSL in cpp-httplib (must undef, not define-as-0,
// because httplib uses #ifdef CPPHTTPLIB_OPENSSL_SUPPORT)
#undef  CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "nlohmann/json.hpp"
#include "GenieWorker.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <sys/file.h>
#include <sys/wait.h>
#include <ctime>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using json   = nlohmann::json;

// =============================================================================
// Thread-safe token queue – bridges async GenieWorker callbacks → SSE sink
// =============================================================================
struct TokenQueue {
    mutable std::mutex      m;
    std::condition_variable cv;
    std::queue<std::string> tokens;
    bool        done{false};
    std::string error;
    std::chrono::steady_clock::time_point created_at{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point first_token_at{};
    std::chrono::steady_clock::time_point last_token_at{};
    size_t      token_count{0};
    bool        first_token_seen{false};

    void push(std::string_view sv) {
        {
            std::lock_guard<std::mutex> lk(m);
            const auto now = std::chrono::steady_clock::now();
            if (!first_token_seen) {
                first_token_seen = true;
                first_token_at = now;
            }
            last_token_at = now;
            ++token_count;
            tokens.emplace(sv);
        }
        cv.notify_one();
    }

    void finish() {
        { std::lock_guard<std::mutex> lk(m); done = true; }
        cv.notify_one();
    }

    void fail(const std::string& err) {
        { std::lock_guard<std::mutex> lk(m); error = err; done = true; }
        cv.notify_one();
    }

    // Returns true if a token was popped, false on timeout
    bool pop(std::string& out, int timeout_ms = 10000) {
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                    [this]{ return !tokens.empty() || done; });
        if (!tokens.empty()) {
            out = std::move(tokens.front());
            tokens.pop();
            return true;
        }
        return false;
    }

    bool is_done() const {
        std::lock_guard<std::mutex> lk(m);
        return done && tokens.empty();
    }

    bool has_error() const {
        std::lock_guard<std::mutex> lk(m);
        return !error.empty();
    }

    std::string get_error() const {
        std::lock_guard<std::mutex> lk(m);
        return error;
    }

    json timingJson() const {
        std::lock_guard<std::mutex> lk(m);
        auto r = [](double v) { return std::round(v * 1000.0) / 1000.0; };
        const auto now = std::chrono::steady_clock::now();
        const double total_ms = std::chrono::duration<double, std::milli>(now - created_at).count();
        double ttft_ms = 0.0;
        double generation_ms = 0.0;
        double tps = 0.0;
        if (first_token_seen) {
            ttft_ms = std::chrono::duration<double, std::milli>(first_token_at - created_at).count();
            generation_ms = std::chrono::duration<double, std::milli>(last_token_at - first_token_at).count();
            if (total_ms > 0.0) {
                tps = (static_cast<double>(token_count) * 1000.0) / total_ms;
            }
        }
        return json{
            {"tokens", static_cast<uint64_t>(token_count)},
            {"time_to_first_token_ms", r(ttft_ms)},
            {"generation_time_ms", r(generation_ms)},
            {"total_time_ms", r(total_ms)},
            {"tokens_per_second", r(tps)}
        };
    }
};

// =============================================================================
// Utilities
// =============================================================================
static std::string make_id() {
    auto ns = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::to_string(ns);
}

static int64_t unix_seconds() {
    return static_cast<int64_t>(std::time(nullptr));
}

static void add_cors(httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin",  "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Session-Id");
}

static constexpr const char* kDefaultSessionId = "__default__";
static std::atomic<int> g_inflight_responses{0};

struct InflightGuard {
    InflightGuard() { g_inflight_responses.fetch_add(1, std::memory_order_relaxed); }
    ~InflightGuard() { g_inflight_responses.fetch_sub(1, std::memory_order_relaxed); }
};

static std::string extract_session_id(const httplib::Request& req) {
    std::string sid = req.get_header_value("X-Session-Id");
    if (sid.empty()) sid = req.get_header_value("x-session-id");
    if (sid.empty()) sid = kDefaultSessionId;
    return sid;
}

static json session_conflict_body() {
    return {{"error", {
        {"message", "Active session is different. Reset current session or reuse the active session id."},
        {"type", "session_conflict"},
        {"code", "session_conflict"}
    }}};
}

static double round3(double v) {
    return std::round(v * 1000.0) / 1000.0;
}

static double elapsed_ms(const std::chrono::steady_clock::time_point& t0) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
}

struct PythonPreprocessResult {
    fs::path pixel_values_path;
    double preprocess_ms{0.0};
};

static std::string trim_copy(const std::string& input) {
    size_t start = 0;
    while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start]))) {
        ++start;
    }
    size_t end = input.size();
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
        --end;
    }
    return input.substr(start, end - start);
}

static std::string shell_quote(const std::string& value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

static std::string env_or_default(const char* key, const std::string& fallback) {
    const char* value = std::getenv(key);
    if (value && *value) return std::string(value);
    return fallback;
}

static int env_int_or_default(const char* key, int fallback) {
    const char* raw = std::getenv(key);
    if (!raw || !*raw) return fallback;
    try {
        return std::stoi(raw);
    } catch (...) {
        return fallback;
    }
}

static std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

static bool truthy(std::string value) {
    value = lower_copy(trim_copy(value));
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

static size_t i2t_max_prompt_chars() {
    const int configured = env_int_or_default("I2T_MAX_PROMPT_CHARS", 4096);
    return static_cast<size_t>(std::max(256, configured));
}

static std::string i2t_npu_lock_file() {
    return env_or_default("I2T_NPU_LOCK_FILE", "/opt/genai-lock/npu.lock");
}

static int i2t_npu_lock_timeout_ms() {
    return std::max(1000, env_int_or_default("I2T_NPU_LOCK_TIMEOUT_MS", 300000));
}

class ScopedNpuLock {
public:
    ScopedNpuLock(const std::string& lock_file, int timeout_ms) {
        const fs::path lock_path(lock_file);
        if (lock_path.has_parent_path()) {
            std::error_code ec;
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

static bool release_should_unload_worker(const httplib::Request& req) {
    bool unload = truthy(env_or_default("I2T_RELEASE_UNLOAD_DEFAULT", "0"));
    if (req.has_param("unload")) {
        unload = truthy(req.get_param_value("unload"));
    }
    std::string mode = req.get_header_value("X-I2T-Release-Mode");
    if (mode.empty()) {
        mode = req.get_header_value("x-i2t-release-mode");
    }
    mode = lower_copy(trim_copy(mode));
    if (mode == "unload" || mode == "hard" || mode == "stop") {
        unload = true;
    } else if (mode == "soft" || mode == "reset") {
        unload = false;
    }
    return unload;
}

static bool preprocess_image_with_python(const fs::path& base_dir,
                                         const std::string& image_source,
                                         PythonPreprocessResult& out,
                                         std::string& err) {
    if (image_source.empty()) {
        err = "image_url is empty";
        return false;
    }

    const std::string script =
        env_or_default("I2T_PREPROCESS_SCRIPT", "/opt/i2t/preprocess.py");
    const std::string python_exec =
        env_or_default("I2T_PREPROCESS_PYTHON", "python3");
    const std::string model_hint =
        env_or_default("I2T_PREPROCESSOR_MODEL", env_or_default("I2T_MODEL_ID", ""));

    if (!fs::exists(script)) {
        err = "preprocess script not found: " + script;
        return false;
    }

    const fs::path output_dir = base_dir / "uploads" / ("preprocess_" + make_id());
    std::error_code mkerr;
    fs::create_directories(output_dir, mkerr);
    if (mkerr) {
        err = "failed to create output dir: " + output_dir.string();
        return false;
    }

    std::string python_image_source = image_source;
    if (image_source.rfind("data:", 0) == 0) {
        const fs::path data_url_file = output_dir / "image_source.dataurl.txt";
        std::ofstream ofs(data_url_file, std::ios::binary);
        if (!ofs) {
            err = "failed to write temp data URL file: " + data_url_file.string();
            return false;
        }
        ofs << image_source;
        if (!ofs.good()) {
            err = "failed to persist data URL payload: " + data_url_file.string();
            return false;
        }
        python_image_source = data_url_file.string();
    }

    std::string cmd = shell_quote(python_exec) + " " +
                      shell_quote(script) + " " +
                      shell_quote(python_image_source) + " " +
                      shell_quote(output_dir.string());
    if (!model_hint.empty()) {
        cmd += " " + shell_quote(model_hint);
    }
    cmd += " 2>&1";

    const auto t_preprocess_start = std::chrono::steady_clock::now();
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        err = "failed to launch python preprocessing command";
        return false;
    }

    std::string output;
    std::array<char, 512> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }

    const int rc = pclose(pipe);
    int exit_code = rc;
    if (rc != -1 && WIFEXITED(rc)) {
        exit_code = WEXITSTATUS(rc);
    }
    if (rc == -1 || exit_code != 0) {
        err = "python preprocess failed: " + trim_copy(output);
        return false;
    }

    std::istringstream stream(output);
    std::string line;
    std::string last_non_empty;
    while (std::getline(stream, line)) {
        const std::string trimmed = trim_copy(line);
        if (!trimmed.empty()) last_non_empty = trimmed;
    }
    if (last_non_empty.empty()) {
        err = "python preprocess returned no output path";
        return false;
    }

    fs::path pixel_values_path = fs::path(last_non_empty);
    if (!pixel_values_path.is_absolute()) {
        pixel_values_path = output_dir / pixel_values_path;
    }
    if (!fs::exists(pixel_values_path)) {
        err = "pixel_values output not found: " + pixel_values_path.string();
        return false;
    }

    out.pixel_values_path = fs::weakly_canonical(pixel_values_path);
    out.preprocess_ms = round3(elapsed_ms(t_preprocess_start));
    return true;
}

// Extract the effective prompt from an OpenAI-style messages array.
struct ParsedMessages {
    std::string prompt;
    std::string latest_user_prompt;
    std::string image_url;
};

static void append_with_newline(std::string& dst, const std::string& value) {
    if (value.empty()) return;
    if (!dst.empty()) dst += "\n";
    dst += value;
}

static void maybe_capture_image_url_part(const json& part, std::string& image_url) {
    if (!part.is_object()) return;
    const std::string type = part.value("type", "");
    if (type != "image_url" || !part.contains("image_url")) return;

    if (part["image_url"].is_object() &&
        part["image_url"].contains("url") &&
        part["image_url"]["url"].is_string()) {
        image_url = part["image_url"]["url"].get<std::string>();
    } else if (part["image_url"].is_string()) {
        image_url = part["image_url"].get<std::string>();
    }
}

static void maybe_append_text_part(const json& part, std::string& content_text) {
    if (!part.is_object()) return;
    const std::string type = part.value("type", "");
    if (type == "text" && part.contains("text") && part["text"].is_string()) {
        content_text += part["text"].get<std::string>();
    }
}

static std::string extract_content_text(const json& message, std::string& image_url) {
    std::string content_text;
    if (message.contains("content") && message["content"].is_string()) {
        content_text = message["content"].get<std::string>();
        return content_text;
    }
    if (message.contains("content") && message["content"].is_array()) {
        for (const auto& part : message["content"]) {
            maybe_append_text_part(part, content_text);
            maybe_capture_image_url_part(part, image_url);
        }
    }
    return content_text;
}

static ParsedMessages parse_messages(const json& messages) {
    ParsedMessages parsed;
    std::string sys_ctx;
    std::string user_msg;
    for (const auto& m : messages) {
        if (!m.is_object()) continue;
        const std::string role = m.value("role", "");
        std::string content_text = extract_content_text(m, parsed.image_url);
        if (role == "system" || role == "developer") {
            append_with_newline(sys_ctx, content_text);
        } else if (role == "user") {
            if (!content_text.empty()) {
                parsed.latest_user_prompt = content_text;
                append_with_newline(user_msg, content_text);
            }
        }
    }
    if (!sys_ctx.empty() && !user_msg.empty()) parsed.prompt = sys_ctx + "\n\n" + user_msg;
    else parsed.prompt = user_msg.empty() ? sys_ctx : user_msg;
    return parsed;
}

static bool starts_with(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() &&
           text.compare(0, prefix.size(), prefix) == 0;
}

static bool is_supported_image_source(const std::string& image_url) {
    return starts_with(image_url, "https://") || starts_with(image_url, "data:");
}

static bool normalize_responses_content_part(const json& in_part,
                                             json& out_part,
                                             std::string& err) {
    if (!in_part.is_object()) {
        err = "input[].content[] entries must be objects";
        return false;
    }

    const std::string type = in_part.value("type", "");
    if (type == "input_text" || type == "text") {
        if (!in_part.contains("text") || !in_part["text"].is_string()) {
            err = "input_text requires string 'text'";
            return false;
        }
        out_part = {
            {"type", "text"},
            {"text", in_part["text"].get<std::string>()}
        };
        return true;
    }

    if (type == "input_image" || type == "image_url") {
        std::string image_url;
        if (in_part.contains("image_url") && in_part["image_url"].is_string()) {
            image_url = in_part["image_url"].get<std::string>();
        } else if (in_part.contains("image_url") &&
                   in_part["image_url"].is_object() &&
                   in_part["image_url"].contains("url") &&
                   in_part["image_url"]["url"].is_string()) {
            image_url = in_part["image_url"]["url"].get<std::string>();
        } else {
            err = "input_image requires string 'image_url' or object 'image_url.url'";
            return false;
        }
        out_part = {
            {"type", "image_url"},
            {"image_url", {{"url", image_url}}}
        };
        return true;
    }

    err = "unsupported input content type: " + type;
    return false;
}

static bool parse_responses_input_to_messages(const json& input,
                                              json& messages_out,
                                              std::string& err) {
    if (!input.is_array() || input.empty()) {
        err = "'input' must be a non-empty array";
        return false;
    }

    messages_out = json::array();
    for (const auto& item : input) {
        if (!item.is_object()) {
            err = "input[] entries must be objects";
            return false;
        }

        std::string role = item.value("role", "user");
        if (role.empty()) role = "user";

        if (!item.contains("content")) {
            err = "input[] requires 'content' array";
            return false;
        }

        json msg = {
            {"role", role},
            {"content", json::array()}
        };

        if (item["content"].is_array()) {
            for (const auto& part : item["content"]) {
                json normalized;
                std::string part_err;
                if (!normalize_responses_content_part(part, normalized, part_err)) {
                    err = part_err;
                    return false;
                }
                msg["content"].push_back(normalized);
            }
        } else {
            err = "input[].content must be array";
            return false;
        }

        messages_out.push_back(msg);
    }

    return true;
}

// Build an OpenAI-compatible SSE delta chunk
static json delta_chunk(const std::string& id, const std::string& model,
                         const std::string& content, int64_t created) {
    return {
        {"id",     id},
        {"object", "chat.completion.chunk"},
        {"model",  model},
        {"created", created},
        {"choices", {{
            {"index",         0},
            {"delta",         {{"content", content}}},
            {"finish_reason", nullptr}
        }}}
    };
}

static json role_chunk(const std::string& id, const std::string& model, int64_t created) {
    return {
        {"id",     id},
        {"object", "chat.completion.chunk"},
        {"model",  model},
        {"created", created},
        {"choices", {{
            {"index",         0},
            {"delta",         {{"role", "assistant"}, {"content", ""}}},
            {"finish_reason", nullptr}
        }}}
    };
}

static json stop_chunk(const std::string& id, const std::string& model, int64_t created) {
    return {
        {"id",     id},
        {"object", "chat.completion.chunk"},
        {"model",  model},
        {"created", created},
        {"choices", {{
            {"index",         0},
            {"delta",         json::object()},
            {"finish_reason", "stop"}
        }}}
    };
}

static json error_body(const std::string& msg,
                        const std::string& type = "server_error") {
    return {{"error", {{"message", msg}, {"type", type}, {"code", type}}}};
}

static bool contains_ci(const std::string& text, const std::string& needle) {
    if (needle.empty()) return true;
    if (needle.size() > text.size()) return false;
    for (size_t i = 0; i + needle.size() <= text.size(); ++i) {
        size_t j = 0;
        while (j < needle.size()) {
            const unsigned char tc = static_cast<unsigned char>(text[i + j]);
            const unsigned char nc = static_cast<unsigned char>(needle[j]);
            if (std::tolower(tc) != std::tolower(nc)) break;
            ++j;
        }
        if (j == needle.size()) return true;
    }
    return false;
}

static bool is_context_length_error(const std::string& message) {
    return contains_ci(message, "context") ||
           contains_ci(message, "text_encoder_text_input") ||
           contains_ci(message, "genienode_setdata") ||
           contains_ci(message, "status=-1");
}

static bool is_transient_busy_error(const std::string& message) {
    if (contains_ci(message, "npu_lock_timeout") ||
        contains_ci(message, "npu lock timeout")) {
        return true;
    }
    return contains_ci(message, "geniepipeline_execute") &&
           contains_ci(message, "status=4");
}

static bool should_recover_worker_on_error(const std::string& message) {
    // Context-related failures frequently leave the pipeline in a bad state for
    // subsequent requests; reset/recover immediately.
    if (is_context_length_error(message)) return true;
    if (contains_ci(message, "geniepipeline_execute") &&
        contains_ci(message, "status=4")) {
        return true;
    }
    return !is_transient_busy_error(message);
}

static bool recover_restart_enabled() {
    const char* raw = std::getenv("I2T_RECOVER_RESTART");
    if (!raw || !*raw) return true;
    return truthy(raw);
}

class SessionRuntime;
static void best_effort_session_recover(GenieWorker& worker, SessionRuntime& sessions);
static void set_inference_error_response(httplib::Response& res, const std::string& message);

static void set_method_not_allowed(httplib::Response& res, const std::string& allowed) {
    add_cors(res);
    res.status = 405;
    res.set_header("Allow", allowed);
    res.set_content(
        error_body("Method not allowed", "method_not_allowed").dump(),
        "application/json");
}

// =============================================================================
// Attach SSE stream to response – drains token queue
// NOTE: sink.done() is called before returning false to properly terminate
//       the HTTP chunked transfer encoding (avoids RemoteProtocolError).
// =============================================================================
static void attach_sse_stream(httplib::Response&          res,
                               std::shared_ptr<TokenQueue> queue,
                               const std::string&          cid,
                               const std::string&          model) {
    res.set_header("Cache-Control",    "no-cache");
    res.set_header("X-Accel-Buffering","no");
    add_cors(res);

    auto sent_role = std::make_shared<bool>(false);
    const int64_t created = unix_seconds();

    res.set_chunked_content_provider(
        "text/event-stream",
        [queue, cid, model, sent_role, created](size_t /*offset*/, httplib::DataSink& sink) -> bool {
            // ── Error path ────────────────────────────────────────────────
            if (queue->has_error()) {
                std::string ev = "data: " +
                    error_body(queue->get_error()).dump() + "\n\n";
                sink.write(ev.c_str(), ev.size());
                sink.write("data: [DONE]\n\n", 14);
                sink.done();
                return false;
            }

            // ── First assistant role chunk (OpenAI-compatible) ───────────
            if (!*sent_role) {
                *sent_role = true;
                std::string ev = "data: " +
                    role_chunk(cid, model, created).dump() + "\n\n";
                if (!sink.write(ev.c_str(), ev.size()))
                    return false;
                return true;
            }

            // ── Token available ───────────────────────────────────────────
            std::string token;
            if (queue->pop(token, 10000)) {
                std::string ev = "data: " +
                    delta_chunk(cid, model, token, created).dump() + "\n\n";
                if (!sink.write(ev.c_str(), ev.size()))
                    return false;   // client disconnected
                return true;
            }

            // ── Generation complete ───────────────────────────────────────
            if (queue->is_done()) {
                std::string timing_ev = "data: " + json{
                    {"type", "x_timing"},
                    {"timing", queue->timingJson()}
                }.dump() + "\n\n";
                sink.write(timing_ev.c_str(), timing_ev.size());
                std::string ev = "data: " + stop_chunk(cid, model, created).dump() + "\n\n";
                sink.write(ev.c_str(), ev.size());
                sink.write("data: [DONE]\n\n", 14);
                sink.done();        // ← properly terminates chunked transfer
                return false;
            }

            // ── Keepalive (timeout waiting for token) ─────────────────────
            return sink.write(": keepalive\n\n", 13);
        }
    );
}

static json responses_completed_body(const std::string& id,
                                     const std::string& model,
                                     const std::string& output_text,
                                     int64_t created_at,
                                     const json& timing) {
    return {
        {"id", id},
        {"object", "response"},
        {"created_at", created_at},
        {"status", "completed"},
        {"model", model},
        {"output", {{
            {"id", "msg_" + id},
            {"type", "message"},
            {"role", "assistant"},
            {"content", {{
                {"type", "output_text"},
                {"text", output_text},
                {"annotations", json::array()}
            }}}
        }}},
        {"output_text", output_text},
        {"usage", {
            {"input_tokens", 0},
            {"output_tokens", 0},
            {"total_tokens", 0}
        }},
        {"x_timing", timing}
    };
}

static void attach_responses_stream(httplib::Response& res,
                                    std::shared_ptr<TokenQueue> queue,
                                    const std::string& rid,
                                    const std::string& model,
                                    int64_t created,
                                    double parse_json_ms,
                                    double image_preprocess_ms,
                                    const std::chrono::steady_clock::time_point& t_req_start) {
    res.set_header("Cache-Control", "no-cache");
    res.set_header("X-Accel-Buffering", "no");
    add_cors(res);

    auto sent_created = std::make_shared<bool>(false);
    auto accumulated = std::make_shared<std::string>();

    res.set_chunked_content_provider(
        "text/event-stream",
        [queue,
         rid,
         model,
         created,
         parse_json_ms,
         image_preprocess_ms,
         t_req_start,
         sent_created,
         accumulated](size_t /*offset*/, httplib::DataSink& sink) -> bool {
            if (queue->has_error()) {
                json err = {
                    {"type", "response.error"},
                    {"response_id", rid},
                    {"error", {
                        {"message", queue->get_error()},
                        {"type", "server_error"},
                        {"code", "server_error"}
                    }}
                };
                const std::string ev = "event: response.error\ndata: " + err.dump() + "\n\n";
                sink.write(ev.c_str(), ev.size());
                sink.write("event: done\ndata: [DONE]\n\n", 27);
                sink.done();
                return false;
            }

            if (!*sent_created) {
                *sent_created = true;
                json created_ev = {
                    {"type", "response.created"},
                    {"response", {
                        {"id", rid},
                        {"object", "response"},
                        {"created_at", created},
                        {"model", model},
                        {"status", "in_progress"}
                    }}
                };
                const std::string ev = "event: response.created\ndata: " + created_ev.dump() + "\n\n";
                if (!sink.write(ev.c_str(), ev.size())) return false;
                return true;
            }

            std::string token;
            if (queue->pop(token, 10000)) {
                *accumulated += token;
                json delta_ev = {
                    {"type", "response.output_text.delta"},
                    {"response_id", rid},
                    {"delta", token}
                };
                const std::string ev =
                    "event: response.output_text.delta\ndata: " + delta_ev.dump() + "\n\n";
                if (!sink.write(ev.c_str(), ev.size())) return false;
                return true;
            }

            if (queue->is_done()) {
                json timing = queue->timingJson();
                timing["parse_json_ms"] = parse_json_ms;
                timing["request_total_ms"] = round3(elapsed_ms(t_req_start));
                if (image_preprocess_ms > 0.0) {
                    timing["image_preprocess_ms"] = image_preprocess_ms;
                }
                json completed = {
                    {"type", "response.completed"},
                    {"response", responses_completed_body(
                        rid, model, *accumulated, created, timing)}
                };
                const std::string ev = "event: response.completed\ndata: " +
                                       completed.dump() + "\n\n";
                sink.write(ev.c_str(), ev.size());
                sink.write("event: done\ndata: [DONE]\n\n", 27);
                sink.done();
                return false;
            }

            return sink.write(": keepalive\n\n", 13);
        });
}

// =============================================================================
// Server runtime state and route registration
// =============================================================================
struct CliOptions {
    fs::path base_dir{fs::current_path()};
    int port{8080};
};

static CliOptions parse_cli_options(int argc, char** argv) {
    CliOptions opts;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "--base-dir" || a == "-b") && i + 1 < argc) {
            opts.base_dir = fs::path(argv[++i]);
        } else if ((a == "--port" || a == "-p") && i + 1 < argc) {
            opts.port = std::stoi(argv[++i]);
        } else if (!a.empty() && a[0] != '-') {
            opts.base_dir = fs::path(a);
        }
    }
    opts.base_dir = fs::weakly_canonical(opts.base_dir);
    return opts;
}

class SessionRuntime {
public:
    std::string active_session_snapshot() const {
        std::lock_guard<std::mutex> lk(mu_);
        return active_session_id_;
    }

    bool try_activate_or_validate_session(const std::string& sid) {
        std::lock_guard<std::mutex> lk(mu_);
        if (active_session_id_.empty()) {
            active_session_id_ = sid;
            active_session_has_vision_ = false;
            return true;
        }
        return active_session_id_ == sid;
    }

    bool is_session_conflict(const std::string& sid) const {
        std::lock_guard<std::mutex> lk(mu_);
        return !active_session_id_.empty() && active_session_id_ != sid;
    }

    bool is_active_visual_session(const std::string& sid) const {
        if (sid.empty()) return false;
        std::lock_guard<std::mutex> lk(mu_);
        return active_session_has_vision_ && active_session_id_ == sid;
    }

    bool requires_vision_transition_reset(const std::string& sid) const {
        if (sid.empty()) return false;
        std::lock_guard<std::mutex> lk(mu_);
        return active_session_id_ == sid && !active_session_has_vision_;
    }

    void mark_active_session(const std::string& sid, bool has_vision) {
        std::lock_guard<std::mutex> lk(mu_);
        if (sid.empty()) {
            active_session_id_.clear();
            active_session_has_vision_ = false;
            return;
        }
        active_session_id_ = sid;
        active_session_has_vision_ = has_vision;
    }

private:
    mutable std::mutex mu_;
    std::string active_session_id_;
    bool active_session_has_vision_{false};
};

static void best_effort_session_recover(GenieWorker& worker, SessionRuntime& sessions) {
    bool restart_requested = recover_restart_enabled();
    try {
        auto fut = worker.resetSession();
        fut.get();
        if (restart_requested) {
            worker.restart();
        }
    } catch (const std::exception& e) {
        std::cerr << "[SSE] session reset failed during recover: " << e.what() << "\n";
        if (restart_requested) {
            try {
                worker.restart();
            } catch (const std::exception& re) {
                std::cerr << "[SSE] worker restart failed during recover: " << re.what() << "\n";
            } catch (...) {}
        }
    } catch (...) {
        if (restart_requested) {
            try { worker.restart(); } catch (...) {}
        }
    }
    sessions.mark_active_session("", false);
}

static bool ensure_vision_transition_ready(GenieWorker& worker,
                                           SessionRuntime& sessions,
                                           const std::string& session_id,
                                           httplib::Response& res) {
    if (!sessions.requires_vision_transition_reset(session_id)) {
        return true;
    }

    try {
        auto fut = worker.resetSession();
        fut.get();
        sessions.mark_active_session(session_id, false);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[SSE] vision transition reset error: " << e.what() << "\n";
        best_effort_session_recover(worker, sessions);
        set_inference_error_response(res, e.what());
        return false;
    }
}

static void set_inference_error_response(httplib::Response& res, const std::string& message) {
    if (is_context_length_error(message)) {
        res.status = 400;
        res.set_content(
            error_body(
                "Input exceeds model context window. Reduce input text length and retry.",
                "context_length_exceeded"
            ).dump(),
            "application/json");
        return;
    }
    if (is_transient_busy_error(message)) {
        res.status = 503;
        res.set_header("Retry-After", "2");
        res.set_content(
            error_body(
                "Inference backend is temporarily busy. Retry the request shortly.",
                "upstream_busy"
            ).dump(),
            "application/json");
        return;
    }
    res.status = 500;
    res.set_content(error_body(message).dump(), "application/json");
}

static void configure_server(httplib::Server& svr) {
    svr.set_read_timeout(600);
    svr.set_write_timeout(600);
    svr.set_idle_interval(0, 100'000);

    svr.Options(".*", [](const httplib::Request&, httplib::Response& res) {
        add_cors(res);
        res.status = 204;
    });
}

static bool ensure_worker_ready(GenieWorker& worker, httplib::Response& res) {
    if (worker.isReady()) return true;
    try {
        ScopedNpuLock lk(i2t_npu_lock_file(), i2t_npu_lock_timeout_ms());
        if (!worker.isReady()) {
            worker.start();
        }
    } catch (const std::exception& e) {
        add_cors(res);
        const std::string err = e.what();
        if (is_transient_busy_error(err) || contains_ci(err, "npu_lock_")) {
            res.status = 503;
            res.set_header("Retry-After", "2");
            res.set_content(
                error_body(
                    "Inference backend is temporarily busy. Retry the request shortly.",
                    "upstream_busy"
                ).dump(),
                "application/json");
            return false;
        }
        res.status = 500;
        res.set_content(
            error_body(std::string("Failed to initialize inference worker: ") + err).dump(),
            "application/json");
        return false;
    }
    if (!worker.isReady()) {
        add_cors(res);
        res.status = 500;
        res.set_content(
            error_body("Inference worker is not ready after start").dump(),
            "application/json");
        return false;
    }
    return true;
}

static bool is_default_session(const std::string& sid) {
    return sid == kDefaultSessionId;
}

static bool ensure_session_access(GenieWorker& worker,
                                  SessionRuntime& sessions,
                                  const std::string& sid,
                                  httplib::Response& res) {
    const std::string active_sid = sessions.active_session_snapshot();
    if (active_sid.empty() || active_sid == sid) return true;

    const bool allow_soft_switch = is_default_session(active_sid) || is_default_session(sid);
    if (!allow_soft_switch) {
        res.status = 409;
        res.set_header("X-Active-Session-Id", active_sid);
        res.set_content(session_conflict_body().dump(), "application/json");
        return false;
    }

    try {
        auto fut = worker.resetSession();
        fut.get();
    } catch (const std::exception& e) {
        res.status = 500;
        res.set_content(error_body(e.what()).dump(), "application/json");
        return false;
    }
    sessions.mark_active_session(sid, false);
    return true;
}

static void register_health_route(httplib::Server& svr,
                                  GenieWorker& worker,
                                  const std::string& model) {
    svr.Get("/health", [&worker, model](const httplib::Request&, httplib::Response& res) {
        add_cors(res);
        json body = {
            {"status", "ok"},
            {"model",  model},
            {"ready",  worker.isReady()}
        };
        res.set_content(body.dump(), "application/json");
    });
}

static void register_models_route(httplib::Server& svr, const std::string& model) {
    svr.Get("/v1/models", [model](const httplib::Request&, httplib::Response& res) {
        add_cors(res);
        json body = {
            {"object", "list"},
            {"data", {{
                {"id",           model},
                {"object",       "model"},
                {"owned_by",     "local"},
                {"capabilities", {"text", "vision"}}
            }}}
        };
        res.set_content(body.dump(), "application/json");
    });
}

static void register_session_reset_route(httplib::Server& svr,
                                         GenieWorker& worker,
                                         SessionRuntime& sessions) {
    svr.Post("/v1/session/reset", [&](const httplib::Request& req, httplib::Response& res) {
        const auto t_req_start = std::chrono::steady_clock::now();
        add_cors(res);
        if (!ensure_worker_ready(worker, res)) return;

        try {
            const auto t_worker_start = std::chrono::steady_clock::now();
            auto fut = worker.resetSession();
            fut.get();
            sessions.mark_active_session("", false);
            const double worker_ms = round3(elapsed_ms(t_worker_start));
            json body = {
                {"status", "ok"},
                {"message", "Session reset successfully"},
                {"x_timing", {
                    {"worker_reset_ms", worker_ms},
                    {"request_total_ms", round3(elapsed_ms(t_req_start))}
                }}
            };
            res.set_header("X-Worker-Reset-Ms", std::to_string(worker_ms));
            res.set_header("X-Request-Total-Ms", std::to_string(round3(elapsed_ms(t_req_start))));
            res.set_content(body.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(error_body(e.what()).dump(), "application/json");
        }
    });
}

static void register_session_release_route(httplib::Server& svr,
                                           GenieWorker& worker,
                                           SessionRuntime& sessions) {
    svr.Post("/v1/session/release", [&](const httplib::Request& req, httplib::Response& res) {
        add_cors(res);
        sessions.mark_active_session("", false);
        const bool unload_worker = release_should_unload_worker(req);
        if (unload_worker && g_inflight_responses.load(std::memory_order_relaxed) > 0) {
            res.status = 503;
            res.set_header("Retry-After", "2");
            res.set_content(
                error_body(
                    "Inference backend is temporarily busy. Retry the request shortly.",
                    "upstream_busy"
                ).dump(),
                "application/json");
            return;
        }
        try {
            if (unload_worker) {
                worker.stop();
            } else if (worker.isReady()) {
                auto fut = worker.resetSession();
                fut.get();
            }
            res.set_content(
                json{
                    {"status", "ok"},
                    {"message", unload_worker
                                    ? "Inference session released and worker unloaded"
                                    : "Inference session released"},
                    {"worker_unloaded", unload_worker}
                }.dump(),
                "application/json");
        } catch (const std::exception& e) {
            if (is_transient_busy_error(e.what())) {
                res.status = 503;
                res.set_header("Retry-After", "2");
                res.set_content(
                    error_body(
                        "Inference backend is temporarily busy. Retry the request shortly.",
                        "upstream_busy"
                    ).dump(),
                    "application/json");
            } else {
                res.status = 500;
                res.set_content(error_body(e.what()).dump(), "application/json");
            }
        }
    });
}

static void register_responses_route(httplib::Server& svr,
                                     const fs::path& baseDir,
                                     GenieWorker& worker,
                                     SessionRuntime& sessions,
                                     const std::string& model) {
    svr.Get("/v1/responses", [](const httplib::Request&, httplib::Response& res) {
        set_method_not_allowed(res, "POST, OPTIONS");
    });
    svr.Put("/v1/responses", [](const httplib::Request&, httplib::Response& res) {
        set_method_not_allowed(res, "POST, OPTIONS");
    });

    svr.Post("/v1/responses", [&](const httplib::Request& req,
                                  httplib::Response& res) {
        const auto t_req_start = std::chrono::steady_clock::now();
        add_cors(res);
        InflightGuard inflight_guard;
        if (!ensure_worker_ready(worker, res)) return;

        json body;
        const auto t_parse_start = std::chrono::steady_clock::now();
        double parse_json_ms = 0.0;
        try { body = json::parse(req.body); }
        catch (...) {
            res.status = 400;
            res.set_content(error_body("Invalid JSON", "invalid_request_error").dump(),
                            "application/json");
            return;
        }
        parse_json_ms = round3(elapsed_ms(t_parse_start));

        if (!body.contains("input")) {
            res.status = 400;
            res.set_content(error_body("'input' array is required",
                                       "invalid_request_error").dump(),
                            "application/json");
            return;
        }

        json messages;
        std::string parse_err;
        if (!parse_responses_input_to_messages(body["input"], messages, parse_err)) {
            res.status = 400;
            res.set_content(error_body(parse_err, "invalid_request_error").dump(),
                            "application/json");
            return;
        }

        const std::string session_id = extract_session_id(req);
        if (!ensure_session_access(worker, sessions, session_id, res)) return;

        const ParsedMessages parsed = parse_messages(messages);
        bool stream = true;
        if (body.contains("stream")) {
            if (!body["stream"].is_boolean()) {
                res.status = 400;
                res.set_content(
                    error_body("'stream' must be a boolean",
                               "invalid_request_error").dump(),
                    "application/json");
                return;
            }
            stream = body["stream"].get<bool>();
        }

        if (body.contains("max_output_tokens")) {
            if (!body["max_output_tokens"].is_number_integer()) {
                res.status = 400;
                res.set_content(
                    error_body("'max_output_tokens' must be an integer",
                               "invalid_request_error").dump(),
                    "application/json");
                return;
            }
            const int max_tokens = body["max_output_tokens"].get<int>();
            if (max_tokens < 1 || max_tokens > 8192) {
                res.status = 400;
                res.set_content(
                    error_body("'max_output_tokens' must be between 1 and 8192",
                               "invalid_request_error").dump(),
                    "application/json");
                return;
            }
        }
        std::string rid = "resp-" + make_id();
        const int64_t created = unix_seconds();

        if (!parsed.image_url.empty() && !is_supported_image_source(parsed.image_url)) {
            res.status = 400;
            res.set_content(
                error_body("Only https:// and data: image_url sources are supported",
                           "invalid_request_error").dump(),
                "application/json");
            return;
        }

        if (body.contains("pixel_values_path")) {
            res.status = 400;
            res.set_content(
                error_body("'pixel_values_path' is not supported in /v1/responses. "
                           "Send image via input[].content[].input_image.image_url",
                           "invalid_request_error").dump(),
                "application/json");
            return;
        }

        std::string pvRaw;
        double preprocess_ms = 0.0;
        if (!parsed.image_url.empty()) {
            PythonPreprocessResult preprocessed;
            std::string preprocess_error;
            if (!preprocess_image_with_python(baseDir, parsed.image_url, preprocessed, preprocess_error)) {
                res.status = 400;
                res.set_content(
                    error_body("image_url preprocessing failed: " + preprocess_error,
                               "invalid_request_error").dump(),
                    "application/json");
                return;
            }
            pvRaw = preprocessed.pixel_values_path.string();
            preprocess_ms = preprocessed.preprocess_ms;
        }

        const bool continue_from_visual_session =
            pvRaw.empty() && sessions.is_active_visual_session(session_id);

        std::string prompt = parsed.prompt;
        if (continue_from_visual_session) {
            prompt = parsed.latest_user_prompt.empty()
                         ? parsed.prompt
                         : parsed.latest_user_prompt;
        }
        if (prompt.empty()) {
            res.status = 400;
            res.set_content(error_body("No user input_text found in 'input'",
                                       "invalid_request_error").dump(),
                            "application/json");
            return;
        }

        if (prompt.size() > i2t_max_prompt_chars()) {
            res.status = 400;
            res.set_content(
                error_body(
                    "Input exceeds model context window. Reduce input text length and retry.",
                    "context_length_exceeded"
                ).dump(),
                "application/json");
            return;
        }

        if (!sessions.try_activate_or_validate_session(session_id)) {
            res.status = 409;
            const std::string active_sid = sessions.active_session_snapshot();
            if (!active_sid.empty()) {
                res.set_header("X-Active-Session-Id", active_sid);
            }
            res.set_content(session_conflict_body().dump(), "application/json");
            return;
        }

        if (!pvRaw.empty()) {
            if (!ensure_vision_transition_ready(worker, sessions, session_id, res)) {
                return;
            }

            fs::path pvPath = fs::path(pvRaw).is_absolute()
                            ? fs::path(pvRaw)
                            : baseDir / pvRaw;

            if (!fs::exists(pvPath)) {
                res.status = 400;
                res.set_content(
                    error_body("pixel_values_path not found after preprocessing",
                               "invalid_request_error").dump(),
                    "application/json");
                return;
            }

            if (!stream) {
                try {
                    const auto t_worker_start = std::chrono::steady_clock::now();
                    auto fut = worker.submitVision(pvPath, prompt);
                    const std::string answer = fut.get();
                    sessions.mark_active_session(session_id, true);
                    const double worker_ms = round3(elapsed_ms(t_worker_start));

                    json timing = {
                        {"parse_json_ms", parse_json_ms},
                        {"image_preprocess_ms", preprocess_ms},
                        {"worker_inference_ms", worker_ms},
                        {"request_total_ms", round3(elapsed_ms(t_req_start))}
                    };
                    res.set_header("X-Parse-Json-Ms", std::to_string(parse_json_ms));
                    res.set_header("X-Image-Preprocess-Ms", std::to_string(preprocess_ms));
                    res.set_header("X-Worker-Inference-Ms", std::to_string(worker_ms));
                    res.set_header("X-Request-Total-Ms",
                                   std::to_string(round3(elapsed_ms(t_req_start))));
                    res.set_content(
                        responses_completed_body(rid, model, answer, created, timing).dump(),
                        "application/json");
                } catch (const std::exception& e) {
                    const std::string err = e.what();
                    std::cerr << "[SSE] runVision error: " << err << "\n";
                    if (should_recover_worker_on_error(err)) {
                        best_effort_session_recover(worker, sessions);
                    }
                    set_inference_error_response(res, err);
                }
                return;
            }

            auto queue = std::make_shared<TokenQueue>();
            try {
                const auto t_submit_start = std::chrono::steady_clock::now();
                worker.submitrunVisionContinuous(
                    pvPath,
                    prompt,
                    [queue](std::string_view token) { queue->push(token); },
                    [queue]()                        { queue->finish(); },
                    [queue](const std::string& err)  { queue->fail(err); }
                );
                sessions.mark_active_session(session_id, true);
                const double submit_ms = round3(elapsed_ms(t_submit_start));
                res.set_header("X-Parse-Json-Ms", std::to_string(parse_json_ms));
                res.set_header("X-Image-Preprocess-Ms", std::to_string(preprocess_ms));
                res.set_header("X-Queue-Submit-Ms", std::to_string(submit_ms));
                res.set_header("X-Request-Setup-Ms",
                               std::to_string(round3(elapsed_ms(t_req_start))));
            } catch (const std::exception& e) {
                const std::string err = e.what();
                std::cerr << "[SSE] submitrunVisionContinuous error: " << err << "\n";
                if (should_recover_worker_on_error(err)) {
                    best_effort_session_recover(worker, sessions);
                }
                set_inference_error_response(res, err);
                return;
            }
            attach_responses_stream(res,
                                    queue,
                                    rid,
                                    model,
                                    created,
                                    parse_json_ms,
                                    preprocess_ms,
                                    t_req_start);
            return;
        }

        if (!stream) {
            try {
                const auto t_worker_start = std::chrono::steady_clock::now();
                auto fut = continue_from_visual_session
                             ? worker.submitContinueText(prompt)
                             : worker.submitText(prompt);
                const std::string answer = fut.get();
                sessions.mark_active_session(session_id, continue_from_visual_session);
                const double worker_ms = round3(elapsed_ms(t_worker_start));

                json timing = {
                    {"parse_json_ms", parse_json_ms},
                    {"worker_inference_ms", worker_ms},
                    {"request_total_ms", round3(elapsed_ms(t_req_start))}
                };
                res.set_header("X-Parse-Json-Ms", std::to_string(parse_json_ms));
                res.set_header("X-Worker-Inference-Ms", std::to_string(worker_ms));
                res.set_header("X-Request-Total-Ms",
                               std::to_string(round3(elapsed_ms(t_req_start))));
                res.set_content(
                    responses_completed_body(rid, model, answer, created, timing).dump(),
                    "application/json");
            } catch (const std::exception& e) {
                const std::string err = e.what();
                std::cerr << "[SSE] runText error: " << err << "\n";
                if (should_recover_worker_on_error(err)) {
                    best_effort_session_recover(worker, sessions);
                }
                set_inference_error_response(res, err);
            }
            return;
        }

        auto queue = std::make_shared<TokenQueue>();
        try {
            const auto t_submit_start = std::chrono::steady_clock::now();
            if (continue_from_visual_session) {
                worker.submitContinueTextContinuous(
                    prompt,
                    [queue](std::string_view token) { queue->push(token); },
                    [queue]()                        { queue->finish(); },
                    [queue](const std::string& err)  { queue->fail(err); }
                );
                sessions.mark_active_session(session_id, true);
            } else {
                worker.submitrunTextContinuous(
                    prompt,
                    [queue](std::string_view token) { queue->push(token); },
                    [queue]()                        { queue->finish(); },
                    [queue](const std::string& err)  { queue->fail(err); }
                );
                sessions.mark_active_session(session_id, false);
            }
            const double submit_ms = round3(elapsed_ms(t_submit_start));
            res.set_header("X-Parse-Json-Ms", std::to_string(parse_json_ms));
            res.set_header("X-Queue-Submit-Ms", std::to_string(submit_ms));
            res.set_header("X-Request-Setup-Ms",
                           std::to_string(round3(elapsed_ms(t_req_start))));
        } catch (const std::exception& e) {
            const std::string err = e.what();
            std::cerr << "[SSE] submitrunTextContinuous error: " << err << "\n";
            if (should_recover_worker_on_error(err)) {
                best_effort_session_recover(worker, sessions);
            }
            set_inference_error_response(res, err);
            return;
        }
        attach_responses_stream(res,
                                queue,
                                rid,
                                model,
                                created,
                                parse_json_ms,
                                0.0,
                                t_req_start);
    });
}

static void register_routes(httplib::Server& svr,
                            const fs::path& baseDir,
                            GenieWorker& worker,
                            SessionRuntime& sessions,
                            const std::string& model) {
    register_health_route(svr, worker, model);
    register_models_route(svr, model);
    register_session_reset_route(svr, worker, sessions);
    register_session_release_route(svr, worker, sessions);
    register_responses_route(svr, baseDir, worker, sessions, model);
}

// =============================================================================
// Main
// =============================================================================
int main(int argc, char** argv) {
    const CliOptions opts = parse_cli_options(argc, argv);
    const fs::path& baseDir = opts.base_dir;
    const int port = opts.port;

    std::cout << "[SSE] Image-To-Text SSE Server\n"
              << "[SSE] base-dir         : " << baseDir          << "\n"
              << "[SSE] port             : " << port             << "\n";

    GenieWorker worker(baseDir);
    std::cout << "[SSE] Starting GenieWorker (loading model)...\n";
    worker.start();
    std::cout << "[SSE] GenieWorker ready.\n";

    const std::string MODEL = "qwen2.5-vl-7b-instruct";
    SessionRuntime sessions;

    httplib::Server svr;
    // Serialize request handlers to avoid unsafe overlap across session/reset/vision paths.
    svr.new_task_queue = [] { return new httplib::ThreadPool(1); };
    configure_server(svr);
    register_routes(svr, baseDir, worker, sessions, MODEL);

    std::cout << "[SSE] Endpoints:\n"
              << "[SSE]   GET  /health\n"
              << "[SSE]   GET  /v1/models\n"
              << "[SSE]   POST /v1/responses\n"
              << "[SSE]   POST /v1/session/reset\n"
              << "[SSE]   POST /v1/session/release\n"
              << "[SSE] Listening on 0.0.0.0:" << port << " ...\n";

    if (!svr.listen("0.0.0.0", port)) {
        std::cerr << "[SSE] Failed to bind port " << port << "\n";
        return 1;
    }
    return 0;
}
