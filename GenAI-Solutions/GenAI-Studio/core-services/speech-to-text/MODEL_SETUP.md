# Speech-To-Text Model Setup (whisper-tiny on QCS9075)

This guide prepares model artifacts required by `speech-to-text:latest`.

## 0) Assumptions

- Device provisioning and Docker setup are already complete.
- Host directory `/opt/genai-studio-models/speech-to-text/` is writable.
- If you need the private Whisper SDK, prepare it on a host or staging machine first and then stage it onto the target.

Execution boundary:

- Run the QPM3 install on a host machine only.
- Run model unzip, VAD asset copy, and build/run checks on the target device.
- Canonical target staging path for the private SDK payload is `core-services/speech-to-text/whisper_sdk`.

Scripted setup (recommended):

```bash
bash scripts/phases/model_gen.sh --service stt
```

Force refresh:

```bash
bash scripts/phases/model_gen.sh --service stt --force-download
```

## 1) Host-side Whisper SDK install and target staging (run on host machine only)

```bash
qpm-cli --install VoiceAI_ASR -v 2.6.0.0 --path /opt/qcom/qpm/VoiceAI_ASR/2.6.0.0 --silent

TARGET_REPO=/path/to/genai-studio-on-target
rsync -av /opt/qcom/qpm/VoiceAI_ASR/2.6.0.0/whisper_sdk/ \
  ubuntu@<target-host>:${TARGET_REPO}/core-services/speech-to-text/whisper_sdk/
```

That install creates the source SDK root at:

```text
/opt/qcom/qpm/VoiceAI_ASR/2.6.0.0/whisper_sdk
```

If you do not want to stage the SDK into the repo checkout, copy it anywhere on
the target and set `WHISPER_SDK_ROOT` explicitly during build and model prep.

## 2) Download model artifacts (run on target device)

```bash
wget https://qaihub-public-assets.s3.us-west-2.amazonaws.com/qai-hub-models/models/whisper_tiny/releases/v0.50.2/whisper_tiny-qnn_context_binary-float-qualcomm_qcs9075.zip
unzip whisper_tiny-qnn_context_binary-float-qualcomm_qcs9075.zip -d /opt/genai-studio-models/speech-to-text/
export MODEL_ROOT=/opt/genai-studio-models/speech-to-text/whisper_tiny-qnn_context_binary-float-qualcomm_qcs9075
```

Optional integrity check:

```bash
sha256sum whisper_tiny-qnn_context_binary-float-qualcomm_qcs9075.zip
```

## 3) Whisper SDK requirement (build + VAD asset)

Whisper SDK 2.6.0.0 is required because:

1. `core-services/speech-to-text/build.sh` compiles against SDK headers and libs.
2. `libnnvad_model.so` is sourced from SDK assets.

Typical SDK path:

```text
/opt/qcom/qpm/VoiceAI_ASR/2.6.0.0/whisper_sdk
```

## 4) Copy required VAD asset (run on target device)

```bash
REPO_ROOT=/path/to/genai-studio
WHISPER_SDK_ROOT="${REPO_ROOT}/core-services/speech-to-text/whisper_sdk"
MODEL_ROOT=/opt/genai-studio-models/speech-to-text/whisper_tiny-qnn_context_binary-float-qualcomm_qcs9075

test -f "${WHISPER_SDK_ROOT}/libs/npu/rpc_libraries/assets/aarch64_linux/libnnvad_model.so" \
  && echo "whisper sdk asset OK"

cp -f "${WHISPER_SDK_ROOT}/libs/npu/rpc_libraries/assets/aarch64_linux/libnnvad_model.so" \
      "${MODEL_ROOT}/libnnvad_model.so"
```

If the SDK is staged outside the repo on target, set `WHISPER_SDK_ROOT`
accordingly. QPM3 installs use `whisper_sdk`; older local checkouts may still
use `whisper-sdk`.

## 5) Verify required model files

Service startup expects these files in `MODEL_ROOT`:

- `encoder.bin` (or compatible encoder binary naming)
- `decoder.bin` (or compatible decoder binary naming)
- `vocab.bin`
- `libnnvad_model.so`

Verification:

```bash
ls -lah "${MODEL_ROOT}"
test -f "${MODEL_ROOT}/vocab.bin" && echo "vocab OK"
test -f "${MODEL_ROOT}/libnnvad_model.so" && echo "vad OK"
ls "${MODEL_ROOT}"/encoder.bin "${MODEL_ROOT}"/decoder.bin 2>/dev/null || \
ls "${MODEL_ROOT}"/whisper_tiny-encoder-*.bin "${MODEL_ROOT}"/whisper_tiny-decoder-*.bin
```

Expected directory shape:

```text
${MODEL_ROOT}/
  encoder.bin (or whisper_tiny-encoder-*.bin)
  decoder.bin (or whisper_tiny-decoder-*.bin)
  vocab.bin
  libnnvad_model.so
```

## 6) Runtime QNN library directory

Recommended: keep runtime QNN libraries in a dedicated directory and pass it through `STT_QNN_LIB_HOST_DIR`.

```bash
export STT_QNN_LIB_HOST_DIR=/opt/qairt/current/qairt_260_flat_libs
```

Reference for preparing these libraries:

- `../../docs/setup/DEVICE_SETUP.md` section `7) Install QAIRT under /opt/qairt` (flat QAIRT library bundle commands)

## 7) Next step

Continue with `core-services/speech-to-text/README.md` for build/run/validation.

## 8) Related API Docs

- `core-services/speech-to-text/README.md` (quick build/run/validate flow)
- `core-services/speech-to-text/CODE_FLOW.md` (internal request flow + contract summary)
