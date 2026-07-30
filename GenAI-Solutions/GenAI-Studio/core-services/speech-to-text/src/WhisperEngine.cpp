// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------
#include "WhisperEngine.hpp"

#include <stdexcept>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <condition_variable>
#include <map>
#include <chrono>
#include <atomic>
#include <vector>
#include <optional>
#include <cstdlib>
#include <cctype>

// ==========================================================================
// TranscriptionListener
//
// Bridges the async Whisper SDK callbacks to a blocking wait.
// The listener is registered once at init time and reused across requests.
// reset() must be called before each new transcription.
// ==========================================================================
class WhisperEngine::TranscriptionListener
    : public WhisperFunction::WhisperResponseListener
{
public:
    // Prepare for a new transcription request
    void reset() {
        std::lock_guard<std::mutex> lock(mu_);
        done_             = false;
        error_            = false;
        error_code_       = 0;
        accumulated_text_.clear();
        language_.clear();
    }

    // -----------------------------------------------------------------------
    // SDK callbacks (called from the Whisper processing thread)
    // -----------------------------------------------------------------------

    // Called for each partial or final transcription segment.
    // The SDK may call this multiple times for long audio; we accumulate
    // only the final segments (isFinal == "1").
    void onTranscription(
        const std::map<std::string, std::string>& results) override
    {
        const std::string& text     = results.at(KEY_TRANSCRIPTION);
        const std::string& lang     = results.at(KEY_LANGUAGE);
        const std::string& is_final = results.at(KEY_IS_FINAL);

        std::lock_guard<std::mutex> lock(mu_);
        language_ = lang;

        if (is_final == "1") {
            accumulated_text_ += text;
        }
        // Partial transcriptions are not accumulated; the final result
        // contains the complete text for that segment.
    }

    // Called on VAD and lifecycle events.
    // For file transcription, completing on EVENT_SPEECH_ENDED can truncate
    // multi-segment audio; wait for EVENT_ON_FINISHED instead.
    void onEvent(int event) override {
        if (event == EVENT_ON_FINISHED) {
            std::lock_guard<std::mutex> lock(mu_);
            if (!done_) {
                done_ = true;
                cv_.notify_all();
            }
        }
    }

    // Called on SDK error.
    void onError(int error) override {
        std::lock_guard<std::mutex> lock(mu_);
        error_      = true;
        error_code_ = error;
        done_       = true;
        cv_.notify_all();
    }

    // -----------------------------------------------------------------------
    // Blocking wait – called from the HTTP request thread
    // -----------------------------------------------------------------------
    TranscriptionResult waitForResult(std::chrono::seconds timeout) {
        std::unique_lock<std::mutex> lock(mu_);
        const bool signaled =
            cv_.wait_for(lock, timeout, [this] { return done_; });

        TranscriptionResult r;
        if (!signaled) {
            r.success = false;
            r.error   = "Transcription timed out after " +
                        std::to_string(timeout.count()) + " s";
        } else if (error_) {
            r.success = false;
            r.error   = "Whisper SDK error code: " + std::to_string(error_code_);
        } else {
            r.success  = true;
            r.text     = accumulated_text_;
            r.language = language_;
        }
        return r;
    }

private:
    std::mutex              mu_;
    std::condition_variable cv_;
    bool                    done_       = false;
    bool                    error_      = false;
    int                     error_code_ = 0;
    std::string             accumulated_text_;
    std::string             language_;
};

// ==========================================================================
// WhisperEngine
// ==========================================================================

namespace {
std::string normalizeDirPath(std::string path) {
    if (!path.empty() && path.back() != '/') path.push_back('/');
    return path;
}

bool fileExists(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    return std::filesystem::exists(path, ec) &&
           std::filesystem::is_regular_file(path, ec);
}

std::optional<std::string> findSingleBinByPrefix(const std::string& dir,
                                                 const std::string& prefix) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
        return std::nullopt;
    }

    std::optional<std::string> match;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const auto file_name = entry.path().filename().string();
        if (file_name.rfind(prefix, 0) == 0 && entry.path().extension() == ".bin") {
            const auto full_path = entry.path().string();
            if (match.has_value() && match.value() != full_path) {
                return std::nullopt;
            }
            match = full_path;
        }
    }
    return match;
}

std::string resolveRequiredModelFile(const std::string& model_dir,
                                     const std::string& canonical_name,
                                     const std::string& prefix_fallback) {
    const std::string canonical = model_dir + canonical_name;
    if (fileExists(canonical)) return canonical;
    if (!prefix_fallback.empty()) {
        const auto maybe = findSingleBinByPrefix(model_dir, prefix_fallback);
        if (maybe.has_value()) return maybe.value();
    }
    return "";
}

