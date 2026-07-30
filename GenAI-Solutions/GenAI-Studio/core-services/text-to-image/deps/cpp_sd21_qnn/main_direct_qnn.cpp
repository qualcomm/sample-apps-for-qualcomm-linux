// =============================================================================
// Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// =============================================================================

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "QNN/QnnCommon.h"
#include "QNN/QnnContext.h"
#include "QNN/QnnDevice.h"
#include "QNN/QnnError.h"
#include "QNN/QnnGraph.h"
#include "QNN/QnnInterface.h"
#include "QNN/QnnLog.h"
#include "QNN/QnnProfile.h"
#include "QNN/QnnTensor.h"
#include "QNN/QnnTypes.h"
#include "QNN/System/QnnSystemContext.h"
#include "QNN/System/QnnSystemInterface.h"

namespace fs = std::filesystem;

struct Options {
  std::string runtime_dir = "/opt/runtime";
  std::string qnn_lib_dir = "";
  std::string tokenizer_dir = "";
  std::string prompt = "a cinematic photo of an orange cat astronaut, detailed, moon surface, ultra realistic";
  std::string negative_prompt = "";
  int steps = 20;
  float guidance = 7.5f;
  int seed = 12345;
  std::string output_image = "";
  std::string log_file = "";
  std::string backend_lib = "libQnnHtp.so";
  std::string system_lib = "libQnnSystem.so";

  //TODO: https://qaihub-public-assets.s3.us-west-2.amazonaws.com/qai-hub-models/models/stable_diffusion_v2_1/releases/v0.50.2/stable_diffusion_v2_1-qnn_context_binary-w8a16-qualcomm_qcs9075.zip
  // This model name is fixed based on AIHUB repo.
  std::string text_context = "text_encoder.bin";
  std::string unet_context = "unet.bin";
  std::string vae_context = "vae.bin";
  std::string prediction_type = "epsilon";
  float vae_scaling_factor = 1.0f;
  bool prediction_type_user_set = false;
  bool vae_scaling_factor_user_set = false;
};

struct Stats {
  std::vector<double> step_seconds;
  double text_uncond_s = 0.0;
  double text_cond_s = 0.0;
  double vae_s = 0.0;
};

static constexpr int kTokenLength = 77;
static constexpr int kEmbTokens = 77;
static constexpr int kEmbDim = 1024;
static constexpr int kLatentC = 4;
static constexpr int kLatentH = 64;
static constexpr int kLatentW = 64;
static constexpr int kImageH = 512;
static constexpr int kImageW = 512;
static constexpr int kImageC = 3;

enum class PredictionType {
  Epsilon,
  VPrediction,
};

enum class TensorLayout4D {
  NCHW,
  NHWC,
};

enum class ImagePostProcessMode {
  Auto,
  Unit01,
  Tanh,
};

static uint16_t Float32ToFloat16(float v) {
  uint32_t x = 0;
  std::memcpy(&x, &v, sizeof(x));
  const uint16_t sign = static_cast<uint16_t>((x >> 16) & 0x8000u);
  int32_t exp = static_cast<int32_t>((x >> 23) & 0xFFu) - 127 + 15;
  const uint32_t mant = x & 0x7FFFFFu;
  if (exp <= 0) {
    if (exp < -10) return sign;
    const uint32_t m = (mant | 0x800000u) >> (1 - exp);
    return static_cast<uint16_t>(sign | ((m + 0x1000u) >> 13));
  }
  if (exp >= 31) {
    return static_cast<uint16_t>(sign | 0x7C00u);
  }
  return static_cast<uint16_t>(sign | (static_cast<uint16_t>(exp) << 10) |
                               static_cast<uint16_t>((mant + 0x1000u) >> 13));
}

static float Float16ToFloat32(uint16_t h) {
  const uint32_t sign = (static_cast<uint32_t>(h & 0x8000u)) << 16;
  uint32_t exp = (h >> 10) & 0x1Fu;
  uint32_t mant = h & 0x03FFu;
  uint32_t out = 0;
  if (exp == 0) {
    if (mant == 0) {
      out = sign;
    } else {
      while ((mant & 0x0400u) == 0) {
        mant <<= 1;
        --exp;
      }
      ++exp;
      mant &= ~0x0400u;
      out = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
    }
  } else if (exp == 31) {
    out = sign | 0x7F800000u | (mant << 13);
  } else {
    out = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
  }
  float v = 0.0f;
  std::memcpy(&v, &out, sizeof(v));
  return v;
}

class Logger {
 public:
  explicit Logger(const std::string& path) : path_(path) {
    fs::create_directories(fs::path(path_).parent_path());
    ofs_.open(path_, std::ios::out | std::ios::trunc);
    if (!ofs_) {
      throw std::runtime_error("Failed to open log file: " + path_);
    }
  }

  void Log(const std::string& msg) {
    std::cout << msg << std::endl;
    ofs_ << msg << "\n";
    ofs_.flush();
  }

 private:
  std::string path_;
  std::ofstream ofs_;
};

static std::string FormatFixed(double v, int prec = 3) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(prec) << v;
  return oss.str();
}

static std::string Pad2(int v) {
  std::ostringstream oss;
  oss << std::setw(2) << std::setfill('0') << v;
  return oss.str();
}

static std::string ReadTextFile(const std::string& path) {
  std::ifstream ifs(path);
  if (!ifs) {
    throw std::runtime_error("Failed to read file: " + path);
  }
  std::ostringstream oss;
  oss << ifs.rdbuf();
  return oss.str();
}

static std::vector<uint8_t> ReadBinaryFile(const std::string& path) {
  std::ifstream ifs(path, std::ios::binary | std::ios::ate);
  if (!ifs) {
    throw std::runtime_error("Failed to read binary file: " + path);
  }
  const std::streamsize sz = ifs.tellg();
  if (sz <= 0) {
    throw std::runtime_error("Binary file is empty: " + path);
  }
  ifs.seekg(0, std::ios::beg);
  std::vector<uint8_t> data(static_cast<size_t>(sz));
  if (!ifs.read(reinterpret_cast<char*>(data.data()), sz)) {
    throw std::runtime_error("Failed to read binary payload: " + path);
  }
  return data;
}

static void WriteTextFile(const std::string& path, const std::string& text) {
  fs::create_directories(fs::path(path).parent_path());
  std::ofstream ofs(path, std::ios::out | std::ios::trunc);
  if (!ofs) {
    throw std::runtime_error("Failed to write file: " + path);
  }
  ofs << text;
}

static std::string Trim(const std::string& s) {
  size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) {
    ++b;
  }
  size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
    --e;
  }
  return s.substr(b, e - b);
}

static std::string CollapseWhitespaceLower(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  bool in_ws = false;
  for (unsigned char c : in) {
    if (std::isspace(c)) {
      if (!in_ws) {
        out.push_back(' ');
        in_ws = true;
      }
    } else {
      out.push_back(static_cast<char>(std::tolower(c)));
      in_ws = false;
    }
  }
  return Trim(out);
}

static std::string CodepointToUtf8(uint32_t cp) {
  std::string out;
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  return out;
}

static std::vector<std::string> Utf8SplitCodepoints(const std::string& s) {
  std::vector<std::string> out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t len = 1;
    if ((c & 0x80) == 0x00) {
      len = 1;
    } else if ((c & 0xE0) == 0xC0) {
      len = 2;
    } else if ((c & 0xF0) == 0xE0) {
      len = 3;
    } else if ((c & 0xF8) == 0xF0) {
      len = 4;
    }
    if (i + len > s.size()) {
      len = 1;
    }
    out.emplace_back(s.substr(i, len));
    i += len;
  }
  return out;
}

static void SkipWs(const std::string& s, size_t& i) {
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
    ++i;
  }
}

static int HexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

static uint32_t ParseHex4(const std::string& s, size_t& i) {
  if (i + 4 > s.size()) {
    throw std::runtime_error("Invalid unicode escape (truncated)");
  }
  uint32_t cp = 0;
  for (int k = 0; k < 4; ++k) {
    int v = HexVal(s[i + k]);
    if (v < 0) {
      throw std::runtime_error("Invalid unicode escape (hex)");
    }
    cp = (cp << 4) | static_cast<uint32_t>(v);
  }
  i += 4;
  return cp;
}

static uint32_t ParseJsonUnicodeCodepoint(const std::string& s, size_t& i) {
  uint32_t cp = ParseHex4(s, i);
  if (cp < 0xD800 || cp > 0xDBFF) {
    return cp;
  }

  if (i + 2 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
    i += 2;
    uint32_t low = ParseHex4(s, i);
    if (low >= 0xDC00 && low <= 0xDFFF) {
      return 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
    }
    throw std::runtime_error("Invalid UTF-16 surrogate pair");
  }
  throw std::runtime_error("Missing UTF-16 low surrogate");
}

static void AppendJsonEscape(const std::string& s, size_t& i, std::string* out) {
  if (i >= s.size()) {
    throw std::runtime_error("Invalid JSON escape");
  }
  char esc = s[i++];
  switch (esc) {
    case '"': out->push_back('"'); break;
    case '\\': out->push_back('\\'); break;
    case '/': out->push_back('/'); break;
    case 'b': out->push_back('\b'); break;
    case 'f': out->push_back('\f'); break;
    case 'n': out->push_back('\n'); break;
    case 'r': out->push_back('\r'); break;
    case 't': out->push_back('\t'); break;
    case 'u': {
      uint32_t cp = ParseJsonUnicodeCodepoint(s, i);
      *out += CodepointToUtf8(cp);
      break;
    }
    default:
      throw std::runtime_error("Unsupported JSON escape sequence");
  }
}

static std::string ParseJsonString(const std::string& s, size_t& i) {
  if (i >= s.size() || s[i] != '"') {
    throw std::runtime_error("Expected JSON string");
  }
  ++i;
  std::string out;
  while (i < s.size()) {
    char c = s[i++];
    if (c == '"') {
      return out;
    }
    if (c != '\\') {
      out.push_back(c);
      continue;
    }
    AppendJsonEscape(s, i, &out);
  }
  throw std::runtime_error("Unterminated JSON string");
}

static int ParseJsonInt(const std::string& s, size_t& i) {
  if (i >= s.size()) {
    throw std::runtime_error("Expected integer");
  }
  int sign = 1;
  if (s[i] == '-') {
    sign = -1;
    ++i;
  }
  if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) {
    throw std::runtime_error("Invalid integer");
  }
  long long v = 0;
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
    v = v * 10 + (s[i] - '0');
    if (v > std::numeric_limits<int>::max()) {
      throw std::runtime_error("Integer overflow");
    }
    ++i;
  }
  return static_cast<int>(sign * v);
}

static std::unordered_map<std::string, int> ParseVocabJson(const std::string& text) {
  size_t i = 0;
  SkipWs(text, i);
  if (i >= text.size() || text[i] != '{') {
    throw std::runtime_error("Vocab JSON must start with '{'");
  }
  ++i;
  std::unordered_map<std::string, int> out;
  out.reserve(55000);

  while (true) {
    SkipWs(text, i);
    if (i >= text.size()) {
      throw std::runtime_error("Unexpected EOF in vocab JSON");
    }
    if (text[i] == '}') {
      ++i;
      break;
    }

    std::string key = ParseJsonString(text, i);
    SkipWs(text, i);
    if (i >= text.size() || text[i] != ':') {
      throw std::runtime_error("Expected ':' in vocab JSON");
    }
    ++i;
    SkipWs(text, i);
    int value = ParseJsonInt(text, i);
    out.emplace(std::move(key), value);

    SkipWs(text, i);
    if (i >= text.size()) {
      throw std::runtime_error("Unexpected EOF after vocab entry");
    }
    if (text[i] == ',') {
      ++i;
      continue;
    }
    if (text[i] == '}') {
      ++i;
      break;
    }
    throw std::runtime_error("Expected ',' or '}' in vocab JSON");
  }

  return out;
}

