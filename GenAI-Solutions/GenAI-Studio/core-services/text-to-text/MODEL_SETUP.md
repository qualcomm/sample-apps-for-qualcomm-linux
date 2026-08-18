# Text-to-Text Model Setup

This file covers only Text-to-Text model placement and validation.
For full stack bring-up, follow repo-root [README.md](../../README.md) first.

## Golden Path

1. [../../README.md](../../README.md)
2. [README.md](README.md) in this folder
3. This file for model payload checks

## Scripted setup (recommended)

Run on target device from repo root:

```bash
bash scripts/phases/model_gen.sh --service t2t
```

Force refresh:

```bash
bash scripts/phases/model_gen.sh --service t2t --force-download
```

## Canonical Layout

Use a flat model directory layout for all T2T models (usually aihub default).

Example download links:
- https://aihub.qualcomm.com/models/llama_v3_2_3b_instruct_ssd
- https://aihub.qualcomm.com/models/qwen3_4b_instruct_2507

```text
/opt/genai-studio-models/text-to-text/<model_name>/genie_config.json
/opt/genai-studio-models/text-to-text/<model_name>/<all referenced binaries>
```

Example model directories:

```text
/opt/genai-studio-models/text-to-text/llama_v3_2_3b_instruct_ssd-genie-w4a16-qualcomm_qcs9075
/opt/genai-studio-models/text-to-text/qwen3_4b-genie-w4a16-qualcomm_qcs9075
```

## 1) Create Models

```bash
sudo mkdir -p /opt/genai-studio-models/text-to-text
```

## 2) Copy Model Payloads

Copy each extracted model directory from aihub
Required minimum per model:

- `genie_config.json`
- all files referenced by `genie_config.json` (for example `.bin`, `.so`, tokenizer files)

Example copy (from host to target):

```bash
# Run on host machine
scp -r /path/to/llama_v3_2_3b_instruct_ssd-genie-w4a16-qualcomm_qcs9075/ ubuntu@<target-host>:/opt/genai-studio-models/text-to-text/
scp -r /path/to/qwen3_4b-genie-w4a16-qualcomm_qcs9075/  ubuntu@<target-host>:/opt/genai-studio-models/text-to-text/
```

## 3) Validate Files on Target

Example validation:

```bash
LLAMA_DIR=/opt/genai-studio-models/text-to-text/llama_v3_2_3b_instruct_ssd-genie-w4a16-qualcomm_qcs9075
QWEN_DIR=/opt/genai-studio-models/text-to-text/qwen3_4b-genie-w4a16-qualcomm_qcs9075

test -f "$LLAMA_DIR/genie_config.json" && echo "llama config OK"
test -f "$QWEN_DIR/genie_config.json" && echo "qwen config OK"
```

Recursive reference check:

```bash
python3 - <<'PY'
import json
from pathlib import Path

model_dirs = [
    Path("/opt/genai-studio-models/text-to-text/llama_v3_2_3b_instruct_ssd-genie-w4a16-qualcomm_qcs9075"),
    Path("/opt/genai-studio-models/text-to-text/qwen3_4b-genie-w4a16-qualcomm_qcs9075"),
]

for model_dir in model_dirs:
    cfg_path = model_dir / "genie_config.json"
    cfg = json.loads(cfg_path.read_text())
    refs = []

    def walk(node):
        if isinstance(node, dict):
            for value in node.values():
                walk(value)
        elif isinstance(node, list):
            for value in node:
                walk(value)
        elif isinstance(node, str) and node.endswith((".bin", ".so", ".dat", ".json")):
            refs.append(node)

    walk(cfg)
    missing = [str(model_dir / r) for r in sorted(set(refs)) if not (model_dir / r).exists()]
    if missing:
        print(f"Missing references for {model_dir.name}:")
        for m in missing:
            print(m)
        raise SystemExit(1)
    print(f"{model_dir.name}: reference check OK")
PY
```

## 4) Required Runtime Env (Compose)

```bash
export TG_MODELS_ROOT=/opt/genai-studio-models/text-to-text
export TG_MODEL_NAME=llama_v3_2_3b_instruct_ssd-genie-w4a16-qualcomm_qcs9075
export TG_MODEL_DIR=/opt/genai-studio-models/text-to-text/${TG_MODEL_NAME}
export GENIE_CONFIG=${TG_MODEL_DIR}/genie_config.json
export BASE_DIR=${TG_MODEL_DIR}
```

## 5) Multi-Model Switch Verification

After service is up:

```bash
# discover
curl -s http://127.0.0.1:8088/v1/internal/models

# switch to qwen
curl -s -X POST http://127.0.0.1:8088/v1/internal/models/load \
  -H 'Content-Type: application/json' \
  -d '{"model":"qwen3_4b-genie-w4a16-qualcomm_qcs9075"}'

# switch back to llama
curl -s -X POST http://127.0.0.1:8088/v1/internal/models/load \
  -H 'Content-Type: application/json' \
  -d '{"model":"llama_v3_2_3b_instruct_ssd-genie-w4a16-qualcomm_qcs9075"}'
```

If `/v1/internal/models/load` fails:

- verify `genie_config.json` exists in the model root (flat layout)
- verify referenced files exist
- verify QAIRT and DSP libs are mounted as documented in [README.md](README.md)
