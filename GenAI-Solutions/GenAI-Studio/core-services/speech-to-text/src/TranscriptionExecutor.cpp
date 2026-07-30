// ---------------------------------------------------------------------
// Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// ---------------------------------------------------------------------

#include "TranscriptionExecutor.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::duration<double, std::milli>;

struct TempFileGuard {
    std::string path;
    explicit TempFileGuard(std::string p) : path(std::move(p)) {}
    ~TempFileGuard() {
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }
    TempFileGuard(const TempFileGuard&) = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;
};

std::string makeTempWavPath() {
    static std::atomic<uint64_t> counter{0};
    return "/tmp/asr_" + std::to_string(counter.fetch_add(1)) + ".wav";
}

bool saveToFile(const std::string& data, const std::string& path) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
    return ofs.good();
}

struct ParsedWavPcm16 {
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    std::vector<int16_t> samples;  // interleaved
};

static uint16_t readLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) |
           (static_cast<uint16_t>(p[1]) << 8);
}

static uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

bool parsePcm16Wav(const std::string& path, ParsedWavPcm16& out, std::string& err) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        err = "Unable to read uploaded audio";
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(ifs)),
                               std::istreambuf_iterator<char>());
    if (bytes.size() < 44) {
        err = "Invalid WAV: too small";
        return false;
    }
    if (std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        err = "Unsupported audio container. Expected WAV/RIFF";
        return false;
    }

    size_t pos = 12;
    bool found_fmt = false;
    bool found_data = false;
    uint16_t audio_format = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;
    size_t data_offset = 0;
    uint32_t data_size = 0;

    while (pos + 8 <= bytes.size()) {
        const uint8_t* chunk = bytes.data() + pos;
        const uint32_t chunk_size = readLe32(chunk + 4);
        const size_t payload = pos + 8;
        if (payload + chunk_size > bytes.size()) {
            err = "Invalid WAV: malformed chunk size";
            return false;
        }

        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            if (chunk_size < 16) {
                err = "Invalid WAV: fmt chunk too small";
                return false;
            }
            const uint8_t* f = bytes.data() + payload;
            audio_format = readLe16(f + 0);
            channels = readLe16(f + 2);
            sample_rate = readLe32(f + 4);
            bits_per_sample = readLe16(f + 14);
            found_fmt = true;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            data_offset = payload;
            data_size = chunk_size;
            found_data = true;
        }

        pos = payload + chunk_size + (chunk_size % 2);  // chunks are word-aligned
    }

    if (!found_fmt || !found_data) {
        err = "Invalid WAV: missing fmt/data chunks";
        return false;
    }
    if (audio_format != 1) {
        err = "Unsupported WAV codec. Expected PCM";
        return false;
    }
    if (channels == 0 || sample_rate == 0) {
        err = "Invalid WAV metadata";
        return false;
    }
    if (bits_per_sample != 16) {
        err = "Unsupported PCM bit depth. Expected 16-bit PCM WAV";
        return false;
    }
    if (data_size % 2 != 0) {
        err = "Invalid WAV PCM payload alignment";
        return false;
    }

    out.channels = channels;
    out.sample_rate = sample_rate;
    out.samples.resize(data_size / 2);
    std::memcpy(out.samples.data(), bytes.data() + data_offset, data_size);
    return true;
}

std::vector<int16_t> downmixToMono(const std::vector<int16_t>& interleaved, uint16_t channels) {
    if (channels <= 1) return interleaved;

    const size_t frames = interleaved.size() / channels;
    std::vector<int16_t> mono(frames, 0);
    for (size_t i = 0; i < frames; ++i) {
        int32_t acc = 0;
        for (uint16_t c = 0; c < channels; ++c) {
            acc += interleaved[i * channels + c];
        }
        mono[i] = static_cast<int16_t>(acc / static_cast<int32_t>(channels));
    }
    return mono;
}