class ClipBpeTokenizer {
 public:
  ClipBpeTokenizer(const std::string& vocab_path, const std::string& merges_path) {
    vocab_ = ParseVocabJson(ReadTextFile(vocab_path));
    InitBytesToUnicode();
    LoadMerges(merges_path);
    space_token_ = byte_encoder_[32];

    auto bos_it = vocab_.find("<|startoftext|>");
    auto eos_it = vocab_.find("<|endoftext|>");
    if (bos_it == vocab_.end() || eos_it == vocab_.end()) {
      throw std::runtime_error("Tokenizer vocab missing special tokens");
    }
    bos_id_ = bos_it->second;
    eos_id_ = eos_it->second;
    unk_id_ = eos_id_;
    auto space_it = vocab_.find(space_token_);
    if (space_it != vocab_.end()) {
      space_token_id_ = space_it->second;
    }
  }

  std::vector<int> Encode(const std::string& text, int max_len) {
    std::string clean = CollapseWhitespaceLower(text);
    std::vector<int> ids;
    ids.reserve(max_len);
    ids.push_back(bos_id_);

    static const std::regex re("('s|'t|'re|'ve|'m|'ll|'d| ?[A-Za-z]+| ?[0-9]+| ?[^\\sA-Za-z0-9]+|\\s+)");
    auto begin = std::sregex_iterator(clean.begin(), clean.end(), re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
      const std::string piece = it->str();
      if (piece.empty()) {
        continue;
      }
      if (std::all_of(piece.begin(), piece.end(), [](unsigned char c) { return std::isspace(c); })) {
        continue;
      }
      std::string encoded;
      encoded.reserve(piece.size() * 2);
      for (unsigned char b : piece) {
        encoded += byte_encoder_[b];
      }

      auto bpe_tokens = Bpe(encoded);
      for (const std::string& token : bpe_tokens) {
        auto vit = vocab_.find(token);
        const int token_id = (vit == vocab_.end()) ? unk_id_ : vit->second;
        if (space_token_id_ >= 0 && token_id == space_token_id_) {
          continue;
        }
        ids.push_back(token_id);
      }
    }

    ids.push_back(eos_id_);
    if (static_cast<int>(ids.size()) > max_len) {
      ids.resize(max_len);
      ids[max_len - 1] = eos_id_;
    } else {
      while (static_cast<int>(ids.size()) < max_len) {
        ids.push_back(eos_id_);
      }
    }
    return ids;
  }

 private:
  void InitBytesToUnicode() {
    std::vector<int> bs;
    bs.reserve(256);
    for (int b = static_cast<int>('!'); b <= static_cast<int>('~'); ++b) bs.push_back(b);
    for (int b = 0xA1; b <= 0xAC; ++b) bs.push_back(b);
    for (int b = 0xAE; b <= 0xFF; ++b) bs.push_back(b);

    std::unordered_set<int> present(bs.begin(), bs.end());
    std::vector<int> cs = bs;
    int n = 0;
    for (int b = 0; b < 256; ++b) {
      if (present.find(b) == present.end()) {
        bs.push_back(b);
        cs.push_back(256 + n);
        ++n;
      }
    }

    for (size_t i = 0; i < bs.size(); ++i) {
      byte_encoder_[bs[i]] = CodepointToUtf8(static_cast<uint32_t>(cs[i]));
    }
  }

  void LoadMerges(const std::string& merges_path) {
    std::ifstream ifs(merges_path);
    if (!ifs) {
      throw std::runtime_error("Failed to read merges file: " + merges_path);
    }
    std::string line;
    int rank = 0;
    while (std::getline(ifs, line)) {
      line = Trim(line);
      if (line.empty() || line[0] == '#') {
        continue;
      }
      std::istringstream iss(line);
      std::string a, b;
      if (!(iss >> a >> b)) {
        continue;
      }
      bpe_ranks_[a + "\t" + b] = rank++;
    }
  }

  std::vector<std::string> Bpe(const std::string& token) {
    auto cache_it = bpe_cache_.find(token);
    if (cache_it != bpe_cache_.end()) {
      return cache_it->second;
    }

    std::vector<std::string> word = Utf8SplitCodepoints(token);
    if (word.empty()) {
      return {};
    }
    word.back() += "</w>";

    while (true) {
      int best_rank = std::numeric_limits<int>::max();
      size_t best_i = std::numeric_limits<size_t>::max();
      for (size_t i = 0; i + 1 < word.size(); ++i) {
        auto it = bpe_ranks_.find(word[i] + "\t" + word[i + 1]);
        if (it != bpe_ranks_.end() && it->second < best_rank) {
          best_rank = it->second;
          best_i = i;
        }
      }
      if (best_i == std::numeric_limits<size_t>::max()) {
        break;
      }

      std::vector<std::string> merged;
      merged.reserve(word.size());
      size_t i = 0;
      while (i < word.size()) {
        if (i == best_i && i + 1 < word.size()) {
          merged.push_back(word[i] + word[i + 1]);
          i += 2;
        } else {
          merged.push_back(word[i]);
          ++i;
        }
      }
      word.swap(merged);
      if (word.size() == 1) {
        break;
      }
    }

    bpe_cache_[token] = word;
    return word;
  }

  std::unordered_map<std::string, int> vocab_;
  std::array<std::string, 256> byte_encoder_;
  std::unordered_map<std::string, int> bpe_ranks_;
  std::unordered_map<std::string, std::vector<std::string>> bpe_cache_;
  std::string space_token_;
  int bos_id_ = -1;
  int eos_id_ = -1;
  int unk_id_ = -1;
  int space_token_id_ = -1;
};

class DdimScheduler {
 public:
  DdimScheduler(int num_train_timesteps, float beta_start, float beta_end, bool set_alpha_to_one,
                int steps_offset)
      : num_train_timesteps_(num_train_timesteps),
        beta_start_(beta_start),
        beta_end_(beta_end),
        set_alpha_to_one_(set_alpha_to_one),
        steps_offset_(steps_offset) {
    BuildBetas();
  }

  void SetTimesteps(int num_inference_steps) {
    if (num_inference_steps <= 0) {
      throw std::runtime_error("num_inference_steps must be > 0");
    }
    num_inference_steps_ = num_inference_steps;
    step_ratio_ = num_train_timesteps_ / num_inference_steps_;
    if (step_ratio_ <= 0) {
      throw std::runtime_error("Invalid timestep ratio");
    }

    timesteps_.clear();
    timesteps_.reserve(num_inference_steps_);
    for (int i = 0; i < num_inference_steps_; ++i) {
      int t = (num_inference_steps_ - 1 - i) * step_ratio_ + steps_offset_;
      if (t >= num_train_timesteps_) {
        t = num_train_timesteps_ - 1;
      }
      timesteps_.push_back(t);
    }
  }

  const std::vector<int>& timesteps() const { return timesteps_; }
  float init_noise_sigma() const { return 1.0f; }

  std::vector<float> ScaleModelInput(const std::vector<float>& sample, int /*timestep*/) const {
    return sample;
  }

  std::vector<float> Step(const std::vector<float>& model_output_nchw,
                          int timestep,
                          const std::vector<float>& sample_nchw,
                          PredictionType prediction_type) const {
    if (num_inference_steps_ <= 0) {
      throw std::runtime_error("Call SetTimesteps before Step");
    }
    if (model_output_nchw.size() != sample_nchw.size()) {
      throw std::runtime_error("model_output/sample size mismatch");
    }

    const int prev_timestep = timestep - step_ratio_;
    const float alpha_prod_t = alphas_cumprod_.at(static_cast<size_t>(timestep));
    const float alpha_prod_t_prev =
        prev_timestep >= 0 ? alphas_cumprod_.at(static_cast<size_t>(prev_timestep))
                           : (set_alpha_to_one_ ? 1.0f : alphas_cumprod_.front());

    const float sqrt_alpha_prod_t = std::sqrt(alpha_prod_t);
    const float sqrt_beta_prod_t = std::sqrt(1.0f - alpha_prod_t);
    const float sqrt_alpha_prod_t_prev = std::sqrt(alpha_prod_t_prev);
    const float sqrt_beta_prod_t_prev = std::sqrt(1.0f - alpha_prod_t_prev);

    std::vector<float> out(sample_nchw.size());
    for (size_t i = 0; i < sample_nchw.size(); ++i) {
      float pred_original = 0.0f;
      float pred_epsilon = 0.0f;
      if (prediction_type == PredictionType::VPrediction) {
        pred_original = sqrt_alpha_prod_t * sample_nchw[i] - sqrt_beta_prod_t * model_output_nchw[i];
        pred_epsilon = sqrt_alpha_prod_t * model_output_nchw[i] + sqrt_beta_prod_t * sample_nchw[i];
      } else {
        pred_original = (sample_nchw[i] - sqrt_beta_prod_t * model_output_nchw[i]) / sqrt_alpha_prod_t;
        pred_epsilon = model_output_nchw[i];
      }
      const float pred_direction = sqrt_beta_prod_t_prev * pred_epsilon;
      out[i] = sqrt_alpha_prod_t_prev * pred_original + pred_direction;
    }
    return out;
  }

 private:
  void BuildBetas() {
    betas_.resize(num_train_timesteps_);
    alphas_cumprod_.resize(num_train_timesteps_);
    const float sqrt_start = std::sqrt(beta_start_);
    const float sqrt_end = std::sqrt(beta_end_);

    float cumprod = 1.0f;
    for (int i = 0; i < num_train_timesteps_; ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(num_train_timesteps_ - 1);
      const float v = sqrt_start + t * (sqrt_end - sqrt_start);
      const float beta = v * v;
      betas_[i] = beta;
      const float alpha = 1.0f - beta;
      cumprod *= alpha;
      alphas_cumprod_[i] = cumprod;
    }
  }

  int num_train_timesteps_;
  float beta_start_;
  float beta_end_;
  bool set_alpha_to_one_;
  int steps_offset_;
  int num_inference_steps_ = 0;
  int step_ratio_ = 0;
  std::vector<float> betas_;
  std::vector<float> alphas_cumprod_;
  std::vector<int> timesteps_;
};

static std::vector<float> ConvertLatentNchwToLayout(const std::vector<float>& src_nchw,
                                                    TensorLayout4D dst_layout) {
  if (src_nchw.size() != static_cast<size_t>(kLatentC * kLatentH * kLatentW)) {
    throw std::runtime_error("ConvertLatentNchwToLayout input size mismatch");
  }
  if (dst_layout == TensorLayout4D::NCHW) {
    return src_nchw;
  }
  std::vector<float> dst(src_nchw.size());
  for (int h = 0; h < kLatentH; ++h) {
    for (int w = 0; w < kLatentW; ++w) {
      for (int c = 0; c < kLatentC; ++c) {
        const size_t src_idx = static_cast<size_t>(c * kLatentH * kLatentW + h * kLatentW + w);
        const size_t dst_idx = static_cast<size_t>((h * kLatentW + w) * kLatentC + c);
        dst[dst_idx] = src_nchw[src_idx];
      }
    }
  }
  return dst;
}

static std::vector<float> ConvertLatentLayoutToNchw(const std::vector<float>& src,
                                                    TensorLayout4D src_layout) {
  if (src.size() != static_cast<size_t>(kLatentC * kLatentH * kLatentW)) {
    throw std::runtime_error("ConvertLatentLayoutToNchw input size mismatch");
  }
  if (src_layout == TensorLayout4D::NCHW) {
    return src;
  }
  std::vector<float> dst(src.size());
  for (int h = 0; h < kLatentH; ++h) {
    for (int w = 0; w < kLatentW; ++w) {
      for (int c = 0; c < kLatentC; ++c) {
        const size_t src_idx = static_cast<size_t>((h * kLatentW + w) * kLatentC + c);
        const size_t dst_idx = static_cast<size_t>(c * kLatentH * kLatentW + h * kLatentW + w);
        dst[dst_idx] = src[src_idx];
      }
    }
  }
  return dst;
}

static std::vector<float> ConvertImageLayoutToNhwc(const std::vector<float>& src,
                                                   TensorLayout4D src_layout) {
  const size_t expected = static_cast<size_t>(kImageH * kImageW * kImageC);
  if (src.size() != expected) {
    throw std::runtime_error("ConvertImageLayoutToNhwc input size mismatch");
  }
  if (src_layout == TensorLayout4D::NHWC) {
    return src;
  }
  std::vector<float> dst(expected);
  for (int h = 0; h < kImageH; ++h) {
    for (int w = 0; w < kImageW; ++w) {
      for (int c = 0; c < kImageC; ++c) {
        const size_t src_idx = static_cast<size_t>(c * kImageH * kImageW + h * kImageW + w);
        const size_t dst_idx = static_cast<size_t>((h * kImageW + w) * kImageC + c);
        dst[dst_idx] = src[src_idx];
      }
    }
  }
  return dst;
}

