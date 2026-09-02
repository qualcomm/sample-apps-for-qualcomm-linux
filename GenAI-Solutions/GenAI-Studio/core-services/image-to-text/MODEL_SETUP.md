# Image-To-Text Model Setup (Qwen2.5-VL-7B on QCS9075)

This guide prepares model artifacts required by `image-to-text:responses-v1`.

## 0) Assumptions

- Device provisioning and Docker setup are already complete.
- Host directory `/opt/genai-studio-models/image-to-text/` is writable.
- QAIRT flat runtime libs are available on target (default: `/opt/qairt/current/qairt_245_flat_libs`).

Execution boundary:

- Run model download/extract/prep steps on the target device.
- Service container mounts host model path into `/opt/I2T_binary/files`.

## 1) Canonical model path and mount

Default host model directory (compose/scripts):

```text
/opt/genai-studio-models/image-to-text/qwen2_5_vl_7b_instruct-genie-w4a16-qualcomm_qcs9075
```

Default container runtime path:

```text
/opt/I2T_binary/files
```

Custom host override:

```bash
export I2T_MODEL_HOST_DIR=/your/custom/path/to/qwen2_5_vl_7b_instruct-genie-w4a16-qualcomm_qcs9075
```

## 2) Download model artifacts (target device)

```bash
I2T_URL="https://qaihub-public-assets.s3.us-west-2.amazonaws.com/qai-hub-models/models/qwen2_5_vl_7b_instruct/releases/v0.59.0/qwen2_5_vl_7b_instruct-genie-w4a16-qualcomm_qcs9075.zip"
I2T_PARENT_DIR="/opt/genai-studio-models/image-to-text"
I2T_MODEL_DIR="${I2T_PARENT_DIR}/qwen2_5_vl_7b_instruct-genie-w4a16-qualcomm_qcs9075"
I2T_ZIP="/tmp/$(basename "${I2T_URL}")"

mkdir -p "${I2T_PARENT_DIR}"
wget -O "${I2T_ZIP}" "${I2T_URL}"
```

Optional integrity check:

```bash
sha256sum "${I2T_ZIP}"
```

## 3) Extract model bundle (7z preferred)

```bash
extracted=0
if command -v 7z >/dev/null 2>&1; then
  7z x -y -o"${I2T_PARENT_DIR}" "${I2T_ZIP}" && extracted=1
else
  if command -v apt-get >/dev/null 2>&1; then
    if command -v sudo >/dev/null 2>&1; then
      sudo apt-get update && sudo apt-get install -y p7zip-full || true
    else
      apt-get update && apt-get install -y p7zip-full || true
    fi
  fi
  if command -v 7z >/dev/null 2>&1; then
    7z x -y -o"${I2T_PARENT_DIR}" "${I2T_ZIP}" && extracted=1
  fi
fi

if [[ ${extracted} -ne 1 ]]; then
  unzip -o "${I2T_ZIP}" -d "${I2T_PARENT_DIR}"
fi

test -d "${I2T_MODEL_DIR}" && echo "I2T model directory OK"
```

## 4) Stage runtime helpers (libGenie + uploads)

If `libGenie.so` is missing in model folder, copy from QAIRT flat libs:

```bash
QAIRT_FLAT_LIB_DIR="${I2T_QAIRT_FLAT_LIB_DIR:-/opt/qairt/current/qairt_245_flat_libs}"

if [[ ! -f "${I2T_MODEL_DIR}/libGenie.so" ]]; then
  cp -f "${QAIRT_FLAT_LIB_DIR}/libGenie.so" "${I2T_MODEL_DIR}/libGenie.so"
fi

mkdir -p "${I2T_MODEL_DIR}/uploads"
```

## 5) Verify required artifacts

Minimum runtime checks:

```bash
ls -lah "${I2T_MODEL_DIR}"

test -f "${I2T_MODEL_DIR}/libGenie.so" && echo "libGenie.so OK"
test -d "${I2T_MODEL_DIR}/uploads" && echo "uploads/ OK"

# At least one valid image encoder config
test -f "${I2T_MODEL_DIR}/img-enc-htp.json" || test -f "${I2T_MODEL_DIR}/image_encoder.json"

# At least one valid text encoder config
test -f "${I2T_MODEL_DIR}/text-encoder.json" || test -f "${I2T_MODEL_DIR}/text_encoder.json"

# At least one valid text decoder config
test -f "${I2T_MODEL_DIR}/text-dec-htp.json" || test -f "${I2T_MODEL_DIR}/text-generator.json"
```

## 6) QAIRT runtime validation

```bash
QAIRT_FLAT_LIB_DIR="${I2T_QAIRT_FLAT_LIB_DIR:-/opt/qairt/current/qairt_245_flat_libs}"

ls -lah "${QAIRT_FLAT_LIB_DIR}"
test -f "${QAIRT_FLAT_LIB_DIR}/libQnnHtp.so" && echo "libQnnHtp.so OK"
test -f "${QAIRT_FLAT_LIB_DIR}/libQnnSystem.so" && echo "libQnnSystem.so OK"
test -f "${QAIRT_FLAT_LIB_DIR}/libGenie.so" && echo "libGenie.so OK"
```

## 7) Troubleshooting

- `libGenie.so not found`
  - copy `libGenie.so` from `I2T_QAIRT_FLAT_LIB_DIR` into model directory.
- `uploads` errors (permission/path)
  - ensure `uploads/` exists and model directory mount is writable.
- extraction fails with `7z: command not found`
  - install `p7zip-full`, or fallback to `unzip`.
- container cannot resolve model configs
  - check `MODEL_DIR` (`/opt/I2T_binary/files` in container) and host `I2T_MODEL_HOST_DIR` mapping.

## 8) Next step

Continue with `core-services/image-to-text/README.md` for build/run/validation.

## 9) Related API Docs

- `core-services/image-to-text/README.md` (quick build/run/validate flow)
- `core-services/image-to-text/CODE_FLOW.md` (internal request flow + contract summary)
