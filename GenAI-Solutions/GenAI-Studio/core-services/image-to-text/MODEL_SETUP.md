# Image-To-Text Model and Dependency Setup

This document defines required model artifacts and runtime prerequisites for `image-to-text:latest`.

## 1) Canonical Version Source

Load canonical versions from repo root:

```bash
cd /path/to/genai-studio
set -a
source ./versions.env
set +a
```

Important keys:

- `QAIRT_VERSION`
- `QAIRT_FLAT_LIB_DIR`
- `I2T_QAIRT_VERSION_HINT`

Do not hardcode alternate QAIRT versions in local setup notes unless intentionally testing a non-default branch.

## 2) Model generation source

Download from QPM tutorial:

- `https://qpm.qualcomm.com/#/main/tools/details/Tutorial_for_Qwen2_5_VL_7B_IoT`

After downloading, run the notebook step by step and then place the runtime-ready folder on device. 

## 3) Model Package Location

Default model directory used by compose/runtime:

```text
/opt/genai-studio-models/image-to-text/Lemans_LE_Gen2_QNN2_41_qwen25_vl_7B/files
```

Model acquisition note:

- This repository does not provide an automated public downloader for the I2T VLM package.
- Obtain the full exported bundle from your internal model source or owner on a host or staging machine, then copy it into `MODEL_DIR` on the target.
- Partial copies (for example only `uploads/`) will pass path existence checks but fail at runtime.

Example target copy from a staging machine:

```bash
rsync -av /path/to/Lemans_LE_Gen2_QNN2_41_qwen25_vl_7B/files/ \
  ubuntu@<target-host>:/opt/genai-studio-models/image-to-text/Lemans_LE_Gen2_QNN2_41_qwen25_vl_7B/files/
```

Custom model path override:

If you keep models in a different host location, set compose override before `docker compose up`:

```bash
export I2T_MODEL_HOST_DIR=/your/custom/path/to/model/files
```

## 4) Required Artifacts (Minimum)

Inside `MODEL_DIR`, ensure the following artifacts are present:

 - `libGenie.so` (QNN runtime library)
- `image_encoder.json` (encoder configuration)
- `lut_encoder.json` (lookup table configuration)
- `text_generator.json` (text generator configuration)
- referenced context/model binaries required by the JSONs
- writable `uploads/` directory (created automatically if missing)

Quick check:

```bash
MODEL_DIR=/opt/genai-studio-models/image-to-text/Lemans_LE_Gen2_QNN2_41_qwen25_vl_7B/files
ls -lah "${MODEL_DIR}"
test -f "${MODEL_DIR}/libGenie.so" && echo "libGenie.so OK"
test -f "${MODEL_DIR}/image_encoder.json" && echo "image_encoder.json OK"
test -f "${MODEL_DIR}/lut_encoder.json" && echo "lut_encoder.json OK"
test -f "${MODEL_DIR}/text_generator.json" && echo "text_generator.json OK"
```

## 5) QAIRT Runtime Validation

Validate flat runtime bundle (from `versions.env`):

```bash
set -a; source ./versions.env; set +a
ls -lah "${QAIRT_FLAT_LIB_DIR}"
test -f "${QAIRT_FLAT_LIB_DIR}/libGenie.so" && echo "QAIRT libGenie.so OK"
```

Compose defaults for I2T should match this runtime path:

- `I2T_QAIRT_FLAT_LIB_DIR`
- `I2T_LD_LIBRARY_PATH`
- `I2T_ADSP_LIBRARY_PATH`

## 6) In-Container Preprocess Dependency (HF Cache)

Image preprocessing now runs inside the Image-To-Text container (`/v1/responses` path).
`Qwen2VLImageProcessor` must be available from the mounted Hugging Face cache.

Prepare cache on the target device, or prepare it on a host machine and then
copy the resulting cache tree into `/opt/genai-studio-cache/huggingface` on the target:

```bash
huggingface-cli download Qwen/Qwen2.5-VL-7B-Instruct --local-dir-use-symlinks False
```

Ensure compose mount exists:

```yaml
- /opt/genai-studio-cache/huggingface:/root/.cache/huggingface
```

Optional warmup check (inside `image-to-text` container):

```bash
docker compose up -d image-to-text
docker exec -i image-to-text /opt/i2t-venv/bin/python3 - <<'PY'
from transformers import Qwen2VLImageProcessor
Qwen2VLImageProcessor.from_pretrained("Qwen/Qwen2.5-VL-7B-Instruct")
print("Qwen2VLImageProcessor cache ready")
PY
```

## 7) Troubleshooting

- `libGenie.so not found`
  - verify `MODEL_DIR` mount and file presence.
- `Could not create context from binary ... err 1002`
  - NPU/context allocation issue; stop competing workloads, restart `image-to-text`.
- container start fails with `...libxdsprpc.so... not a directory`
  - stale container metadata; recreate container:
  - `docker rm -f image-to-text`
  - `docker compose up -d image-to-text`
- preprocess fails in Image-To-Text container
  - missing HF cache mount or missing model cache.
  - verify HF cache mount in compose: `/opt/genai-studio-cache/huggingface:/root/.cache/huggingface`
- vision request returns validation error for image payload
  - send image in OpenAI Responses format: `input[].content[].input_image.image_url`

## 10) Next Steps

After setup, continue with:

- `core-services/image-to-text/README.md` (build/run/validate flow)
- `core-services/image-to-text/CODE_FLOW.md` (internal request flow + contract summary)