static std::vector<float> NormalizeImageForPng(const std::vector<float>& image_nhwc,
                                               ImagePostProcessMode mode) {
  if (image_nhwc.empty()) return {};

  auto [min_it, max_it] = std::minmax_element(image_nhwc.begin(), image_nhwc.end());
  const float min_v = *min_it;
  const float max_v = *max_it;

  bool apply_tanh = false;
  if (mode == ImagePostProcessMode::Tanh) {
    apply_tanh = true;
  } else if (mode == ImagePostProcessMode::Auto) {
    apply_tanh = (min_v < -0.05f || max_v > 1.05f);
  }

  std::vector<float> out(image_nhwc.size());
  if (apply_tanh) {
    for (size_t i = 0; i < image_nhwc.size(); ++i) {
      out[i] = image_nhwc[i] * 0.5f + 0.5f;
    }
    return out;
  }
  return image_nhwc;
}

static void WritePpmRgb(const std::string& path, const std::vector<float>& nhwc_1x512x512x3) {
  const size_t expected = static_cast<size_t>(kImageH * kImageW * kImageC);
  if (nhwc_1x512x512x3.size() != expected) {
    throw std::runtime_error("PPM input size mismatch");
  }

  fs::create_directories(fs::path(path).parent_path());
  std::ofstream ofs(path, std::ios::binary | std::ios::out | std::ios::trunc);
  if (!ofs) {
    throw std::runtime_error("Failed to open output image: " + path);
  }
  ofs << "P6\n" << kImageW << " " << kImageH << "\n255\n";
  for (float v : nhwc_1x512x512x3) {
    const float x = std::clamp(v, 0.0f, 1.0f);
    const uint8_t u = static_cast<uint8_t>(std::lrint(x * 255.0f));
    ofs.write(reinterpret_cast<const char*>(&u), 1);
  }
}

static size_t NumElements(const std::vector<uint32_t>& dims) {
  size_t n = 1;
  for (uint32_t d : dims) {
    n *= static_cast<size_t>(d);
  }
  return n;
}

static std::string DataTypeToString(Qnn_DataType_t dt);

static size_t DataTypeSize(Qnn_DataType_t dt) {
  switch (dt) {
    case QNN_DATATYPE_FLOAT_32:
    case QNN_DATATYPE_INT_32:
    case QNN_DATATYPE_UINT_32:
      return 4;
    case QNN_DATATYPE_FLOAT_16:
    case QNN_DATATYPE_SFIXED_POINT_16:
    case QNN_DATATYPE_INT_16:
    case QNN_DATATYPE_UFIXED_POINT_16:
    case QNN_DATATYPE_UINT_16:
      return 2;
    case QNN_DATATYPE_SFIXED_POINT_8:
    case QNN_DATATYPE_INT_8:
    case QNN_DATATYPE_UFIXED_POINT_8:
    case QNN_DATATYPE_UINT_8:
      return 1;
    default:
      break;
  }
  throw std::runtime_error("Unsupported QNN tensor datatype size: " + DataTypeToString(dt));
}

struct TensorMeta {
  Qnn_TensorVersion_t tensor_version = QNN_TENSOR_VERSION_1;
  uint32_t id = 0;
  std::string name;
  Qnn_TensorType_t type = QNN_TENSOR_TYPE_UNDEFINED;
  Qnn_TensorDataFormat_t data_format = QNN_TENSOR_DATA_FORMAT_FLAT_BUFFER;
  Qnn_DataType_t data_type = QNN_DATATYPE_UNDEFINED;
  Qnn_QuantizeParams_t quant = QNN_QUANTIZE_PARAMS_INIT;
  std::vector<uint32_t> dims;
};

struct GraphMeta {
  std::string name;
  std::vector<TensorMeta> inputs;
  std::vector<TensorMeta> outputs;
};

static std::string DimsToString(const std::vector<uint32_t>& dims) {
  std::ostringstream oss;
  oss << "[";
  for (size_t i = 0; i < dims.size(); ++i) {
    if (i) oss << "x";
    oss << dims[i];
  }
  oss << "]";
  return oss.str();
}

static std::string DataTypeToString(Qnn_DataType_t dt) {
  switch (dt) {
    case QNN_DATATYPE_FLOAT_32: return "FLOAT_32";
    case QNN_DATATYPE_FLOAT_16: return "FLOAT_16";
    case QNN_DATATYPE_INT_32: return "INT_32";
    case QNN_DATATYPE_UINT_32: return "UINT_32";
    case QNN_DATATYPE_INT_16: return "INT_16";
    case QNN_DATATYPE_UINT_16: return "UINT_16";
    case QNN_DATATYPE_INT_8: return "INT_8";
    case QNN_DATATYPE_UINT_8: return "UINT_8";
    case QNN_DATATYPE_SFIXED_POINT_16: return "SFIXED_POINT_16";
    case QNN_DATATYPE_UFIXED_POINT_16: return "UFIXED_POINT_16";
    case QNN_DATATYPE_SFIXED_POINT_8: return "SFIXED_POINT_8";
    case QNN_DATATYPE_UFIXED_POINT_8: return "UFIXED_POINT_8";
    default: break;
  }
  std::ostringstream oss;
  oss << "0x" << std::hex << static_cast<uint32_t>(dt);
  return oss.str();
}

static TensorLayout4D InferLatentLayout(const TensorMeta& meta) {
  if (meta.dims.size() != 4 || meta.dims[0] != 1) {
    throw std::runtime_error("Expected 4D latent tensor with batch=1 for tensor: " + meta.name +
                             " dims=" + DimsToString(meta.dims));
  }
  if (meta.dims[1] == static_cast<uint32_t>(kLatentC) &&
      meta.dims[2] == static_cast<uint32_t>(kLatentH) &&
      meta.dims[3] == static_cast<uint32_t>(kLatentW)) {
    return TensorLayout4D::NCHW;
  }
  if (meta.dims[1] == static_cast<uint32_t>(kLatentH) &&
      meta.dims[2] == static_cast<uint32_t>(kLatentW) &&
      meta.dims[3] == static_cast<uint32_t>(kLatentC)) {
    return TensorLayout4D::NHWC;
  }
  throw std::runtime_error("Unsupported latent layout for tensor: " + meta.name +
                           " dims=" + DimsToString(meta.dims));
}

static TensorLayout4D InferImageLayout(const TensorMeta& meta) {
  if (meta.dims.size() != 4 || meta.dims[0] != 1) {
    throw std::runtime_error("Expected 4D image tensor with batch=1 for tensor: " + meta.name +
                             " dims=" + DimsToString(meta.dims));
  }
  if (meta.dims[1] == static_cast<uint32_t>(kImageC) &&
      meta.dims[2] == static_cast<uint32_t>(kImageH) &&
      meta.dims[3] == static_cast<uint32_t>(kImageW)) {
    return TensorLayout4D::NCHW;
  }
  if (meta.dims[1] == static_cast<uint32_t>(kImageH) &&
      meta.dims[2] == static_cast<uint32_t>(kImageW) &&
      meta.dims[3] == static_cast<uint32_t>(kImageC)) {
    return TensorLayout4D::NHWC;
  }
  throw std::runtime_error("Unsupported image layout for tensor: " + meta.name +
                           " dims=" + DimsToString(meta.dims));
}

static const TensorMeta* FindTensorMetaByName(const std::vector<TensorMeta>& tensors,
                                              const std::string& name) {
  for (const auto& tensor : tensors) {
    if (tensor.name == name) return &tensor;
  }
  return nullptr;
}

static std::string LayoutToString(TensorLayout4D layout) {
  return layout == TensorLayout4D::NCHW ? "NCHW" : "NHWC";
}

static bool HasScaleOffsetQuant(const Qnn_QuantizeParams_t& q) {
  return q.encodingDefinition == QNN_DEFINITION_DEFINED &&
         q.quantizationEncoding == QNN_QUANTIZATION_ENCODING_SCALE_OFFSET;
}

static std::string QuantToString(const Qnn_QuantizeParams_t& q) {
  if (q.encodingDefinition == QNN_DEFINITION_UNDEFINED &&
      q.quantizationEncoding == QNN_QUANTIZATION_ENCODING_UNDEFINED) {
    return "none";
  }
  if (HasScaleOffsetQuant(q)) {
    std::ostringstream oss;
    oss << "scale_offset(scale=" << q.scaleOffsetEncoding.scale
        << ", offset=" << q.scaleOffsetEncoding.offset << ")";
    return oss.str();
  }
  std::ostringstream oss;
  oss << "unsupported(encDef=" << static_cast<int>(q.encodingDefinition)
      << ", quantEnc=" << static_cast<int>(q.quantizationEncoding) << ")";
  return oss.str();
}

static TensorMeta TensorMetaFromQnn(const Qnn_Tensor_t& t) {
  TensorMeta meta;
  meta.tensor_version = t.version;

  if (t.version == QNN_TENSOR_VERSION_1) {
    meta.id = t.v1.id;
    meta.name = (t.v1.name ? t.v1.name : "");
    meta.type = t.v1.type;
    meta.data_format = t.v1.dataFormat;
    meta.data_type = t.v1.dataType;
    meta.quant = t.v1.quantizeParams;
    meta.dims.assign(t.v1.dimensions, t.v1.dimensions + t.v1.rank);
  } else if (t.version == QNN_TENSOR_VERSION_2) {
    meta.id = t.v2.id;
    meta.name = (t.v2.name ? t.v2.name : "");
    meta.type = t.v2.type;
    meta.data_format = t.v2.dataFormat;
    meta.data_type = t.v2.dataType;
    meta.quant = t.v2.quantizeParams;
    meta.dims.assign(t.v2.dimensions, t.v2.dimensions + t.v2.rank);
  } else {
    throw std::runtime_error("Unsupported tensor version in metadata");
  }

  if (meta.data_format != QNN_TENSOR_DATA_FORMAT_FLAT_BUFFER) {
    throw std::runtime_error("Only flat-buffer tensors are supported by this app");
  }

  const bool quant_ok = (meta.quant.encodingDefinition == QNN_DEFINITION_UNDEFINED &&
                         meta.quant.quantizationEncoding == QNN_QUANTIZATION_ENCODING_UNDEFINED) ||
                        HasScaleOffsetQuant(meta.quant);
  if (!quant_ok) {
    throw std::runtime_error("Unsupported quantization encoding in tensor: " + meta.name);
  }
  return meta;
}

static void GetGraphsFromBinaryInfo(const QnnSystemContext_BinaryInfo_t* binary_info,
                                    const QnnSystemContext_GraphInfo_t** graphs,
                                    uint32_t* num_graphs) {
  switch (binary_info->version) {
    case QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_1:
      *graphs = binary_info->contextBinaryInfoV1.graphs;
      *num_graphs = binary_info->contextBinaryInfoV1.numGraphs;
      return;
    case QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_2:
      *graphs = binary_info->contextBinaryInfoV2.graphs;
      *num_graphs = binary_info->contextBinaryInfoV2.numGraphs;
      return;
    case QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_3:
      *graphs = binary_info->contextBinaryInfoV3.graphs;
      *num_graphs = binary_info->contextBinaryInfoV3.numGraphs;
      return;
    default:
      throw std::runtime_error("Unsupported binary info version");
  }
}

struct GraphInfoView {
  std::string name;
  const Qnn_Tensor_t* inputs = nullptr;
  uint32_t input_count = 0;
  const Qnn_Tensor_t* outputs = nullptr;
  uint32_t output_count = 0;
};