std::string resolveVocabFile(const std::string& model_dir) {
    const std::vector<std::string> candidates = {
        model_dir + "vocab.bin",
        model_dir + "whisper_vocab.bin",
    };
    for (const auto& candidate : candidates) {
        if (fileExists(candidate)) return candidate;
    }
    return "";
}

void replaceAll(std::string& text, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string normalizeDecodedText(std::string text) {
    replaceAll(text, u8"Ġ", " ");
    replaceAll(text, u8"Ċ", "\n");

    std::string cleaned;
    cleaned.reserve(text.size());
    bool last_was_space = false;
    for (char ch : text) {
        if (ch == '\n') {
            if (!cleaned.empty() && cleaned.back() == ' ') cleaned.pop_back();
            cleaned.push_back('\n');
            last_was_space = false;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!last_was_space) {
                cleaned.push_back(' ');
                last_was_space = true;
            }
            continue;
        }
        cleaned.push_back(ch);
        last_was_space = false;
    }

    // Trim leading/trailing spaces and newlines.
    size_t start = 0;
    while (start < cleaned.size() &&
           std::isspace(static_cast<unsigned char>(cleaned[start]))) {
        ++start;
    }
    size_t end = cleaned.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(cleaned[end - 1]))) {
        --end;
    }
    return cleaned.substr(start, end - start);
}

// SDK 2.6 exposes InputStream::writeFile(path), while 2.5 exposes write(path).
template <typename T>
auto writeAudioFile(T& stream, const std::string& wav_path, int)
    -> decltype(stream.writeFile(wav_path), void()) {
    (void)stream.writeFile(wav_path);
}

template <typename T>
auto writeAudioFile(T& stream, const std::string& wav_path, long)
    -> decltype(stream.write(wav_path), void()) {
    stream.write(wav_path);
}
}  // namespace

WhisperEngine::WhisperEngine(const std::string& model_path,
                             const std::string& vad_model_path)
    : model_path_(model_path),
      vad_model_path_(vad_model_path),
      listener_(std::make_unique<TranscriptionListener>()) {}

WhisperEngine::~WhisperEngine() {
    shutdown();
}

// --------------------------------------------------------------------------
// initialize – load the model (blocking, called once at startup)
// --------------------------------------------------------------------------
void WhisperEngine::initialize() {
    std::lock_guard<std::mutex> lock(request_mu_);

    model_path_ = normalizeDirPath(model_path_);

    std::cout << "[WhisperEngine] SDK version: " << whisper_.getVersion() << "\n"
              << "[WhisperEngine] Model path:  " << model_path_ << "\n";


    //TODO: Based on AIHUB Naming conventions
    //https://qaihub-public-assets.s3.us-west-2.amazonaws.com/qai-hub-models/models/whisper_tiny/releases/v0.50.2/whisper_tiny-qnn_context_binary-float-qualcomm_qcs9075.zip
    const std::string encoder_path = resolveRequiredModelFile(
        model_path_, "encoder.bin", "whisper_tiny-encoder-");
    const std::string decoder_path = resolveRequiredModelFile(
        model_path_, "decoder.bin", "whisper_tiny-decoder-");
    const std::string vocab_path = resolveVocabFile(model_path_);

    std::string vad_model;
    if (const char* env_vad_path = std::getenv("ASR_VAD_PATH")) {
        const std::string path = env_vad_path;
        if (!path.empty() && fileExists(path)) {
            vad_model = path;
        }
    }
    if (vad_model.empty() && !vad_model_path_.empty() && fileExists(vad_model_path_)) {
        vad_model = vad_model_path_;
    }
    if (vad_model.empty()) {
        const std::vector<std::string> vad_candidates = {
            model_path_ + "libnnvad_model.so",
            model_path_ + "speech_float.eai",
            "/opt/asr-assets/libnnvad_model.so",
            "/opt/asr-assets/speech_float.eai",
        };
        for (const auto& candidate : vad_candidates) {
            if (fileExists(candidate)) {
                vad_model = candidate;
                break;
            }
        }
    }

    std::vector<std::string> missing;
    if (encoder_path.empty()) missing.emplace_back("encoder_model_htp.bin");
    if (decoder_path.empty()) missing.emplace_back("decoder_model_htp.bin");
    if (vocab_path.empty())   missing.emplace_back("vocab.bin");
    if (vad_model.empty())    missing.emplace_back("libnnvad_model.so or speech_float.eai");
    if (!missing.empty()) {
        std::string error = "[WhisperEngine] Missing required model artifacts in " + model_path_ + ": ";
        for (size_t idx = 0; idx < missing.size(); ++idx) {
            if (idx > 0) error += ", ";
            error += missing[idx];
        }
        throw std::runtime_error(error);
    }

    std::cout << "[WhisperEngine] Encoder: " << encoder_path << "\n"
              << "[WhisperEngine] Decoder: " << decoder_path << "\n"
              << "[WhisperEngine] Vocab  : " << vocab_path << "\n"
              << "[WhisperEngine] VAD    : " << vad_model << "\n";

    // Build the paths map expected by Whisper::init()
    std::map<std::string, std::string> paths;
    paths[WhisperFunction::Whisper::KEY_MODEL_FILE_ENCODER] = encoder_path;
    paths[WhisperFunction::Whisper::KEY_MODEL_FILE_DECODER] = decoder_path;
    paths[WhisperFunction::Whisper::KEY_VOCAB_FILE]         = vocab_path;
    paths[WhisperFunction::Whisper::KEY_PATH_VAD_MODEL]     = vad_model;
    // Populate both key variants for SDK compatibility (2.5 ADSP, 2.6 CDSP).
    paths["key_path_adsp"]                                  = model_path_;
    paths["key_path_cdsp"]                                  = model_path_;

    if (!whisper_.init(paths)) {
        throw std::runtime_error(
            "[WhisperEngine] whisper_.init() failed. "
            "Check model path: " + model_path_);
    }

    // Default configuration (mirrors the sample app)
    whisper_.setVadLenHangover(120);           // ms of silence before speech-end
    whisper_.setPartialTranscriptionsEnabled(true);
    whisper_.setContinuousTranscriptionEnabled(false);
    whisper_.setSuppressNonSpeech(true);

    // Register the listener once; it is reused across all requests
    whisper_.registerListener(listener_.get());

    initialized_ = true;
    std::cout << "[WhisperEngine] Initialized successfully.\n";
}

