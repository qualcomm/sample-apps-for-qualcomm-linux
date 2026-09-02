# Text-Generation Service

Text-Generation serves OpenAI-style chat completions on `:8088`.
Client traffic should use orchestrator `:8090`; direct `:8088` is for backend debugging.

Canonical build/run/rebuild commands live in repo-root `README.md`:

1. `One Bring-Up Path`
2. `Strict clean rebuild cycle`
3. `Six-service functional tests`

## Prerequisites

Required model directories (download from AI Hub):

You can use any model from AI Hub. The examples below show the default and an alternative:

- `/opt/genai-studio-models/text-to-text/qwen3_4b-genie-w4a16-qualcomm_qcs9075` (default)
- `/opt/genai-studio-models/text-to-text/llama_v3_2_3b_instruct_ssd-genie-w4a16-qualcomm_qcs9075` (alternative)

### Environment Variable Examples

**Qwen (default):**
```bash
export TG_MODEL_DIR=/opt/genai-studio-models/text-to-text/qwen3_4b-genie-w4a16-qualcomm_qcs9075
export GENIE_CONFIG=/opt/genai-studio-models/text-to-text/qwen3_4b-genie-w4a16-qualcomm_qcs9075/genie_config.json
export BASE_DIR=/opt/genai-studio-models/text-to-text/qwen3_4b-genie-w4a16-qualcomm_qcs9075
```

**Llama (alternative):**
```bash
export TG_MODEL_DIR=/opt/genai-studio-models/text-to-text/llama_v3_2_3b_instruct_ssd-genie-w4a16-qualcomm_qcs9075
export GENIE_CONFIG=/opt/genai-studio-models/text-to-text/llama_v3_2_3b_instruct_ssd-genie-w4a16-qualcomm_qcs9075/genie_config.json
export BASE_DIR=/opt/genai-studio-models/text-to-text/llama_v3_2_3b_instruct_ssd-genie-w4a16-qualcomm_qcs9075
```

Required runtime libs:

- `/opt/qairt/current/qairt_245_flat_libs`

Model bundle layout:

- Canonical layout for all T2T models: `<model>/genie_config.json`
- Canonical `TG_MODEL_DIR`: `<model>` (flat layout, not `.../files`)
- Llama and Qwen both use the same flat model directory pattern

Quick checks on target:

```bash
test -d /opt/genai-studio-models/text-to-text/qwen3_4b-genie-w4a16-qualcomm_qcs9075 && echo "Text-to-Text model directory OK"
test -d /opt/qairt/current/qairt_245_flat_libs && echo "QAIRT flat libs OK"
```

## Build and Start Service

Build the service image:

```bash
DOCKER_BUILDKIT=1 docker build --progress=plain -t text-to-text:latest core-services/text-to-text/
```

Export required environment variables and start the container:

```bash
export TG_QAIRT_LIBS_HOST_DIR=/opt/qairt/current/qairt_245_flat_libs
export TG_MODEL_DIR=/opt/genai-studio-models/text-to-text/qwen3_4b-genie-w4a16-qualcomm_qcs9075
export TG_LD_LIBRARY_PATH=/opt/qairt/current/qairt_245_flat_libs:/usr/lib:/usr/lib/aarch64-linux-gnu
export TG_ADSP_LIBRARY_PATH='/opt/qairt/current/qairt_245_flat_libs;/usr/lib/rfsa/adsp;/usr/lib/dsp;/dsp;/usr/lib/dsp/cdsp1'
docker compose up -d text-to-text
```

For full stack bring-up, refer to `README.md` [section 7) Start services with docker compose](#7-start-services-with-docker-compose). (Recommended)

## Validate

Health check:

```bash
curl -s http://localhost:8088/health
curl -s http://localhost:8088/v1/models
```

Expected response shape:

```json
{
  "health": {"status": "ok"},
  "models": {"data": [{"id": "qwen3_4b-genie-w4a16-qualcomm_qcs9075"}]}
}
```

Non-stream chat request:

```bash
curl -s -X POST http://localhost:8088/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"qwen3_4b-genie-w4a16-qualcomm_qcs9075","messages":[{"role":"user","content":"Reply with OK"}],"stream":false}'
```

Expected response shape:

```json
{
  "id": "...",
  "object": "text_completion",
  "created": 1234567890,
  "model": "qwen3_4b-genie-w4a16-qualcomm_qcs9075",
  "choices": [{"message": {"role": "assistant", "content": "OK"}}]
}
```

Stream check:

```bash
curl -N -X POST http://localhost:8088/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"qwen3_4b-genie-w4a16-qualcomm_qcs9075","messages":[{"role":"user","content":"Write one short line"}],"stream":true}'
```

Expected stream shape:

```text
data: {"id":"...","choices":[{"delta":{"content":"..."}}]}
data: [DONE]
```

## Endpoint Guide

- `GET /health`: service liveness
- `GET /v1/models`: list available models
- `POST /v1/chat/completions`: OpenAI-style chat completions (stream or non-stream)
- `POST /v1/internal/models`: inspect discoverable local model bundles
- `POST /v1/internal/models/load`: switch active model at runtime

## Multi-Model Load/Switch Flow (Direct T2T)

Inspect discoverable local bundles:

```bash
curl -s http://127.0.0.1:8088/v1/internal/models
```

Switch to Qwen by model folder key:

```bash
curl -s -X POST http://127.0.0.1:8088/v1/internal/models/load \
  -H 'Content-Type: application/json' \
  -d '{"model":"qwen3_4b-genie-w4a16-qualcomm_qcs9075"}'
```

Confirm active public model id:

```bash
curl -s http://127.0.0.1:8088/v1/models
# expected id: qwen3_4b-genie-w4a16-qualcomm_qcs9075
```

Chat on active model id:

```bash
curl -s -X POST http://127.0.0.1:8088/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"qwen3_4b-genie-w4a16-qualcomm_qcs9075","messages":[{"role":"user","content":"Reply with OK"}],"stream":false}'
```

Switch back to Llama:

```bash
curl -s -X POST http://127.0.0.1:8088/v1/internal/models/load \
  -H 'Content-Type: application/json' \
  -d '{"model":"llama_v3_2_3b_instruct_ssd-genie-w4a16-qualcomm_qcs9075"}'
```

Prompt template is selected automatically from the active model:

- `llama*` → `llama3`
- `qwen*` → `qwen` (ChatML)
- `falcon*` → `falcon`
- unknown → `llama3` fallback

When loading a different model via `POST /v1/internal/models/load`, the service switches the runtime working directory to the target model folder before re-initializing Genie.

## Contract Tests

```bash
python3 tests/unified/run_http_suite.py \
  --suite tests/unified/suites/text-to-text.yaml \
  --target-host <TARGET_DEVICE_IP> \
  --output tests/unified/reports/ttt_45_case_report.json
```

## Common Failures

- `"messages" array is required`:
  - Payload is not OpenAI chat completions format.
- `400 invalid_request_error`:
  - validate request JSON shape (`messages` array, known fields)
- `503 model_busy`:
  - reduce overlap and retry with `Retry-After`

Expected error response shape (top failure):

```json
{
  "error": {
    "type": "server_error",
    "code": "model_busy",
    "message": "..."
  }
}
```

## Advanced References

- `core-services/text-to-text/MODEL_SETUP.md`
- `core-services/text-to-text/CODE_FLOW.md`
- `docs/API_CONTRACTS.md`
- `docs/TROUBLESHOOTING_GUIDE.md`