static GraphInfoView GetGraphInfoView(const QnnSystemContext_GraphInfo_t& g) {
  GraphInfoView view;
  switch (g.version) {
    case QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_1:
      view.name = (g.graphInfoV1.graphName ? g.graphInfoV1.graphName : "");
      view.inputs = g.graphInfoV1.graphInputs;
      view.input_count = g.graphInfoV1.numGraphInputs;
      view.outputs = g.graphInfoV1.graphOutputs;
      view.output_count = g.graphInfoV1.numGraphOutputs;
      return view;
    case QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_2:
      view.name = (g.graphInfoV2.graphName ? g.graphInfoV2.graphName : "");
      view.inputs = g.graphInfoV2.graphInputs;
      view.input_count = g.graphInfoV2.numGraphInputs;
      view.outputs = g.graphInfoV2.graphOutputs;
      view.output_count = g.graphInfoV2.numGraphOutputs;
      return view;
    case QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_3:
      view.name = (g.graphInfoV3.graphName ? g.graphInfoV3.graphName : "");
      view.inputs = g.graphInfoV3.graphInputs;
      view.input_count = g.graphInfoV3.numGraphInputs;
      view.outputs = g.graphInfoV3.graphOutputs;
      view.output_count = g.graphInfoV3.numGraphOutputs;
      return view;
    default:
      throw std::runtime_error("Unsupported graph info version");
  }
}

static GraphMeta ExtractFirstGraphMeta(const QnnSystemContext_BinaryInfo_t* binary_info) {
  if (!binary_info) {
    throw std::runtime_error("Null binary info");
  }

  const QnnSystemContext_GraphInfo_t* graphs = nullptr;
  uint32_t num_graphs = 0;
  GetGraphsFromBinaryInfo(binary_info, &graphs, &num_graphs);

  if (!graphs || num_graphs == 0) {
    throw std::runtime_error("No graphs found in context binary info");
  }

  const auto& g = graphs[0];
  GraphMeta meta;
  const GraphInfoView graph_view = GetGraphInfoView(g);
  meta.name = graph_view.name;

  if (meta.name.empty()) {
    throw std::runtime_error("Graph name is empty in binary metadata");
  }

  meta.inputs.reserve(graph_view.input_count);
  for (uint32_t i = 0; i < graph_view.input_count; ++i) {
    meta.inputs.push_back(TensorMetaFromQnn(graph_view.inputs[i]));
  }

  meta.outputs.reserve(graph_view.output_count);
  for (uint32_t i = 0; i < graph_view.output_count; ++i) {
    meta.outputs.push_back(TensorMetaFromQnn(graph_view.outputs[i]));
  }

  return meta;
}

using QnnInterfaceGetProvidersFn_t =
    Qnn_ErrorHandle_t (*)(const QnnInterface_t*** providerList, uint32_t* numProviders);
using QnnSystemInterfaceGetProvidersFn_t =
    Qnn_ErrorHandle_t (*)(const QnnSystemInterface_t*** providerList, uint32_t* numProviders);

template <typename FnT>
static FnT ResolveSymbol(void* lib_handle, const char* symbol) {
  if (!lib_handle) {
    throw std::runtime_error("ResolveSymbol called with null library handle");
  }
  dlerror();
  void* ptr = dlsym(lib_handle, symbol);
  const char* err = dlerror();
  if (err || !ptr) {
    std::string msg = "dlsym failed for symbol: ";
    msg += symbol;
    if (err) {
      msg += " (";
      msg += err;
      msg += ")";
    }
    throw std::runtime_error(msg);
  }
  return reinterpret_cast<FnT>(ptr);
}

class QnnRuntime {
 public:
  QnnRuntime(const std::string& runtime_dir,
             const std::string& backend_lib,
             const std::string& system_lib,
             Logger& log)
      : runtime_dir_(runtime_dir), log_(log) {
    const std::string backend_path = runtime_dir_ + "/" + backend_lib;
    const std::string system_path = runtime_dir_ + "/" + system_lib;

    LoadBackendLibrary(backend_path);
    ResolveBackendInterface();
    LoadSystemLibrary(system_path);
    ResolveSystemInterface();
    ValidateInterfacePointers();
    CreateBackendAndDevice();

    log_.Log("Initialized QNN runtime interface and created backend/device.");
  }

  ~QnnRuntime() {
    if (qnn_interface_.deviceFree && device_handle_) {
      qnn_interface_.deviceFree(device_handle_);
      device_handle_ = nullptr;
    }
    if (backend_handle_ && qnn_interface_.backendFree) {
      qnn_interface_.backendFree(backend_handle_);
      backend_handle_ = nullptr;
    }
    if (system_lib_handle_) {
      dlclose(system_lib_handle_);
      system_lib_handle_ = nullptr;
    }
    if (backend_lib_handle_) {
      dlclose(backend_lib_handle_);
      backend_lib_handle_ = nullptr;
    }
  }

  Qnn_BackendHandle_t backend_handle() const { return backend_handle_; }
  Qnn_DeviceHandle_t device_handle() const { return device_handle_; }
  const QNN_INTERFACE_VER_TYPE& iface() const { return qnn_interface_; }
  const QNN_SYSTEM_INTERFACE_VER_TYPE& sys_iface() const { return qnn_system_interface_; }

 private:
  void LoadBackendLibrary(const std::string& backend_path) {
    backend_lib_handle_ = dlopen(backend_path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!backend_lib_handle_) {
      throw std::runtime_error(std::string("Failed to load backend lib: ") + dlerror());
    }
  }

  void ResolveBackendInterface() {
    auto get_providers =
        ResolveSymbol<QnnInterfaceGetProvidersFn_t>(backend_lib_handle_, "QnnInterface_getProviders");

    const QnnInterface_t** providers = nullptr;
    uint32_t nproviders = 0;
    const Qnn_ErrorHandle_t status = get_providers(&providers, &nproviders);
    if (status != QNN_SUCCESS || !providers || nproviders == 0) {
      throw std::runtime_error("Failed to get QNN interface providers from backend lib");
    }

    for (uint32_t i = 0; i < nproviders; ++i) {
      const auto* p = providers[i];
      if (!p) continue;
      if (p->apiVersion.coreApiVersion.major == QNN_API_VERSION_MAJOR &&
          p->apiVersion.coreApiVersion.minor >= QNN_API_VERSION_MINOR) {
        qnn_interface_ = p->QNN_INTERFACE_VER_NAME;
        return;
      }
    }
    throw std::runtime_error("No compatible QNN backend interface provider found");
  }

  void LoadSystemLibrary(const std::string& system_path) {
    system_lib_handle_ = dlopen(system_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!system_lib_handle_) {
      throw std::runtime_error(std::string("Failed to load system lib: ") + dlerror());
    }
  }

  void ResolveSystemInterface() {
    auto get_sys_providers = ResolveSymbol<QnnSystemInterfaceGetProvidersFn_t>(
        system_lib_handle_, "QnnSystemInterface_getProviders");

    const QnnSystemInterface_t** sys_providers = nullptr;
    uint32_t nsys = 0;
    const Qnn_ErrorHandle_t status = get_sys_providers(&sys_providers, &nsys);
    if (status != QNN_SUCCESS || !sys_providers || nsys == 0) {
      throw std::runtime_error("Failed to get QNN system interface providers from system lib");
    }

    for (uint32_t i = 0; i < nsys; ++i) {
      const auto* p = sys_providers[i];
      if (!p) continue;
      if (p->systemApiVersion.major == QNN_SYSTEM_API_VERSION_MAJOR &&
          p->systemApiVersion.minor >= QNN_SYSTEM_API_VERSION_MINOR) {
        qnn_system_interface_ = p->QNN_SYSTEM_INTERFACE_VER_NAME;
        return;
      }
    }
    throw std::runtime_error("No compatible QNN system interface provider found");
  }

  void ValidateInterfacePointers() const {
    if (!qnn_interface_.backendCreate || !qnn_interface_.contextCreateFromBinary ||
        !qnn_interface_.graphRetrieve || !qnn_interface_.graphExecute ||
        !qnn_interface_.backendFree || !qnn_interface_.contextFree) {
      throw std::runtime_error("Backend interface is missing required function pointers");
    }
    if (!qnn_system_interface_.systemContextCreate ||
        (!qnn_system_interface_.systemContextGetBinaryInfo &&
         !qnn_system_interface_.systemContextGetMetaData) ||
        !qnn_system_interface_.systemContextFree) {
      throw std::runtime_error("System interface is missing required function pointers");
    }
  }

  void CreateBackendAndDevice() {
    if (qnn_interface_.backendCreate(nullptr, nullptr, &backend_handle_) != QNN_BACKEND_NO_ERROR) {
      throw std::runtime_error("QNN backendCreate failed");
    }

    if (!qnn_interface_.deviceCreate) return;
    const auto dev_status = qnn_interface_.deviceCreate(nullptr, nullptr, &device_handle_);
    if (dev_status != QNN_SUCCESS && dev_status != QNN_DEVICE_ERROR_UNSUPPORTED_FEATURE) {
      throw std::runtime_error("QNN deviceCreate failed");
    }
  }

  std::string runtime_dir_;
  Logger& log_;
  void* backend_lib_handle_ = nullptr;
  void* system_lib_handle_ = nullptr;

  QNN_INTERFACE_VER_TYPE qnn_interface_ = QNN_INTERFACE_VER_TYPE_INIT;
  QNN_SYSTEM_INTERFACE_VER_TYPE qnn_system_interface_ = QNN_SYSTEM_INTERFACE_VER_TYPE_INIT;

  Qnn_BackendHandle_t backend_handle_ = nullptr;
  Qnn_DeviceHandle_t device_handle_ = nullptr;
  Qnn_LogHandle_t log_handle_ = nullptr;
};

struct TensorRuntime {
  TensorMeta meta;
  std::vector<uint8_t> buffer;
  Qnn_Tensor_t tensor = QNN_TENSOR_INIT;
};

static Qnn_Tensor_t BuildQnnTensor(const TensorMeta& meta, void* data_ptr, uint32_t data_size) {
  Qnn_Tensor_t t = QNN_TENSOR_INIT;
  if (meta.tensor_version == QNN_TENSOR_VERSION_2) {
    t.version = QNN_TENSOR_VERSION_2;
    t.v2.id = meta.id;
    t.v2.name = meta.name.c_str();
    t.v2.type = meta.type;
    t.v2.dataFormat = meta.data_format;
    t.v2.dataType = meta.data_type;
    t.v2.quantizeParams = meta.quant;
    t.v2.rank = static_cast<uint32_t>(meta.dims.size());
    t.v2.dimensions = const_cast<uint32_t*>(meta.dims.data());
    t.v2.memType = QNN_TENSORMEMTYPE_RAW;
    t.v2.clientBuf.data = data_ptr;
    t.v2.clientBuf.dataSize = data_size;
    t.v2.isDynamicDimensions = nullptr;
    t.v2.sparseParams = QNN_SPARSE_PARAMS_INIT;
    t.v2.isProduced = 0;
  } else {
    t.version = QNN_TENSOR_VERSION_1;
    t.v1.id = meta.id;
    t.v1.name = meta.name.c_str();
    t.v1.type = meta.type;
    t.v1.dataFormat = meta.data_format;
    t.v1.dataType = meta.data_type;
    t.v1.quantizeParams = meta.quant;
    t.v1.rank = static_cast<uint32_t>(meta.dims.size());
    t.v1.dimensions = const_cast<uint32_t*>(meta.dims.data());
    t.v1.memType = QNN_TENSORMEMTYPE_RAW;
    t.v1.clientBuf.data = data_ptr;
    t.v1.clientBuf.dataSize = data_size;
  }
  return t;
}

static inline float QuantizeScaleOffset(float real, bool has_quant, float scale, int32_t offset) {
  return has_quant ? (real / scale - static_cast<float>(offset)) : real;
}

static inline float DequantizeScaleOffset(float quantized, bool has_quant, float scale, int32_t offset) {
  return has_quant ? (quantized + static_cast<float>(offset)) * scale : quantized;
}

struct QuantParams {
  bool has_q = false;
  float scale = 1.0f;
  int32_t offset = 0;
};

static QuantParams ResolveInputQuantParams(const TensorMeta& meta) {
  QuantParams qp;
  qp.has_q = HasScaleOffsetQuant(meta.quant);
  if (!qp.has_q) {
    return qp;
  }
  qp.scale = meta.quant.scaleOffsetEncoding.scale;
  qp.offset = meta.quant.scaleOffsetEncoding.offset;
  if (!(qp.scale > 0.0f)) {
    throw std::runtime_error("Invalid quant scale for tensor: " + meta.name);
  }
  return qp;
}

static QuantParams ResolveOutputQuantParams(const TensorMeta& meta) {
  QuantParams qp;
  qp.has_q = HasScaleOffsetQuant(meta.quant);
  if (!qp.has_q) {
    return qp;
  }
  qp.scale = meta.quant.scaleOffsetEncoding.scale;
  qp.offset = meta.quant.scaleOffsetEncoding.offset;
  return qp;
}

