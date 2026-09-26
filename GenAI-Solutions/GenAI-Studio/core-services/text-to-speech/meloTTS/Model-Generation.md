# Text-To-Speech Model Setup (MeloTTS on QCS9075)

This guide prepares TTS model artifacts required by `text-to-speech:latest`.

## 0) Assumptions

- Device provisioning and Docker setup are already complete.
- Host directory `/opt/genai-studio-models/text-to-speech/` is writable.
- QAIRT flat runtime libs are available on target (default: `/opt/qairt/current/qairt_245_flat_libs`).
- `melo_sdk` is staged in repo (`core-services/text-to-speech/meloTTS/melo_sdk`) or fallback zip exists.
- `tools/model_conversion_scripts` is staged in repo (packer + dictionaries).

Execution boundary:

- Run model download/extract/pack steps on target device.
- Service container mounts host model path into `/opt/TTS_binary/MeloTTS`.

## 1) Canonical model paths and mount

Default TTS source bundle directory (download/extract target):

```text
/opt/genai-studio-models/text-to-speech/melotts_en-qnn_context_binary-mixed_with_float-qualcomm_qcs9075
```

Default TTS runtime model directory (packed `.qnn` output):

```text
/opt/genai-studio-models/text-to-speech/melo-tts-v73/files
```

Default container runtime path:

```text
/opt/TTS_binary/MeloTTS
```

Custom host override:

```bash
export TTS_MODEL_HOST_DIR=/your/custom/path/to/melo-tts-v73/files
```

## 2) Preferred flow (scripted; same pattern as I2T)

Run from repo root on target device:

```bash
bash scripts/phases/model_gen.sh --service tts
```

Force refresh of source download and packed runtime model:

```bash
bash scripts/phases/model_gen.sh --service tts --force-download
```

What this does:

- Downloads MeloTTS AI Hub source bundle
- Extracts source artifacts
- Packs runtime `.qnn` bundle
- Copies `libtts_impl_skel.so` when available
- Validates runtime artifacts

## 3) Manual download source bundle (target device)

```bash
TTS_URL="https://qaihub-public-assets.s3.us-west-2.amazonaws.com/qai-hub-models/models/melotts_en/releases/v0.49.1/melotts_en-qnn_context_binary-mixed_with_float-qualcomm_qcs9075.zip"
TTS_PARENT_DIR="/opt/genai-studio-models/text-to-speech"
TTS_SOURCE_DIR="${TTS_PARENT_DIR}/melotts_en-qnn_context_binary-mixed_with_float-qualcomm_qcs9075"
TTS_ZIP="/tmp/$(basename "${TTS_URL}")"

mkdir -p "${TTS_PARENT_DIR}"
wget -O "${TTS_ZIP}" "${TTS_URL}"
```

Optional integrity check:

```bash
sha256sum "${TTS_ZIP}"
```

## 4) Extract source bundle

```bash
if command -v unzip >/dev/null 2>&1; then
  unzip -o "${TTS_ZIP}" -d "${TTS_PARENT_DIR}"
else
  if command -v sudo >/dev/null 2>&1; then
    sudo apt-get update && sudo apt-get install -y unzip
  else
    apt-get update && apt-get install -y unzip
  fi
  unzip -o "${TTS_ZIP}" -d "${TTS_PARENT_DIR}"
fi

test -d "${TTS_SOURCE_DIR}" && echo "TTS source directory OK"
```

## 5) Pack runtime `.qnn` (manual equivalent)

Recommended command (uses script-managed packer path + compatibility logic):

```bash
bash scripts/phases/model_gen.sh --service tts --force-download
```

If you need explicit packer override:

```bash
bash scripts/phases/model_gen.sh --service tts --skip-download-tts \
  --tts-packer-script /absolute/path/to/qnn_model_generation.py
```

## 6) Verify required runtime artifacts

```bash
TTS_MODEL_DIR=/opt/genai-studio-models/text-to-speech/melo-tts-v73/files
ls -lah "${TTS_MODEL_DIR}"
ls "${TTS_MODEL_DIR}"/*.qnn

test -f "${TTS_MODEL_DIR}/libtts_impl_skel.so" && echo "libtts_impl_skel.so OK"
```

## 7) QAIRT runtime validation

```bash
TTS_QAIRT_FLAT_LIB_DIR="${TTS_QAIRT_FLAT_LIB_DIR:-/opt/qairt/current/qairt_245_flat_libs}"

ls -lah "${TTS_QAIRT_FLAT_LIB_DIR}"
test -f "${TTS_QAIRT_FLAT_LIB_DIR}/libQnnHtp.so" && echo "libQnnHtp.so OK"
test -f "${TTS_QAIRT_FLAT_LIB_DIR}/libQnnSystem.so" && echo "libQnnSystem.so OK"
```

## 8) Troubleshooting

- `TTS packer script missing`
  - stage `tools/model_conversion_scripts` in repo.
- `ModuleNotFoundError: en_dict_generation`
  - ensure packer runs from conversion-script module directory (handled by current `model_gen.sh`).
- `Packer references libQnnHtpV81.so`
  - current `model_gen.sh` auto-applies V73 compatibility shim.
- `libtts_impl_skel.so` missing
  - stage `melo_sdk` in repo and rerun `model_gen.sh --service tts`.
- runtime init failures (`tts_impl_open`, `TTSEngine::init failed`)
  - verify QAIRT mount, ADSP paths, and `TTS_MODEL_HOST_DIR` mapping.

## 9) Next step

Continue with `core-services/text-to-speech/meloTTS/README.md` for build/run/validation.

## 10) Related API Docs

- `core-services/text-to-speech/meloTTS/README.md`
- `docs/TROUBLESHOOTING_GUIDE.md`
- `docs/API_CONTRACTS.md`