// --------------------------------------------------------------------------
// shutdown – unload the model
// --------------------------------------------------------------------------
void WhisperEngine::shutdown() noexcept {
    std::lock_guard<std::mutex> lock(request_mu_);
    if (initialized_) {
        whisper_.stop();
        whisper_.deInit();
        initialized_ = false;
        std::cout << "[WhisperEngine] Shut down.\n";
    }
}

bool WhisperEngine::isReady() const noexcept {
    std::lock_guard<std::mutex> lock(request_mu_);
    return initialized_;
}

std::string WhisperEngine::getVersion() {
    return whisper_.getVersion();
}

// --------------------------------------------------------------------------
// Public transcription helpers
// --------------------------------------------------------------------------
TranscriptionResult WhisperEngine::transcribeFile(
    const std::string& wav_path,
    const std::string& language_code,
    bool               translate,
    std::chrono::seconds timeout)
{
    return doTranscribe(/*use_file=*/true, wav_path, {}, language_code, translate, timeout);
}

// --------------------------------------------------------------------------
// pcmToWav – wrap raw 16-bit mono PCM in a minimal WAV header
// --------------------------------------------------------------------------
static std::vector<uint8_t> pcmToWav(const std::vector<uint8_t>& pcm,
                                      int sample_rate = 16000) {
    const uint32_t data_size   = static_cast<uint32_t>(pcm.size());
    const uint32_t chunk_size  = 36 + data_size;
    const uint16_t num_ch      = 1;
    const uint16_t bits        = 16;
    const uint32_t byte_rate   = sample_rate * num_ch * bits / 8;
    const uint16_t block_align = num_ch * bits / 8;

    std::vector<uint8_t> wav;
    wav.reserve(44 + data_size);

    auto push16 = [&](uint16_t v) {
        wav.push_back(v & 0xFF);
        wav.push_back((v >> 8) & 0xFF);
    };
    auto push32 = [&](uint32_t v) {
        wav.push_back(v & 0xFF);
        wav.push_back((v >> 8) & 0xFF);
        wav.push_back((v >> 16) & 0xFF);
        wav.push_back((v >> 24) & 0xFF);
    };
    auto pushStr = [&](const char* s, size_t n) {
        for (size_t i = 0; i < n; ++i) wav.push_back(static_cast<uint8_t>(s[i]));
    };

    pushStr("RIFF", 4);  push32(chunk_size);
    pushStr("WAVE", 4);
    pushStr("fmt ", 4);  push32(16);
    push16(1);            // PCM
    push16(num_ch);
    push32(static_cast<uint32_t>(sample_rate));
    push32(byte_rate);
    push16(block_align);
    push16(bits);
    pushStr("data", 4);  push32(data_size);
    wav.insert(wav.end(), pcm.begin(), pcm.end());
    return wav;
}

static std::string makeTempPcmWavPath() {
    static std::atomic<uint64_t> cnt{0};
    return "/tmp/asr_pcm_" + std::to_string(cnt.fetch_add(1)) + ".wav";
}