static void FillFloat32Buffer(const std::vector<float>& src, std::vector<uint8_t>& dst) {
  dst.resize(src.size() * sizeof(float));
  std::memcpy(dst.data(), src.data(), dst.size());
}

static void FillFloat16Buffer(const std::vector<float>& src, std::vector<uint8_t>& dst) {
  dst.resize(src.size() * sizeof(uint16_t));
  auto* p = reinterpret_cast<uint16_t*>(dst.data());
  for (size_t i = 0; i < src.size(); ++i) {
    p[i] = Float32ToFloat16(src[i]);
  }
}

static void FillInt32Buffer(const std::vector<float>& src, std::vector<uint8_t>& dst) {
  dst.resize(src.size() * sizeof(int32_t));
  auto* p = reinterpret_cast<int32_t*>(dst.data());
  for (size_t i = 0; i < src.size(); ++i) {
    p[i] = static_cast<int32_t>(std::llround(src[i]));
  }
}

static void FillUint32Buffer(const std::vector<float>& src, std::vector<uint8_t>& dst) {
  dst.resize(src.size() * sizeof(uint32_t));
  auto* p = reinterpret_cast<uint32_t*>(dst.data());
  for (size_t i = 0; i < src.size(); ++i) {
    const long long q = static_cast<long long>(std::llround(std::max(0.0f, src[i])));
    p[i] = static_cast<uint32_t>(std::min<long long>(q, std::numeric_limits<uint32_t>::max()));
  }
}

static void FillUint16Buffer(const std::vector<float>& src,
                             const QuantParams& qp,
                             std::vector<uint8_t>& dst) {
  dst.resize(src.size() * sizeof(uint16_t));
  auto* p = reinterpret_cast<uint16_t*>(dst.data());
  for (size_t i = 0; i < src.size(); ++i) {
    const float qf = QuantizeScaleOffset(src[i], qp.has_q, qp.scale, qp.offset);
    long long qi = static_cast<long long>(std::llround(qf));
    qi = std::max<long long>(0, std::min<long long>(65535, qi));
    p[i] = static_cast<uint16_t>(qi);
  }
}

static void FillInt16Buffer(const std::vector<float>& src,
                            const QuantParams& qp,
                            std::vector<uint8_t>& dst) {
  dst.resize(src.size() * sizeof(int16_t));
  auto* p = reinterpret_cast<int16_t*>(dst.data());
  for (size_t i = 0; i < src.size(); ++i) {
    const float qf = QuantizeScaleOffset(src[i], qp.has_q, qp.scale, qp.offset);
    const long long qi = static_cast<long long>(std::llround(qf));
    p[i] = static_cast<int16_t>(
        std::clamp<long long>(qi, std::numeric_limits<int16_t>::min(), std::numeric_limits<int16_t>::max()));
  }
}

static void FillUint8Buffer(const std::vector<float>& src,
                            const QuantParams& qp,
                            std::vector<uint8_t>& dst) {
  dst.resize(src.size() * sizeof(uint8_t));
  auto* p = reinterpret_cast<uint8_t*>(dst.data());
  for (size_t i = 0; i < src.size(); ++i) {
    const float qf = QuantizeScaleOffset(src[i], qp.has_q, qp.scale, qp.offset);
    const long long qi = static_cast<long long>(std::llround(qf));
    p[i] = static_cast<uint8_t>(std::clamp<long long>(qi, 0, 255));
  }
}

static void FillInt8Buffer(const std::vector<float>& src,
                           const QuantParams& qp,
                           std::vector<uint8_t>& dst) {
  dst.resize(src.size() * sizeof(int8_t));
  auto* p = reinterpret_cast<int8_t*>(dst.data());
  for (size_t i = 0; i < src.size(); ++i) {
    const float qf = QuantizeScaleOffset(src[i], qp.has_q, qp.scale, qp.offset);
    const long long qi = static_cast<long long>(std::llround(qf));
    p[i] = static_cast<int8_t>(
        std::clamp<long long>(qi, std::numeric_limits<int8_t>::min(), std::numeric_limits<int8_t>::max()));
  }
}

static void CopyFloat32ToFloat(const std::vector<uint8_t>& src, size_t elem_count, std::vector<float>* out) {
  const auto* p = reinterpret_cast<const float*>(src.data());
  std::copy(p, p + elem_count, out->begin());
}

static void CopyFloat16ToFloat(const std::vector<uint8_t>& src, size_t elem_count, std::vector<float>* out) {
  const auto* p = reinterpret_cast<const uint16_t*>(src.data());
  for (size_t i = 0; i < elem_count; ++i) {
    (*out)[i] = Float16ToFloat32(p[i]);
  }
}

template <typename T>
static void CopyIntegerToFloat(const std::vector<uint8_t>& src, size_t elem_count, std::vector<float>* out) {
  const auto* p = reinterpret_cast<const T*>(src.data());
  for (size_t i = 0; i < elem_count; ++i) {
    (*out)[i] = static_cast<float>(p[i]);
  }
}

template <typename T>
static void DequantizedIntegerToFloat(const std::vector<uint8_t>& src,
                                      size_t elem_count,
                                      const QuantParams& qp,
                                      std::vector<float>* out) {
  const auto* p = reinterpret_cast<const T*>(src.data());
  for (size_t i = 0; i < elem_count; ++i) {
    (*out)[i] = DequantizeScaleOffset(static_cast<float>(p[i]), qp.has_q, qp.scale, qp.offset);
  }
}

static void FillTensorBufferFromFloat(const TensorMeta& meta,
                                      const std::vector<float>& src,
                                      std::vector<uint8_t>& dst) {
  const size_t elem_count = NumElements(meta.dims);
  if (src.size() != elem_count) {
    std::ostringstream oss;
    oss << "Input tensor size mismatch for " << meta.name << ": got " << src.size()
        << ", expected " << elem_count;
    throw std::runtime_error(oss.str());
  }

  const QuantParams qp = ResolveInputQuantParams(meta);

  switch (meta.data_type) {
    case QNN_DATATYPE_FLOAT_32: {
      FillFloat32Buffer(src, dst);
      return;
    }
    case QNN_DATATYPE_FLOAT_16: {
      FillFloat16Buffer(src, dst);
      return;
    }
    case QNN_DATATYPE_INT_32: {
      FillInt32Buffer(src, dst);
      return;
    }
    case QNN_DATATYPE_UINT_32: {
      FillUint32Buffer(src, dst);
      return;
    }
    case QNN_DATATYPE_UFIXED_POINT_16:
    case QNN_DATATYPE_UINT_16: {
      FillUint16Buffer(src, qp, dst);
      return;
    }
    case QNN_DATATYPE_SFIXED_POINT_16:
    case QNN_DATATYPE_INT_16: {
      FillInt16Buffer(src, qp, dst);
      return;
    }
    case QNN_DATATYPE_UFIXED_POINT_8:
    case QNN_DATATYPE_UINT_8: {
      FillUint8Buffer(src, qp, dst);
      return;
    }
    case QNN_DATATYPE_SFIXED_POINT_8:
    case QNN_DATATYPE_INT_8: {
      FillInt8Buffer(src, qp, dst);
      return;
    }
    default:
      throw std::runtime_error("Unsupported input tensor datatype for conversion: " + meta.name +
                               " dtype=" + DataTypeToString(meta.data_type));
  }
}

static std::vector<float> TensorBufferToFloat(const TensorMeta& meta,
                                              const std::vector<uint8_t>& src) {
  const size_t elem_count = NumElements(meta.dims);
  const size_t expected_bytes = elem_count * DataTypeSize(meta.data_type);
  if (src.size() != expected_bytes) {
    throw std::runtime_error("Unexpected output buffer size for tensor: " + meta.name);
  }

  const QuantParams qp = ResolveOutputQuantParams(meta);

  std::vector<float> out(elem_count);
  switch (meta.data_type) {
    case QNN_DATATYPE_FLOAT_32: {
      CopyFloat32ToFloat(src, elem_count, &out);
      return out;
    }
    case QNN_DATATYPE_FLOAT_16: {
      CopyFloat16ToFloat(src, elem_count, &out);
      return out;
    }
    case QNN_DATATYPE_INT_32: {
      CopyIntegerToFloat<int32_t>(src, elem_count, &out);
      return out;
    }
    case QNN_DATATYPE_UINT_32: {
      CopyIntegerToFloat<uint32_t>(src, elem_count, &out);
      return out;
    }
    case QNN_DATATYPE_UFIXED_POINT_16:
    case QNN_DATATYPE_UINT_16: {
      DequantizedIntegerToFloat<uint16_t>(src, elem_count, qp, &out);
      return out;
    }
    case QNN_DATATYPE_SFIXED_POINT_16:
    case QNN_DATATYPE_INT_16: {
      DequantizedIntegerToFloat<int16_t>(src, elem_count, qp, &out);
      return out;
    }
    case QNN_DATATYPE_UFIXED_POINT_8:
    case QNN_DATATYPE_UINT_8: {
      DequantizedIntegerToFloat<uint8_t>(src, elem_count, qp, &out);
      return out;
    }
    case QNN_DATATYPE_SFIXED_POINT_8:
    case QNN_DATATYPE_INT_8: {
      DequantizedIntegerToFloat<int8_t>(src, elem_count, qp, &out);
      return out;
    }
    default:
      throw std::runtime_error("Unsupported output tensor datatype for conversion: " + meta.name +
                               " dtype=" + DataTypeToString(meta.data_type));
  }
}

class ContextRunner {
 public:
  ContextRunner(QnnRuntime& runtime,
                const std::string& context_bin_path,
                const std::string& label,
                Logger& log)
      : runtime_(runtime), label_(label), log_(log) {
    context_binary_ = ReadBinaryFile(context_bin_path);

    QnnSystemContext_Handle_t sys_ctx = nullptr;
    if (runtime_.sys_iface().systemContextCreate(&sys_ctx) != QNN_SUCCESS || !sys_ctx) {
      throw std::runtime_error("systemContextCreate failed for " + label_);
    }

    const QnnSystemContext_BinaryInfo_t* binary_info = nullptr;
    Qnn_ContextBinarySize_t binary_info_size = 0;
    Qnn_ErrorHandle_t info_status = QNN_SYSTEM_CONTEXT_ERROR_OPERATION_FAILED;

    if (runtime_.sys_iface().systemContextGetBinaryInfo) {
      info_status = runtime_.sys_iface().systemContextGetBinaryInfo(
          sys_ctx,
          static_cast<void*>(context_binary_.data()),
          context_binary_.size(),
          &binary_info,
          &binary_info_size);
    }
    if ((info_status != QNN_SUCCESS || !binary_info) && runtime_.sys_iface().systemContextGetMetaData) {
      info_status = runtime_.sys_iface().systemContextGetMetaData(
          sys_ctx,
          static_cast<const void*>(context_binary_.data()),
          context_binary_.size(),
          &binary_info);
    }
    if (info_status != QNN_SUCCESS || !binary_info) {
      runtime_.sys_iface().systemContextFree(sys_ctx);
      throw std::runtime_error("systemContextGetBinaryInfo/systemContextGetMetaData failed for " +
                               label_);
    }

    graph_meta_ = ExtractFirstGraphMeta(binary_info);
    runtime_.sys_iface().systemContextFree(sys_ctx);

    const auto create_status = runtime_.iface().contextCreateFromBinary(
        runtime_.backend_handle(),
        runtime_.device_handle(),
        nullptr,
        static_cast<void*>(context_binary_.data()),
        context_binary_.size(),
        &context_handle_,
        nullptr);
    if (create_status != QNN_SUCCESS || !context_handle_) {
      throw std::runtime_error("contextCreateFromBinary failed for " + label_);
    }

    const auto retrieve_status =
        runtime_.iface().graphRetrieve(context_handle_, graph_meta_.name.c_str(), &graph_handle_);
    if (retrieve_status != QNN_SUCCESS || !graph_handle_) {
      throw std::runtime_error("graphRetrieve failed for graph: " + graph_meta_.name);
    }

    PrepareTensorRuntimes();

    log_.Log("[" + label_ + "] loaded graph='" + graph_meta_.name + "' with " +
             std::to_string(graph_meta_.inputs.size()) + " input(s), " +
             std::to_string(graph_meta_.outputs.size()) + " output(s)");
    for (size_t i = 0; i < graph_meta_.inputs.size(); ++i) {
      const auto& meta = graph_meta_.inputs[i];
      log_.Log("[" + label_ + "]   in[" + std::to_string(i) + "] name=" + meta.name +
               " dims=" + DimsToString(meta.dims) +
               " dtype=" + DataTypeToString(meta.data_type) +
               " quant=" + QuantToString(meta.quant));
    }
    for (size_t i = 0; i < graph_meta_.outputs.size(); ++i) {
      const auto& meta = graph_meta_.outputs[i];
      log_.Log("[" + label_ + "]   out[" + std::to_string(i) + "] name=" + meta.name +
               " dims=" + DimsToString(meta.dims) +
               " dtype=" + DataTypeToString(meta.data_type) +
               " quant=" + QuantToString(meta.quant));
    }
  }

