# Text-To-Speech Service (MeloTTS)

Text-To-Speech (TTS) serves synthesis on `:8083`.
Client traffic should use orchestrator `:8090`; direct `:8083` is for backend debugging.

Canonical build/run/rebuild commands live in repo-root `README.md`:

1. `One Bring-Up Path`
2. `Strict clean rebuild cycle`
3. `Six-service functional tests`

## Prerequisites

- model bundle under `/opt/genai-studio-models/text-to-speech/melo-tts-v73/files`
- QAIRT flat runtime libs under `/opt/qairt/current/qairt_245_flat_libs`
- host RPC libs found in `/usr/lib/aarch64-linux-gnu` or `/usr/lib`
- base image `ubuntu-runtime:24.04` available on target

Recommended model preparation flow (target device, scripted):

```bash
bash scripts/phases/model_gen.sh --service tts
```

For detailed model setup and overrides, see `core-services/text-to-speech/meloTTS/Model-Generation.md`.

Target preflight:

```bash
test -d /opt/genai-studio-models/text-to-speech/melo-tts-v73/files && echo "Text-to-Speech model directory OK"
test -d /opt/qairt/current/qairt_245_flat_libs && echo "QAIRT flat libs OK"
test -d core-services/text-to-speech/meloTTS/melo_sdk && echo "melo_sdk present" || \
  test -f core-services/text-to-speech/meloTTS/1.1.1.0.zip && echo "melo_sdk 1.1.1.0.zip present"

echo $HOST_RPC_LIB_DIR
ls -l "$HOST_RPC_LIB_DIR/libcdsprpc.so" \
      "$HOST_RPC_LIB_DIR/libcdsprpc.so.1" \
      "$HOST_RPC_LIB_DIR/libcdsprpc.so.1.0.0" \
      "$HOST_RPC_LIB_DIR/libdmabufheap.so.0"
ls -l /usr/lib/dsp/cdsp/fastrpc_shell_unsigned_3
```

> **Note:** If `HOST_RPC_LIB_DIR` is empty or not found, refer to `README.md` [Target preflight checks and runtime preparation](#2-target-preflight-checks-and-runtime-preparation) section to detect and persist the correct path. 

## Build & Start Service

Build the service image:

```bash
DOCKER_BUILDKIT=1 docker build --progress=plain -t text-to-speech:latest core-services/text-to-speech/meloTTS/
```

Start the service with required environment variables:

```bash
export TTS_QAIRT_FLAT_LIB_DIR=/opt/qairt/current/qairt_245_flat_libs
export TTS_MODEL_HOST_DIR=/opt/genai-studio-models/text-to-speech/melo-tts-v73/files

docker compose up -d text-to-speech
```

For full stack bring-up, refer to `README.md` [section 7) Start services with docker compose](#7-start-services-with-docker-compose). 

## Validate

Process env and mounts:

```bash
docker exec text-to-speech /bin/bash -lc \
  "tr '\0' '\n' </proc/1/environ | egrep 'MODEL_PATH|QAIRT|LD_LIBRARY_PATH|DSP_LIBRARY_PATH|ADSP_LIBRARY_PATH'"

docker exec text-to-speech /bin/bash -lc \
  "ls -lah /opt/TTS_binary/MeloTTS /opt/host-libs /usr/lib/dsp /usr/lib/dsp/cdsp 2>/dev/null"
```

Health:

```bash
curl -s http://localhost:8083/health
```

Expected response shape:

```json
{"status":"ok"}
```

Recommended client path (orchestrator):

```bash
curl -sS -o /tmp/tts_orchestrator.wav -X POST http://localhost:8090/v1/audio/speech \
  -H 'Content-Type: application/json' \
  -d '{"model":"tts-1","voice":"alloy","input":"Hello from orchestrator path","response_format":"wav"}'
```

Expected response shape:

```text
HTTP 200
Content-Type: audio/wav
binary wav payload (> 1 KB)
```

Direct backend smoke:

```bash
curl -sS -o /tmp/tts_short.wav -X POST http://localhost:8083/v1/audio/speech \
  -H 'Content-Type: application/json' \
  -d '{"model":"tts-1","voice":"alloy","input":"Hello from Melo TTS","response_format":"wav"}'
```

Expected response shape:

```text
HTTP 200
Content-Type: audio/wav
binary wav payload (> 1 KB)
```

## Known Runtime Contract

- accepted `response_format`: `wav`, `pcm`
- output content type is `audio/wav`
- `voice` currently fixed to `alloy`
- `speed` currently fixed to `1.0`
- busy engine returns `429` with `Retry-After: 1`

Expected error response shape (top failure):

```json
{
  "error": {
    "type": "rate_limit_error",
    "message": "engine busy, retry later"
  }
}
```

## Common Failures

- `melo_sdk/` or `1.1.1.0.zip` missing:
  - stage SDK from host into target repo path
- bind-mount error like `expected shared-library file but found directory`:
  - wrong `${HOST_RPC_LIB_DIR}`; re-detect and recreate container
- `TTSEngine::init() failed` or `tts_impl_open error=-1`:
  - verify `ADSP_LIBRARY_PATH` uses `;`
  - verify `/usr/lib/dsp/cdsp` appears in `ADSP_LIBRARY_PATH`
  - verify `/usr/lib/dsp/cdsp/fastrpc_shell_unsigned_3` exists
- startup error with `tts_impl_open error=-2147482618`:
  - set `TTS_ADSP_HOST_DIR` to a valid host ADSP runtime directory

## Advanced References

- `core-services/text-to-speech/meloTTS/Model-Generation.md`
- `docs/API_CONTRACTS.md`
- `docs/TROUBLESHOOTING_GUIDE.md`
