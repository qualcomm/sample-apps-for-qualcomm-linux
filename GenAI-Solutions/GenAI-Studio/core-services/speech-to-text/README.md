# Speech-To-Text Service

Speech-To-Text (STT) serves transcription and translation on `:8081`.
Client traffic should use orchestrator `:8090`; direct `:8081` is for backend debugging.

Canonical build/run/rebuild commands live in repo-root `README.md`:

1. `One Bring-Up Path`
2. `Strict clean rebuild cycle`
3. `Six-service functional tests`

## Prerequisites

- model bundle under `/opt/genai-studio-models/speech-to-text/whisper_tiny-qnn_context_binary-float-qualcomm_qcs9075`
- QAIRT flat runtime libs under `/opt/qairt/current/qairt_260_flat_libs`
  (override with `STT_QNN_LIB_HOST_DIR` if your layout/version differs)

Quick checks on target:

```bash
test -d core-services/speech-to-text/whisper_sdk && echo "whisper_sdk present"
test -d /opt/genai-studio-models/speech-to-text/whisper_tiny-qnn_context_binary-float-qualcomm_qcs9075 && echo "Speech-to-Text model directory OK"
test -d /opt/qairt/current/qairt_260_flat_libs && echo "QAIRT flat libs OK"
```
## Build and Start Service

Build the service image

```bash
DOCKER_BUILDKIT=1 docker build --progress=plain -t speech-to-text:latest core-services/speech-to-text/
```

Export required environment variables and start the container:

```bash
export STT_QNN_LIB_HOST_DIR=/opt/qairt/current/qairt_260_flat_libs
export STT_MODEL_HOST_DIR=/opt/genai-studio-models/speech-to-text/whisper_tiny-qnn_context_binary-float-qualcomm_qcs9075

docker compose up -d speech-to-text
```

For full stack bring-up, refer to `README.md` [section 7) Start services with docker compose](#7-start-services-with-docker-compose). 

## Validate

Health and orchestrator status:

```bash
curl -s http://localhost:8081/health
curl -s http://localhost:8090/api/status | python3 -m json.tool
```

Expected response shape:

```json
{
  "stt_health": {"status":"ok","model":"<model-id>"},
  "orchestrator_status": {"services":[{"name":"speech_to_text","status":"ok"}]}
}
```

Recommended client path:

```bash
curl -s -X POST http://localhost:8090/api/stt/transcribe \
  -F 'file=@tests/unified/fixtures/stt/1.wav' \
  -F 'task=transcribe' \
  -F 'response_format=json'
```

Expected response shape:

```json
{"text":"..."}
```

OpenAI-compatible paths:

```bash
STT_MODEL=$(curl -s http://localhost:8081/health | python3 -c 'import json,sys; print(json.load(sys.stdin).get("model","whisper-1"))')

curl -s -X POST http://localhost:8090/v1/audio/transcriptions \
  -F 'file=@tests/unified/fixtures/stt/1.wav' \
  -F "model=${STT_MODEL}"

curl -s -X POST http://localhost:8090/v1/audio/translations \
  -F 'file=@tests/unified/fixtures/stt/1.wav' \
  -F "model=${STT_MODEL}"
```

Expected response shape:

```json
{"text":"..."}
```

Contract tests:

```bash
python3 tests/unified/run_http_suite.py \
  --suite tests/unified/suites/speech-to-text.yaml \
  --target-host <TARGET_DEVICE_IP> \
  --output tests/unified/reports/stt_45_case_report.json
```

## Endpoint Guide

- `POST /api/stt/transcribe`: default client upload path
- `POST /v1/audio/transcriptions`: OpenAI-style transcribe
- `POST /v1/audio/translations`: OpenAI-style translate
- `POST /v1/audio/transcriptions/stream`: SSE upload path
- `/v1/realtime/*`: advanced pre-recorded chunk session flow (`create -> append -> finalize`)

## Common Failures

- `whisper_sdk/` missing at build time:
  - stage SDK from host into `core-services/speech-to-text/whisper_sdk`
- `libQnn*` not found at runtime:
  - set `STT_QNN_LIB_HOST_DIR` to your device runtime flat-lib directory
    (example: `/opt/qairt/current/qairt_260_flat_libs`)
- `429 rate_limited`:
  - reduce concurrent requests and retry with backoff
- orchestrator route fails but backend is healthy:
  - check `http://localhost:8090/api/status` and restart orchestrator

Expected error response shape (top failure):

```json
{
  "error": {
    "type": "invalid_request_error",
    "message": "Unsupported model '<model-id>' for transcriptions"
  }
}
```

## Advanced References

- `core-services/speech-to-text/MODEL_SETUP.md`
- `core-services/speech-to-text/CODE_FLOW.md`
- `docs/API_CONTRACTS.md`
- `docs/TROUBLESHOOTING_GUIDE.md`