  ~ContextRunner() {
    if (context_handle_) {
      runtime_.iface().contextFree(context_handle_, nullptr);
      context_handle_ = nullptr;
    }
  }

  std::unordered_map<std::string, std::vector<float>> Execute(
      const std::unordered_map<std::string, std::vector<float>>& input_by_name,
      double* elapsed_s = nullptr) {
    for (auto& tr : input_tensors_) {
      auto it = input_by_name.find(tr.meta.name);
      if (it == input_by_name.end()) {
        throw std::runtime_error("Missing required input tensor: " + tr.meta.name +
                                 " for " + label_);
      }
      FillTensorBufferFromFloat(tr.meta, it->second, tr.buffer);
      tr.tensor = BuildQnnTensor(tr.meta, tr.buffer.data(), static_cast<uint32_t>(tr.buffer.size()));
    }

    for (auto& tr : output_tensors_) {
      tr.tensor = BuildQnnTensor(tr.meta, tr.buffer.data(), static_cast<uint32_t>(tr.buffer.size()));
    }

    std::vector<Qnn_Tensor_t> in_exec;
    in_exec.reserve(input_tensors_.size());
    for (const auto& tr : input_tensors_) {
      in_exec.push_back(tr.tensor);
    }

    std::vector<Qnn_Tensor_t> out_exec;
    out_exec.reserve(output_tensors_.size());
    for (const auto& tr : output_tensors_) {
      out_exec.push_back(tr.tensor);
    }

    const auto t0 = std::chrono::steady_clock::now();
    const auto exec_status = runtime_.iface().graphExecute(
        graph_handle_,
        in_exec.data(),
        static_cast<uint32_t>(in_exec.size()),
        out_exec.data(),
        static_cast<uint32_t>(out_exec.size()),
        nullptr,
        nullptr);
    const auto t1 = std::chrono::steady_clock::now();

    if (exec_status != QNN_GRAPH_NO_ERROR) {
      std::ostringstream oss;
      oss << "graphExecute failed for " << label_ << " (status=" << exec_status << ")";
      throw std::runtime_error(oss.str());
    }

    if (elapsed_s) {
      *elapsed_s = std::chrono::duration<double>(t1 - t0).count();
    }

    std::unordered_map<std::string, std::vector<float>> out;
    out.reserve(output_tensors_.size());
    for (const auto& tr : output_tensors_) {
      out[tr.meta.name] = TensorBufferToFloat(tr.meta, tr.buffer);
    }
    return out;
  }

  const GraphMeta& graph_meta() const { return graph_meta_; }

 private:
  void PrepareTensorRuntimes() {
    input_tensors_.clear();
    input_tensors_.reserve(graph_meta_.inputs.size());
    for (const auto& meta : graph_meta_.inputs) {
      TensorRuntime tr;
      tr.meta = meta;
      tr.buffer.resize(NumElements(meta.dims) * DataTypeSize(meta.data_type));
      tr.tensor = BuildQnnTensor(tr.meta, tr.buffer.data(), static_cast<uint32_t>(tr.buffer.size()));
      input_tensors_.push_back(std::move(tr));
    }

    output_tensors_.clear();
    output_tensors_.reserve(graph_meta_.outputs.size());
    for (const auto& meta : graph_meta_.outputs) {
      TensorRuntime tr;
      tr.meta = meta;
      tr.buffer.resize(NumElements(meta.dims) * DataTypeSize(meta.data_type));
      tr.tensor = BuildQnnTensor(tr.meta, tr.buffer.data(), static_cast<uint32_t>(tr.buffer.size()));
      output_tensors_.push_back(std::move(tr));
    }
  }

  QnnRuntime& runtime_;
  std::string label_;
  Logger& log_;

  std::vector<uint8_t> context_binary_;
  GraphMeta graph_meta_;
  Qnn_ContextHandle_t context_handle_ = nullptr;
  Qnn_GraphHandle_t graph_handle_ = nullptr;

  std::vector<TensorRuntime> input_tensors_;
  std::vector<TensorRuntime> output_tensors_;
};

static std::string JsonEscape(const std::string& s) {
  std::ostringstream oss;
  for (char c : s) {
    switch (c) {
      case '\\': oss << "\\\\"; break;
      case '"': oss << "\\\""; break;
      case '\n': oss << "\\n"; break;
      case '\r': oss << "\\r"; break;
      case '\t': oss << "\\t"; break;
      default: oss << c; break;
    }
  }
  return oss.str();
}

static void EnsureExists(const std::string& path) {
  if (!fs::exists(path)) {
    throw std::runtime_error("Missing required path: " + path);
  }
}

static std::string ResolveRelativeOrAbsolutePath(const std::string& base_dir,
                                                 const std::string& path) {
  if (path.empty()) return path;
  fs::path p(path);
  if (p.is_absolute()) return p.string();
  return (fs::path(base_dir) / p).string();
}

static bool StartsWith(const std::string& s, const std::string& prefix) {
  return s.size() >= prefix.size() &&
         s.compare(0, prefix.size(), prefix) == 0;
}

static bool EndsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static bool ResolveFromExplicitCandidates(const std::string& base_dir,
                                          const std::vector<std::string>& candidates,
                                          std::vector<std::string>* tried,
                                          std::string* resolved_out) {
  std::unordered_set<std::string> seen;
  for (const std::string& candidate : candidates) {
    if (candidate.empty()) continue;
    const std::string resolved = ResolveRelativeOrAbsolutePath(base_dir, candidate);
    if (!seen.insert(resolved).second) continue;
    tried->push_back(resolved);
    if (fs::exists(resolved)) {
      *resolved_out = resolved;
      return true;
    }
  }
  return false;
}

static std::string ResolveFromWildcardPrefixes(const std::string& base_dir,
                                               const std::vector<std::string>& wildcard_prefixes) {
  if (wildcard_prefixes.empty()) return {};
  std::vector<std::string> wildcard_hits;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(base_dir, ec)) {
    if (ec) break;
    if (!entry.is_regular_file()) continue;
    const std::string filename = entry.path().filename().string();
    if (!EndsWith(filename, ".bin")) continue;
    for (const std::string& prefix : wildcard_prefixes) {
      if (StartsWith(filename, prefix)) {
        wildcard_hits.push_back(entry.path().string());
        break;
      }
    }
  }
  if (wildcard_hits.empty()) return {};
  std::sort(wildcard_hits.begin(), wildcard_hits.end());
  return wildcard_hits.front();
}

static std::string BuildResolveNotFoundMessage(const std::string& label,
                                               const std::vector<std::string>& tried) {
  std::ostringstream oss;
  oss << "Could not find " << label << ". Tried: ";
  for (size_t i = 0; i < tried.size(); ++i) {
    if (i > 0) oss << ", ";
    oss << tried[i];
  }
  return oss.str();
}

static std::string ResolveFirstExistingPath(const std::string& base_dir,
                                            const std::vector<std::string>& candidates,
                                            const std::string& label,
                                            const std::vector<std::string>& wildcard_prefixes = {}) {
  std::vector<std::string> tried;
  std::string resolved;
  if (ResolveFromExplicitCandidates(base_dir, candidates, &tried, &resolved)) {
    return resolved;
  }

  resolved = ResolveFromWildcardPrefixes(base_dir, wildcard_prefixes);
  if (!resolved.empty()) {
    return resolved;
  }

  throw std::runtime_error(BuildResolveNotFoundMessage(label, tried));
}

static void AddMergedPathToken(const std::string& path,
                               std::vector<std::string>* merged,
                               std::unordered_set<std::string>* seen) {
  const std::string t = Trim(path);
  if (t.empty()) return;
  if (seen->insert(t).second) {
    merged->push_back(t);
  }
}

static void AppendInheritedPathTokens(const char* inherited_path,
                                      char delimiter,
                                      std::vector<std::string>* merged,
                                      std::unordered_set<std::string>* seen) {
  if (!inherited_path || inherited_path[0] == '\0') return;
  std::stringstream ss(inherited_path);
  std::string item;
  while (std::getline(ss, item, delimiter)) {
    AddMergedPathToken(item, merged, seen);
  }
}

static std::string JoinPathTokens(const std::vector<std::string>& merged, char delimiter) {
  std::ostringstream oss;
  for (size_t i = 0; i < merged.size(); ++i) {
    if (i > 0) oss << delimiter;
    oss << merged[i];
  }
  return oss.str();
}

static std::string BuildMergedPathValue(const std::vector<std::string>& preferred_paths,
                                        const char* inherited_path,
                                        char delimiter) {
  std::vector<std::string> merged;
  std::unordered_set<std::string> seen;

  for (const std::string& path : preferred_paths) {
    AddMergedPathToken(path, &merged, &seen);
  }
  AppendInheritedPathTokens(inherited_path, delimiter, &merged, &seen);

  return JoinPathTokens(merged, delimiter);
}

static std::string ToLowerCopy(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

static bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return true;
  const std::string hs = ToLowerCopy(haystack);
  const std::string nd = ToLowerCopy(needle);
  return hs.find(nd) != std::string::npos;
}

static bool LooksLikeW8A16Export(const Options& opt,
                                 const std::string& text_context_path,
                                 const std::string& unet_context_path,
                                 const std::string& vae_context_path) {
  const std::array<std::string, 8> probes = {
      opt.runtime_dir,
      opt.text_context,
      opt.unet_context,
      opt.vae_context,
      text_context_path,
      unet_context_path,
      vae_context_path,
      opt.output_image,
  };
  for (const auto& value : probes) {
    if (ContainsCaseInsensitive(value, "w8a16")) {
      return true;
    }
  }
  return false;
}

static PredictionType ParsePredictionType(const std::string& s) {
  if (s == "epsilon") return PredictionType::Epsilon;
  if (s == "v_prediction") return PredictionType::VPrediction;
  throw std::runtime_error("Unsupported prediction_type: " + s +
                           " (expected epsilon or v_prediction)");
}

static std::string PredictionTypeToString(PredictionType p) {
  return p == PredictionType::VPrediction ? "v_prediction" : "epsilon";
}

static std::string NeedArgValue(int argc, char** argv, int& i, const std::string& key) {
  if (i + 1 >= argc) throw std::runtime_error("Missing value for " + key);
  return argv[++i];
}

static bool ParseCorePathArgs(const std::string& a, int argc, char** argv, int& i, Options& opt) {
  if (a == "--runtime_dir") {
    opt.runtime_dir = NeedArgValue(argc, argv, i, a);
  } else if (a == "--qnn_lib_dir") {
    opt.qnn_lib_dir = NeedArgValue(argc, argv, i, a);
  } else if (a == "--tokenizer_dir") {
    opt.tokenizer_dir = NeedArgValue(argc, argv, i, a);
  } else {
    return false;
  }
  return true;
}

static bool ParsePromptAndSamplingArgs(const std::string& a,
                                       int argc,
                                       char** argv,
                                       int& i,
                                       Options& opt) {
  if (a == "--prompt") {
    opt.prompt = NeedArgValue(argc, argv, i, a);
  } else if (a == "--negative_prompt") {
    opt.negative_prompt = NeedArgValue(argc, argv, i, a);
  } else if (a == "--steps") {
    opt.steps = std::stoi(NeedArgValue(argc, argv, i, a));
  } else if (a == "--guidance") {
    opt.guidance = std::stof(NeedArgValue(argc, argv, i, a));
  } else if (a == "--seed") {
    opt.seed = std::stoi(NeedArgValue(argc, argv, i, a));
  } else {
    return false;
  }
  return true;
}