std::vector<int16_t> resampleLinear16k(const std::vector<int16_t>& in, uint32_t in_rate) {
    if (in_rate == 16000 || in.empty()) return in;

    const double ratio = static_cast<double>(in_rate) / 16000.0;
    const size_t out_frames =
        static_cast<size_t>(static_cast<double>(in.size()) * 16000.0 / static_cast<double>(in_rate));
    if (out_frames == 0) return {};

    std::vector<int16_t> out(out_frames);
    for (size_t i = 0; i < out_frames; ++i) {
        const double src = static_cast<double>(i) * ratio;
        const size_t idx = static_cast<size_t>(src);
        const double frac = src - static_cast<double>(idx);
        const int16_t s0 = in[idx < in.size() ? idx : (in.size() - 1)];
        const int16_t s1 = in[(idx + 1) < in.size() ? (idx + 1) : (in.size() - 1)];
        const double v = static_cast<double>(s0) + frac * static_cast<double>(s1 - s0);
        out[i] = static_cast<int16_t>(v);
    }
    return out;
}

bool writePcm16MonoWav(const std::string& path, const std::vector<int16_t>& samples, uint32_t sample_rate) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;

    const uint16_t channels = 1;
    const uint16_t bits_per_sample = 16;
    const uint16_t block_align = channels * (bits_per_sample / 8);
    const uint32_t byte_rate = sample_rate * block_align;
    const uint32_t data_size = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    const uint32_t riff_size = 4 + (8 + 16) + (8 + data_size);

    ofs.write("RIFF", 4);
    ofs.write(reinterpret_cast<const char*>(&riff_size), 4);
    ofs.write("WAVE", 4);
    ofs.write("fmt ", 4);
    const uint32_t fmt_size = 16;
    const uint16_t audio_format = 1;
    ofs.write(reinterpret_cast<const char*>(&fmt_size), 4);
    ofs.write(reinterpret_cast<const char*>(&audio_format), 2);
    ofs.write(reinterpret_cast<const char*>(&channels), 2);
    ofs.write(reinterpret_cast<const char*>(&sample_rate), 4);
    ofs.write(reinterpret_cast<const char*>(&byte_rate), 4);
    ofs.write(reinterpret_cast<const char*>(&block_align), 2);
    ofs.write(reinterpret_cast<const char*>(&bits_per_sample), 2);
    ofs.write("data", 4);
    ofs.write(reinterpret_cast<const char*>(&data_size), 4);
    ofs.write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(data_size));
    return ofs.good();
}

bool normalizeUploadedWavTo16kMonoPcm(const std::string& in_path,
                                      const std::string& out_path,
                                      std::string& err) {
    ParsedWavPcm16 wav;
    if (parsePcm16Wav(in_path, wav, err)) {
        auto mono = downmixToMono(wav.samples, wav.channels);
        auto resampled = resampleLinear16k(mono, wav.sample_rate);

        if (!writePcm16MonoWav(out_path, resampled, 16000)) {
            err = "Failed to write normalized WAV";
            return false;
        }
        return true;
    }

    // Fallback path for compressed/unsupported inputs:
    // decode with ffmpeg into 16k mono pcm_s16le WAV.
    if (std::system("command -v ffmpeg >/dev/null 2>&1") != 0) {
        err = "Unsupported audio format and ffmpeg is unavailable for conversion";
        return false;
    }

    const std::string cmd =
        "ffmpeg -y -hide_banner -loglevel error -i '" + in_path +
        "' -ac 1 -ar 16000 -c:a pcm_s16le '" + out_path + "' >/dev/null 2>&1";
    if (std::system(cmd.c_str()) != 0 || !std::filesystem::exists(out_path)) {
        err = "Unsupported audio content. Failed to convert input to 16k mono PCM WAV";
        return false;
    }
    return true;
}
} // namespace

TranscriptionExecutor::TranscriptionExecutor(WhisperEngine& engine,
                                             std::timed_mutex& engine_mutex,
                                             const AsrRuntimeConfig& config)
    : engine_(engine),
      engine_mutex_(engine_mutex),
      config_(config) {}