TranscriptionResult WhisperEngine::transcribePCM(
    const std::vector<uint8_t>& pcm_data,
    const std::string& language_code,
    bool               translate,
    std::chrono::seconds timeout)
{
    // Wrap raw PCM in a WAV header and delegate to the file-based path.
    // This ensures the VAD pipeline (which expects a WAV file) works
    // correctly and fires EVENT_SPEECH_ENDED to unblock the listener.
    const std::vector<uint8_t> wav = pcmToWav(pcm_data);
    const std::string tmp = makeTempPcmWavPath();

    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            return TranscriptionResult{
                .success = false,
                .error   = "Failed to write temp PCM WAV file"};
        }
        ofs.write(reinterpret_cast<const char*>(wav.data()),
                  static_cast<std::streamsize>(wav.size()));
    }

    TranscriptionResult result =
        doTranscribe(/*use_file=*/true, tmp, {}, language_code, translate, timeout);

    // Clean up temp file
    std::error_code ec;
    std::filesystem::remove(tmp, ec);

    return result;
}

// --------------------------------------------------------------------------
// doTranscribe – core implementation
//
// Flow:
//   1. Set language on the Whisper instance
//   2. Reset the listener state
//   3. Create a fresh InputStream for this request
//   4. whisper_.start(&stream)  – starts the processing thread (non-blocking)
//   5. stream.write(...)        – feeds audio data into the buffer
//   6. listener_->waitForResult() – blocks until onTranscription(isFinal=1)
//                                   or onEvent(SPEECH_ENDED) or timeout
//   7. whisper_.stop()          – stops the processing thread (synchronous)
// --------------------------------------------------------------------------
TranscriptionResult WhisperEngine::doTranscribe(
    bool                        use_file,
    const std::string&          wav_path,
    const std::vector<uint8_t>& pcm_data,
    const std::string&          language_code,
    bool                        translate,
    std::chrono::seconds        timeout)
{
    // Serialise: only one transcription at a time
    std::lock_guard<std::mutex> lock(request_mu_);

    if (!initialized_) {
        return TranscriptionResult{
            .success = false,
            .error   = "WhisperEngine not initialized"};
    }

    const auto t_total_start = std::chrono::steady_clock::now();

    // Apply language and translation settings
    const auto t_config_start = std::chrono::steady_clock::now();
    if (!language_code.empty()) {
        whisper_.setLanguageCode(language_code);
    }
    whisper_.setTranslationEnabled(translate);

    // Reset listener for this request
    listener_->reset();
    const auto t_config_end = std::chrono::steady_clock::now();

    // Fresh InputStream per request to avoid stale state
    WhisperFunction::InputStream input_stream;
    input_stream.setUseAudioFile(use_file);

    // Start the Whisper processing pipeline (non-blocking)
    const auto t_start_begin = std::chrono::steady_clock::now();
    whisper_.start(&input_stream);
    const auto t_start_end = std::chrono::steady_clock::now();

    // Feed audio data into the InputStream buffer
    const auto t_feed_begin = std::chrono::steady_clock::now();
    if (use_file) {
        writeAudioFile(input_stream, wav_path, 0);
    } else {
        // write(vector<uint8_t>, offset, length) feeds raw PCM bytes
        // The SDK expects 16 kHz, 16-bit, mono, little-endian PCM
        std::vector<uint8_t> buf(pcm_data);   // copy – write() takes by value
        input_stream.write(buf, 0, static_cast<int32_t>(buf.size()));
    }
    const auto t_feed_end = std::chrono::steady_clock::now();

    // Block until the listener signals completion or timeout
    const auto t_wait_begin = std::chrono::steady_clock::now();
    TranscriptionResult result = listener_->waitForResult(timeout);
    const auto t_wait_end = std::chrono::steady_clock::now();

    // Stop the processing pipeline (synchronous – waits for thread to exit)
    const auto t_stop_begin = std::chrono::steady_clock::now();
    whisper_.stop();
    const auto t_stop_end = std::chrono::steady_clock::now();

    const auto t_total_end = std::chrono::steady_clock::now();
    if (result.success && !result.text.empty()) {
        result.text = normalizeDecodedText(result.text);
    }
    result.timing.configure_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            t_config_end - t_config_start).count();
    result.timing.start_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            t_start_end - t_start_begin).count();
    result.timing.feed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            t_feed_end - t_feed_begin).count();
    result.timing.wait_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            t_wait_end - t_wait_begin).count();
    result.timing.stop_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            t_stop_end - t_stop_begin).count();
    result.timing.total_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            t_total_end - t_total_start).count();
    result.process_time_ms = result.timing.total_ms;

    return result;
}