static bool ParseOutputAndBackendArgs(const std::string& a,
                                      int argc,
                                      char** argv,
                                      int& i,
                                      Options& opt) {
  if (a == "--output") {
    opt.output_image = NeedArgValue(argc, argv, i, a);
  } else if (a == "--log_file") {
    opt.log_file = NeedArgValue(argc, argv, i, a);
  } else if (a == "--backend_lib") {
    opt.backend_lib = NeedArgValue(argc, argv, i, a);
  } else if (a == "--system_lib") {
    opt.system_lib = NeedArgValue(argc, argv, i, a);
  } else {
    return false;
  }
  return true;
}

static bool ParseContextAndTuningArgs(const std::string& a,
                                      int argc,
                                      char** argv,
                                      int& i,
                                      Options& opt) {
  if (a == "--text_context") {
    opt.text_context = NeedArgValue(argc, argv, i, a);
  } else if (a == "--unet_context") {
    opt.unet_context = NeedArgValue(argc, argv, i, a);
  } else if (a == "--vae_context") {
    opt.vae_context = NeedArgValue(argc, argv, i, a);
  } else if (a == "--prediction_type") {
    opt.prediction_type = NeedArgValue(argc, argv, i, a);
    opt.prediction_type_user_set = true;
  } else if (a == "--vae_scaling_factor") {
    opt.vae_scaling_factor = std::stof(NeedArgValue(argc, argv, i, a));
    opt.vae_scaling_factor_user_set = true;
  } else {
    return false;
  }
  return true;
}

static void PrintUsageAndExit() {
  std::cout
      << "Usage: sd21_qnn_cpp_direct [options]\n"
      << "  --runtime_dir <path>\n"
      << "  --qnn_lib_dir <path>\n"
      << "  --tokenizer_dir <path>\n"
      << "  --prompt <text>\n"
      << "  --negative_prompt <text>\n"
      << "  --steps <int>\n"
      << "  --guidance <float>\n"
      << "  --seed <int>\n"
      << "  --output <path>\n"
      << "  --log_file <path>\n"
      << "  --backend_lib <file>\n"
      << "  --system_lib <file>\n"
      << "  --text_context <file>\n"
      << "  --unet_context <file>\n"
      << "  --vae_context <file>\n"
      << "  --prediction_type <epsilon|v_prediction>\n"
      << "  --vae_scaling_factor <float>\n";
  std::exit(0);
}

static void ApplyParseDefaultsAndValidate(Options& opt) {
  if (opt.tokenizer_dir.empty()) {
    opt.tokenizer_dir = opt.runtime_dir + "/tokenizer";
  }
  if (opt.qnn_lib_dir.empty()) {
    opt.qnn_lib_dir = opt.runtime_dir;
  }
  if (opt.output_image.empty()) {
    opt.output_image = "/tmp/sd21_generated_direct.ppm";
  }
  if (opt.log_file.empty()) {
    opt.log_file = "/tmp/sd21_run_cpp_direct.log";
  }
  if (opt.steps <= 0) {
    throw std::runtime_error("steps must be > 0");
  }
  if (!(opt.vae_scaling_factor > 0.0f)) {
    throw std::runtime_error("vae_scaling_factor must be > 0");
  }
}

static Options ParseArgs(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];

    if (ParseCorePathArgs(a, argc, argv, i, opt)) continue;
    if (ParsePromptAndSamplingArgs(a, argc, argv, i, opt)) continue;
    if (ParseOutputAndBackendArgs(a, argc, argv, i, opt)) continue;
    if (ParseContextAndTuningArgs(a, argc, argv, i, opt)) continue;
    if (a == "--help" || a == "-h") PrintUsageAndExit();
    throw std::runtime_error("Unknown argument: " + a);
  }

  ApplyParseDefaultsAndValidate(opt);
  return opt;
}

struct ContextPaths {
  std::string text_context_path;
  std::string unet_context_path;
  std::string vae_context_path;
};

struct TokenBuffers {
  std::vector<float> uncond_tokens;
  std::vector<float> cond_tokens;
};

struct LayoutSelection {
  TensorLayout4D unet_input_layout = TensorLayout4D::NCHW;
  TensorLayout4D unet_output_layout = TensorLayout4D::NCHW;
  TensorLayout4D vae_input_layout = TensorLayout4D::NCHW;
  TensorLayout4D vae_output_layout = TensorLayout4D::NHWC;
};

struct TextEmbeddings {
  std::vector<float> uncond;
  std::vector<float> cond;
};

struct ImageOutputs {
  std::vector<float> raw_nhwc;
  std::vector<float> normalized_nhwc;
};

static void LogRunConfiguration(const Options& opt, Logger& log) {
  log.Log("Starting SD2.1 direct QNN C++ pipeline (persistent contexts, no qnn-net-run)");
  log.Log("Runtime dir: " + opt.runtime_dir);
  log.Log("QNN lib dir: " + opt.qnn_lib_dir);
  log.Log("Tokenizer dir: " + opt.tokenizer_dir);
  log.Log("Prompt: " + opt.prompt);
  log.Log("Negative prompt: " + opt.negative_prompt);
  log.Log("Seed=" + std::to_string(opt.seed) + ", Steps=" + std::to_string(opt.steps) +
          ", Guidance=" + FormatFixed(opt.guidance, 2) +
          ", PredictionType=" + opt.prediction_type +
          ", VaeScalingFactor=" + FormatFixed(opt.vae_scaling_factor, 5));
}

static ContextPaths ResolveContextPaths(const Options& opt) {
  ContextPaths paths;
  paths.text_context_path = ResolveFirstExistingPath(
      opt.runtime_dir,
      {opt.text_context,
       "text_encoder.bin",
       "text_encoder_qairt_context.bin",
       "stable_diffusion_v2_1-text_encoder-qualcomm_qcs9075.bin"},
      "text context",
      {"text_encoder"});
  paths.unet_context_path = ResolveFirstExistingPath(
      opt.runtime_dir,
      {opt.unet_context,
       "unet.bin",
       "unet_qairt_context.bin",
       "stable_diffusion_v2_1-unet-qualcomm_qcs9075.bin"},
      "unet context",
      {"unet"});
  paths.vae_context_path = ResolveFirstExistingPath(
      opt.runtime_dir,
      {opt.vae_context,
       "vae.bin",
       "vae_decoder.bin",
       "vae_qairt_context.bin",
       "stable_diffusion_v2_1-vae-qualcomm_qcs9075.bin"},
      "vae context",
      {"vae"});
  return paths;
}

static void AutoTuneForW8A16(Options* opt, const ContextPaths& paths, Logger& log) {
  if (!LooksLikeW8A16Export(
          *opt, paths.text_context_path, paths.unet_context_path, paths.vae_context_path)) {
    return;
  }

  if (!opt->prediction_type_user_set) {
    opt->prediction_type = "v_prediction";
    log.Log("[auto-tune] prediction_type -> v_prediction");
  } else {
    log.Log("[auto-tune] prediction_type kept (user override): " + opt->prediction_type);
  }
  if (!opt->vae_scaling_factor_user_set) {
    opt->vae_scaling_factor = 1.0f;
    log.Log("[auto-tune] vae_scaling_factor kept at 1.0");
  } else {
    log.Log("[auto-tune] vae_scaling_factor kept (user override): " +
            FormatFixed(opt->vae_scaling_factor, 5));
  }
}

static void ValidateRequiredPaths(const Options& opt,
                                  const ContextPaths& paths,
                                  const std::string& vocab_path,
                                  const std::string& merges_path) {
  EnsureExists(opt.qnn_lib_dir + "/" + opt.backend_lib);
  EnsureExists(opt.qnn_lib_dir + "/" + opt.system_lib);
  EnsureExists(paths.text_context_path);
  EnsureExists(paths.unet_context_path);
  EnsureExists(paths.vae_context_path);
  EnsureExists(vocab_path);
  EnsureExists(merges_path);
}

static void ConfigureRuntimeLibraryPaths(const Options& opt) {
  const std::string ld_path = BuildMergedPathValue(
      {opt.qnn_lib_dir, opt.runtime_dir}, std::getenv("LD_LIBRARY_PATH"), ':');
  const std::string adsp_path = BuildMergedPathValue(
      {opt.qnn_lib_dir, opt.runtime_dir}, std::getenv("ADSP_LIBRARY_PATH"), ';');
  setenv("LD_LIBRARY_PATH", ld_path.c_str(), 1);
  setenv("ADSP_LIBRARY_PATH", adsp_path.c_str(), 1);
}

static TokenBuffers BuildTokenBuffers(const Options& opt,
                                      const std::string& vocab_path,
                                      const std::string& merges_path) {
  ClipBpeTokenizer tokenizer(vocab_path, merges_path);
  auto uncond_ids = tokenizer.Encode(opt.negative_prompt, kTokenLength);
  auto cond_ids = tokenizer.Encode(opt.prompt, kTokenLength);

  TokenBuffers token_buffers;
  token_buffers.uncond_tokens.resize(kTokenLength);
  token_buffers.cond_tokens.resize(kTokenLength);
  for (int i = 0; i < kTokenLength; ++i) {
    token_buffers.uncond_tokens[i] = static_cast<float>(uncond_ids[i]);
    token_buffers.cond_tokens[i] = static_cast<float>(cond_ids[i]);
  }
  return token_buffers;
}

static std::vector<float> InitializeLatents(const DdimScheduler& scheduler, int seed) {
  std::mt19937 rng(static_cast<uint32_t>(seed));
  std::normal_distribution<float> normal(0.0f, 1.0f);
  std::vector<float> latents_nchw(static_cast<size_t>(kLatentC * kLatentH * kLatentW));
  for (float& v : latents_nchw) {
    v = normal(rng) * scheduler.init_noise_sigma();
  }
  return latents_nchw;
}

static LayoutSelection InferLayouts(const ContextRunner& unet_runner, const ContextRunner& vae_runner) {
  const TensorMeta* unet_latent_in_meta =
      FindTensorMetaByName(unet_runner.graph_meta().inputs, "latent");
  if (!unet_latent_in_meta && !unet_runner.graph_meta().inputs.empty()) {
    unet_latent_in_meta = &unet_runner.graph_meta().inputs.front();
  }
  const TensorMeta* unet_out_meta =
      !unet_runner.graph_meta().outputs.empty() ? &unet_runner.graph_meta().outputs.front() : nullptr;
  if (!unet_latent_in_meta || !unet_out_meta) {
    throw std::runtime_error("UNet graph metadata is missing required tensors");
  }

  const TensorMeta* vae_latent_in_meta =
      FindTensorMetaByName(vae_runner.graph_meta().inputs, "latent");
  if (!vae_latent_in_meta && !vae_runner.graph_meta().inputs.empty()) {
    vae_latent_in_meta = &vae_runner.graph_meta().inputs.front();
  }
  const TensorMeta* vae_out_meta =
      !vae_runner.graph_meta().outputs.empty() ? &vae_runner.graph_meta().outputs.front() : nullptr;
  if (!vae_latent_in_meta || !vae_out_meta) {
    throw std::runtime_error("VAE graph metadata is missing required tensors");
  }

  LayoutSelection layouts;
  layouts.unet_input_layout = InferLatentLayout(*unet_latent_in_meta);
  layouts.unet_output_layout = InferLatentLayout(*unet_out_meta);
  layouts.vae_input_layout = InferLatentLayout(*vae_latent_in_meta);
  layouts.vae_output_layout = InferImageLayout(*vae_out_meta);
  return layouts;
}

static void LogLayouts(const LayoutSelection& layouts, Logger& log) {
  log.Log("[layout] unet_input=" + LayoutToString(layouts.unet_input_layout) +
          " unet_output=" + LayoutToString(layouts.unet_output_layout) +
          " vae_input=" + LayoutToString(layouts.vae_input_layout) +
          " vae_output=" + LayoutToString(layouts.vae_output_layout));
}