bool TranscriptionExecutor::transcribeFileAudio(const std::string& audio_content,
                                                const std::string& language,
                                                bool translate,
                                                TranscriptionExecution& out,
                                                int& error_status,
                                                std::string& error_message) const {
    out = TranscriptionExecution{};
    error_status = 0;
    error_message.clear();

    const std::string temp_wav_path = makeTempWavPath();
    TempFileGuard guard(temp_wav_path);
    const std::string normalized_wav_path = temp_wav_path + ".norm.wav";
    TempFileGuard norm_guard(normalized_wav_path);
    const auto save_start = Clock::now();
    if (!saveToFile(audio_content, temp_wav_path)) {
        error_status = 500;
        error_message = "Failed to write temp file";
        return false;
    }
    std::string normalize_err;
    if (!normalizeUploadedWavTo16kMonoPcm(temp_wav_path, normalized_wav_path, normalize_err)) {
        error_status = 400;
        error_message = normalize_err;
        return false;
    }
    out.save_file_ms = Ms(Clock::now() - save_start).count();

    std::unique_lock<std::timed_mutex> lock(engine_mutex_, std::defer_lock);
    const auto lock_start = Clock::now();
    if (!lock.try_lock_for(config_.engine_lock_timeout)) {
        error_status = 429;
        error_message = "Model is busy. Try again later.";
        return false;
    }
    out.lock_wait_ms = Ms(Clock::now() - lock_start).count();

    out.result = engine_.transcribeFile(normalized_wav_path, translate ? "" : language, translate);
    lock.unlock();

    if (!out.result.success) {
        error_status = (out.result.error.find("timed out") != std::string::npos) ? 504 : 500;
        error_message = out.result.error;
        return false;
    }
    return true;
}

bool TranscriptionExecutor::transcribePcmAudio(const std::vector<uint8_t>& pcm_bytes,
                                               const std::string& language,
                                               bool translate,
                                               TranscriptionExecution& out,
                                               int& error_status,
                                               std::string& error_message) const {
    out = TranscriptionExecution{};
    error_status = 0;
    error_message.clear();

    std::unique_lock<std::timed_mutex> lock(engine_mutex_, std::defer_lock);
    const auto lock_start = Clock::now();
    if (!lock.try_lock_for(config_.engine_lock_timeout)) {
        error_status = 429;
        error_message = "Model is busy. Try again later.";
        return false;
    }
    out.lock_wait_ms = Ms(Clock::now() - lock_start).count();

    out.result = engine_.transcribePCM(pcm_bytes, language, translate);
    lock.unlock();

    if (!out.result.success) {
        error_status = (out.result.error.find("timed out") != std::string::npos) ? 504 : 500;
        error_message = out.result.error;
        return false;
    }
    return true;
}

RealtimeTranscribeAttempt TranscriptionExecutor::transcribePcmWithRollback(
    const std::vector<uint8_t>& pcm_bytes,
    const std::string& language,
    bool translate,
    const std::function<void()>& rollback_fn) const {
    RealtimeTranscribeAttempt out;
    if (pcm_bytes.empty()) return out;

    std::unique_lock<std::timed_mutex> lock(engine_mutex_, std::defer_lock);
    const auto lock_start = Clock::now();
    if (!lock.try_lock_for(config_.engine_lock_timeout)) {
        rollback_fn();
        out.success = false;
        out.error_status = 429;
        out.error_message = "Model is busy. Try again later.";
        return out;
    }
    out.lock_wait_ms = Ms(Clock::now() - lock_start).count();

    out.result = engine_.transcribePCM(pcm_bytes, language, translate);
    if (!out.result->success) {
        rollback_fn();
        out.success = false;
        out.error_status =
            out.result->error.find("timed out") != std::string::npos ? 504 : 500;
        out.error_message = out.result->error;
        return out;
    }

    return out;
}