static TextEmbeddings RunTextEncoder(ContextRunner& text_runner,
                                     const std::vector<float>& uncond_tokens,
                                     const std::vector<float>& cond_tokens,
                                     Stats* stats,
                                     Logger& log) {
  double te_uncond_s = 0.0;
  auto te_uncond_map = text_runner.Execute({{"tokens", uncond_tokens}}, &te_uncond_s);
  stats->text_uncond_s = te_uncond_s;
  log.Log("[text_encoder_uncond] direct graphExecute completed in " + FormatFixed(te_uncond_s) + "s");

  double te_cond_s = 0.0;
  auto te_cond_map = text_runner.Execute({{"tokens", cond_tokens}}, &te_cond_s);
  stats->text_cond_s = te_cond_s;
  log.Log("[text_encoder_cond] direct graphExecute completed in " + FormatFixed(te_cond_s) + "s");

  if (te_uncond_map.empty() || te_cond_map.empty()) {
    throw std::runtime_error("Text encoder produced no outputs");
  }

  const std::string te_out_name = text_runner.graph_meta().outputs.front().name;
  TextEmbeddings embeddings;
  embeddings.uncond = te_uncond_map.at(te_out_name);
  embeddings.cond = te_cond_map.at(te_out_name);
  return embeddings;
}

static double RunDenoiseLoop(const Options& opt,
                             DdimScheduler& scheduler,
                             ContextRunner& unet_runner,
                             const std::vector<float>& te_uncond,
                             const std::vector<float>& te_cond,
                             const LayoutSelection& layouts,
                             PredictionType prediction_type,
                             std::vector<float>* latents_nchw,
                             Stats* stats,
                             Logger& log) {
  const auto t_pipeline0 = std::chrono::steady_clock::now();
  const std::string unet_out_name = unet_runner.graph_meta().outputs.front().name;
  for (int i = 0; i < opt.steps; ++i) {
    const auto t_step0 = std::chrono::steady_clock::now();
    int timestep = scheduler.timesteps()[static_cast<size_t>(i)];

    std::vector<float> latent_input_nchw = scheduler.ScaleModelInput(*latents_nchw, timestep);
    std::vector<float> latent_input_model =
        ConvertLatentNchwToLayout(latent_input_nchw, layouts.unet_input_layout);
    std::vector<float> timestep_arr = {static_cast<float>(timestep)};

    double uncond_exec_s = 0.0;
    auto noise_uncond_map = unet_runner.Execute(
        {{"timestep", timestep_arr}, {"latent", latent_input_model}, {"text_emb", te_uncond}},
        &uncond_exec_s);

    double cond_exec_s = 0.0;
    auto noise_cond_map = unet_runner.Execute(
        {{"timestep", timestep_arr}, {"latent", latent_input_model}, {"text_emb", te_cond}},
        &cond_exec_s);

    std::vector<float> noise_uncond_model = noise_uncond_map.at(unet_out_name);
    std::vector<float> noise_cond_model = noise_cond_map.at(unet_out_name);
    std::vector<float> noise_uncond_nchw =
        ConvertLatentLayoutToNchw(noise_uncond_model, layouts.unet_output_layout);
    std::vector<float> noise_cond_nchw =
        ConvertLatentLayoutToNchw(noise_cond_model, layouts.unet_output_layout);

    std::vector<float> noise_pred_nchw(noise_uncond_nchw.size());
    for (size_t k = 0; k < noise_pred_nchw.size(); ++k) {
      noise_pred_nchw[k] = noise_uncond_nchw[k] + opt.guidance * (noise_cond_nchw[k] - noise_uncond_nchw[k]);
    }

    *latents_nchw = scheduler.Step(noise_pred_nchw, timestep, *latents_nchw, prediction_type);

    const auto t_step1 = std::chrono::steady_clock::now();
    const double step_s = std::chrono::duration<double>(t_step1 - t_step0).count();
    stats->step_seconds.push_back(step_s);

    log.Log("[step " + Pad2(i + 1) + "/" + std::to_string(opt.steps) + "] timestep=" +
            std::to_string(timestep) + ", unet_uncond=" + FormatFixed(uncond_exec_s) +
            "s, unet_cond=" + FormatFixed(cond_exec_s) +
            "s, e2e=" + FormatFixed(step_s) + "s");
  }

  const auto t_pipeline1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(t_pipeline1 - t_pipeline0).count();
}

static ImageOutputs RunVaeDecode(const Options& opt,
                                 ContextRunner& vae_runner,
                                 const LayoutSelection& layouts,
                                 const std::vector<float>& latents_nchw,
                                 Stats* stats) {
  std::vector<float> vae_input_nchw(latents_nchw.size());
  for (size_t i = 0; i < latents_nchw.size(); ++i) {
    vae_input_nchw[i] = latents_nchw[i] / opt.vae_scaling_factor;
  }
  std::vector<float> vae_input_model =
      ConvertLatentNchwToLayout(vae_input_nchw, layouts.vae_input_layout);
  double vae_s = 0.0;
  auto vae_map = vae_runner.Execute({{"latent", vae_input_model}}, &vae_s);
  stats->vae_s = vae_s;

  const std::string vae_out_name = vae_runner.graph_meta().outputs.front().name;
  std::vector<float> image_model = vae_map.at(vae_out_name);

  ImageOutputs outputs;
  outputs.raw_nhwc = ConvertImageLayoutToNhwc(image_model, layouts.vae_output_layout);
  outputs.normalized_nhwc = NormalizeImageForPng(outputs.raw_nhwc, ImagePostProcessMode::Auto);
  return outputs;
}

static std::string WriteSummaryJson(const Options& opt,
                                    const ContextPaths& paths,
                                    PredictionType prediction_type,
                                    const Stats& stats,
                                    double denoise_s,
                                    const std::vector<float>& image_nhwc_raw,
                                    const std::vector<float>& image_nhwc) {
  double sum_steps = 0.0;
  double min_step = std::numeric_limits<double>::infinity();
  double max_step = 0.0;
  for (double s : stats.step_seconds) {
    sum_steps += s;
    min_step = std::min(min_step, s);
    max_step = std::max(max_step, s);
  }
  const double avg_step =
      stats.step_seconds.empty() ? 0.0 : sum_steps / static_cast<double>(stats.step_seconds.size());

  auto [raw_min_it, raw_max_it] = std::minmax_element(image_nhwc_raw.begin(), image_nhwc_raw.end());
  auto [min_it, max_it] = std::minmax_element(image_nhwc.begin(), image_nhwc.end());
  double mean = 0.0;
  for (float v : image_nhwc) {
    mean += static_cast<double>(v);
  }
  mean /= static_cast<double>(image_nhwc.size());
  double var = 0.0;
  for (float v : image_nhwc) {
    const double d = static_cast<double>(v) - mean;
    var += d * d;
  }
  var /= static_cast<double>(image_nhwc.size());

  const std::string summary_path =
      fs::path(opt.output_image).parent_path().string() + "/summary_cpp_direct.json";
  std::ostringstream js;
  js << "{\n"
     << "  \"prompt\": \"" << JsonEscape(opt.prompt) << "\",\n"
     << "  \"negative_prompt\": \"" << JsonEscape(opt.negative_prompt) << "\",\n"
     << "  \"seed\": " << opt.seed << ",\n"
     << "  \"steps\": " << opt.steps << ",\n"
     << "  \"guidance\": " << opt.guidance << ",\n"
     << "  \"prediction_type\": \"" << PredictionTypeToString(prediction_type) << "\",\n"
     << "  \"vae_scaling_factor\": " << opt.vae_scaling_factor << ",\n"
     << "  \"runtime_dir\": \"" << JsonEscape(opt.runtime_dir) << "\",\n"
     << "  \"qnn_lib_dir\": \"" << JsonEscape(opt.qnn_lib_dir) << "\",\n"
     << "  \"text_context\": \"" << JsonEscape(paths.text_context_path) << "\",\n"
     << "  \"unet_context\": \"" << JsonEscape(paths.unet_context_path) << "\",\n"
     << "  \"vae_context\": \"" << JsonEscape(paths.vae_context_path) << "\",\n"
     << "  \"tokenizer_dir\": \"" << JsonEscape(opt.tokenizer_dir) << "\",\n"
     << "  \"mode\": \"direct_qnn_api_persistent_contexts\",\n"
     << "  \"output_image\": \"" << JsonEscape(opt.output_image) << "\",\n"
     << "  \"log_file\": \"" << JsonEscape(opt.log_file) << "\",\n"
     << "  \"text_uncond_seconds\": " << stats.text_uncond_s << ",\n"
     << "  \"text_cond_seconds\": " << stats.text_cond_s << ",\n"
     << "  \"denoise_seconds\": " << denoise_s << ",\n"
     << "  \"vae_seconds\": " << stats.vae_s << ",\n"
     << "  \"avg_step_seconds\": " << avg_step << ",\n"
     << "  \"min_step_seconds\": " << (std::isfinite(min_step) ? min_step : 0.0) << ",\n"
     << "  \"max_step_seconds\": " << max_step << ",\n"
     << "  \"image_raw_min\": " << (raw_min_it != image_nhwc_raw.end() ? *raw_min_it : 0.0f) << ",\n"
     << "  \"image_raw_max\": " << (raw_max_it != image_nhwc_raw.end() ? *raw_max_it : 0.0f) << ",\n"
     << "  \"image_min\": " << (min_it != image_nhwc.end() ? *min_it : 0.0f) << ",\n"
     << "  \"image_max\": " << (max_it != image_nhwc.end() ? *max_it : 0.0f) << ",\n"
     << "  \"image_mean\": " << mean << ",\n"
     << "  \"image_std\": " << std::sqrt(var) << "\n"
     << "}\n";

  WriteTextFile(summary_path, js.str());
  return summary_path;
}

int main(int argc, char** argv) {
  try {
    Options opt = ParseArgs(argc, argv);
    Logger log(opt.log_file);

    LogRunConfiguration(opt, log);
    const ContextPaths paths = ResolveContextPaths(opt);
    AutoTuneForW8A16(&opt, paths, log);
    const PredictionType prediction_type = ParsePredictionType(opt.prediction_type);
    log.Log("[effective] PredictionType=" + PredictionTypeToString(prediction_type) +
            ", VaeScalingFactor=" + FormatFixed(opt.vae_scaling_factor, 5));

    const std::string vocab_path = opt.tokenizer_dir + "/vocab.json";
    const std::string merges_path = opt.tokenizer_dir + "/merges.txt";
    ValidateRequiredPaths(opt, paths, vocab_path, merges_path);
    ConfigureRuntimeLibraryPaths(opt);
    TokenBuffers token_buffers = BuildTokenBuffers(opt, vocab_path, merges_path);

    DdimScheduler scheduler(
        /*num_train_timesteps=*/1000,
        /*beta_start=*/0.00085f,
        /*beta_end=*/0.012f,
        /*set_alpha_to_one=*/false,
        /*steps_offset=*/1);
    scheduler.SetTimesteps(opt.steps);

    std::vector<float> latents_nchw = InitializeLatents(scheduler, opt.seed);

    QnnRuntime runtime(opt.qnn_lib_dir, opt.backend_lib, opt.system_lib, log);
    ContextRunner text_runner(runtime, paths.text_context_path, "text_encoder", log);
    ContextRunner unet_runner(runtime, paths.unet_context_path, "unet", log);
    ContextRunner vae_runner(runtime, paths.vae_context_path, "vae", log);

    const LayoutSelection layouts = InferLayouts(unet_runner, vae_runner);
    LogLayouts(layouts, log);

    Stats stats;
    const TextEmbeddings text_embeddings = RunTextEncoder(
        text_runner, token_buffers.uncond_tokens, token_buffers.cond_tokens, &stats, log);

    const double denoise_s = RunDenoiseLoop(
        opt,
        scheduler,
        unet_runner,
        text_embeddings.uncond,
        text_embeddings.cond,
        layouts,
        prediction_type,
        &latents_nchw,
        &stats,
        log);

    const ImageOutputs image_outputs =
        RunVaeDecode(opt, vae_runner, layouts, latents_nchw, &stats);

    WritePpmRgb(opt.output_image, image_outputs.normalized_nhwc);
    log.Log("Saved image: " + opt.output_image);
    const std::string summary_path = WriteSummaryJson(
        opt,
        paths,
        prediction_type,
        stats,
        denoise_s,
        image_outputs.raw_nhwc,
        image_outputs.normalized_nhwc);
    log.Log("Wrote summary: " + summary_path);
    log.Log("Run completed successfully.");
    return 0;

  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << std::endl;
    return 1;
  }
}
