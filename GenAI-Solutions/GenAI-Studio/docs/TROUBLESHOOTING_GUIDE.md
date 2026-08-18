# GenAI Studio Troubleshooting Guide

Complete reference for diagnosing build, runtime, and service-level failures. Use the **Error Signatures** section for quick lookup, then follow the **5-Step Debug Playbook** for systematic diagnosis.

---

## Error Signatures: Quick Lookup

### Build-Time Errors

#### Missing repo-root QAIRT SDK slice
```
COPY qairt-sdk/ /opt/qairt/ ... "/qairt-sdk": not found
```
**Fix:** Ensure `qairt-sdk/` exists at repo root before `docker compose build`.

#### Missing STT SDK payload
```
COPY whisper_sdk/ ./whisper_sdk/ ... "/whisper_sdk": not found
```
**Fix:** Stage `core-services/speech-to-text/whisper_sdk/` before build. See [setup/DEVICE_SETUP.md](setup/DEVICE_SETUP.md).

#### Missing TTS SDK payload
```
[build] ERROR: neither melo_sdk directory nor fallback archive 1.1.1.0.zip found in build context.
```
**Fix:** Stage `core-services/text-to-speech/meloTTS/melo_sdk/` or `1.1.1.0.zip` before build.

#### Wrong host RPC bind source (directory instead of .so file)
```
[run] ERROR: expected shared-library file but found directory: /usr/lib/libcdsprpc.so
```
**Fix:** Set `HOST_RPC_LIB_DIR=/usr/lib/aarch64-linux-gnu` in `.env` and recreate containers.

---

### Runtime Errors: Missing Artifacts

#### Missing TTS runtime artifacts
```
[run] ERROR: required runtime library missing: /usr/lib/libtts.so
[run] ERROR: missing <MODEL_DIR>/libtts_impl_skel.so
[run] ERROR: no .qnn model found under <MODEL_DIR>
```
**Fix:** Verify TTS model mount via `TTS_MODEL_HOST_DIR` contains `.qnn` + `libtts_impl_skel.so`. See [TTS Path Triage](#tts-path-triage).

#### Missing T2I runtime artifacts
```
[run.sh] ERROR: Missing text_encoder/unet/vae context ...
[run.sh] ERROR: Tokenizer files not found under: ...
[run.sh] ERROR: Required QAIRT library missing: ...
```
**Fix:** Verify T2I model mount via `IMAGEGEN_MODEL_DIR` and `IMG_QAIRT_LIBS_HOST_DIR`. See [Text-To-Image Pain Points](#3-text-to-image).

#### Missing STT runtime artifacts
```
[run.sh] ERROR: required model artifacts not found under ...
[run.sh] ERROR: no VAD model found.
```
**Fix:** Verify STT model mount via `STT_MODEL_HOST_DIR` contains encoder/decoder bins + VAD model. See [Speech-To-Text Pain Points](#4-speech-to-text).

#### Missing I2T runtime artifact
```
[run.sh] ERROR: libGenie.so not found at: ...
```
**Fix:** Verify I2T model mount via `I2T_MODEL_HOST_DIR` contains `libGenie.so` and model assets.

---

### Runtime Errors: Service Responses

| Service | Success Signature | Error Signature | Meaning |
|---|---|---|---|
| **Text-To-Text** | `200 + choices[]` | `503 + error.code=model_busy` | Model saturated; use client-side backoff |
| **Image-To-Text** | `200 + output_text` | `409 + error.code=session_conflict` | Another session owns backend; reset and retry |
| **Text-To-Image** | `200 + data[0].b64_json` | `500 + Direct runner command failed with rc=256` | Backend auth or model error; check logs |
| **Speech-To-Text** | `200 + text` | `400 + Unsupported model ...` | Model id mismatch; query `/v1/models` first |
| **Text-To-Speech** | `200 + Content-Type: audio/wav` | `429 + Retry-After` | Service busy; use backoff retry |
| **Orchestrator** | `200 + /api/status.services[] + choices[]` | `503 + error.code=model_busy` | Upstream service down/busy; check backend logs |

---

### Special Case: CDI Device Injection Error
```
ERROR: CDI device injection failed: failed to inject devices: failed to stat CDI host device "/dev/kgsl-3d0": no such file or directory
```
**Fix:** Remove `/dev/kgsl-3d0` from `/etc/cdi/docker-run-cdi-hw-acc.json` on target.

---

## Service Binary/Payload Contract (What Must Exist)

| Service | Required Files/Folders | How Wired | Common Missing-File Error |
|---|---|---|---|
| **Text-To-Text** | `/opt/genai-studio-models/text-to-text/<model>/genie_config.json` + model bundle | Runtime mount via `TG_MODEL_HOST_DIR`; runtime checks in `core-services/text-to-text/run.sh` | `[run.sh] ERROR: genie_config.json not found at: ...` |
| **Image-To-Text** | `/opt/genai-studio-models/image-to-text/.../` files containing `libGenie.so` and model assets | Runtime mount via `I2T_MODEL_HOST_DIR`; direct endpoint is `/v1/responses` | `[run.sh] ERROR: libGenie.so not found at: ...` |
| **Text-To-Image** | SD2.1 context bins (`text_encoder*.bin`, `unet*.bin`, `vae*.bin`), tokenizer (`vocab.json`, `merges.txt`), QAIRT libs (`libQnnHtp.so`, `libQnnSystem.so`), `libQnnHtpV73Skel.so` in model or QAIRT ADSP dir | Runtime mounts via `IMAGEGEN_MODEL_DIR` and `IMG_QAIRT_LIBS_HOST_DIR`; env injected by compose | `[run.sh] ERROR: Missing text_encoder/unet/vae context ...`, tokenizer missing, or missing QAIRT/skel |
| **Speech-To-Text** | Build-time: `core-services/speech-to-text/whisper_sdk`; Runtime: model dir with `encoder.bin`/`decoder.bin` (or prefixed variants) + `vocab.bin`; VAD model `libnnvad_model.so` in model or `/opt/asr-assets` | `whisper_sdk` copied during image build; model mounted via `STT_MODEL_HOST_DIR`; QAIRT via `STT_QNN_LIB_HOST_DIR` | Build: `COPY whisper_sdk/ ... "/whisper_sdk": not found`; Runtime: required model artifacts or VAD not found |
| **Text-To-Speech** | Build-time: `core-services/text-to-speech/meloTTS/melo_sdk` or `1.1.1.0.zip`; Runtime: model dir with `.qnn` + `libtts_impl_skel.so`; runtime `libtts.so` in image | `melo_sdk` consumed at build; model mounted via `TTS_MODEL_HOST_DIR`; runtime path normalization in `meloTTS/run.sh` | Build: `neither melo_sdk directory nor fallback archive 1.1.1.0.zip found`; Runtime: missing `libtts.so`, missing `libtts_impl_skel.so`, or no `.qnn` |
| **Orchestrator** | No private SDK payload; depends on upstream services and `/var/run/docker.sock` | Compose env points to `8088`/`8081`/`8083`/`8084`/`8080` | `503 upstream_error` when upstream service is down/unreachable |

---

## 5-Step Debug Playbook

Use this playbook before deep debugging.

### Step 1: Status Snapshot

```bash
curl -s http://localhost:8090/health
curl -s http://localhost:8090/api/status | python3 -m json.tool
```

**Goal:**
- Confirm orchestrator liveness
- Identify which backend is down/unreachable/not-ready

---

### Step 2: Service-Level Liveness and Readiness

**Text-Generation:**

```bash
curl -s -i http://127.0.0.1:8088/health
curl -s -i http://127.0.0.1:8088/ready
curl -s http://127.0.0.1:8088/v1/models
```

If `/ready` is not `200`, do not run inference tests yet.

**Speech-To-Text via orchestrator (recommended):**

```bash
curl -s http://localhost:8090/api/status | python3 -m json.tool
curl -s -X POST http://localhost:8090/api/stt/transcribe \
  -F 'file=@tests/unified/fixtures/stt/1.wav' \
  -F 'task=transcribe' \
  -F 'response_format=json'
```

---

### Step 3: One Known-Good Request

**Orchestrator text request:**

```bash
curl -s -X POST http://localhost:8090/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"genie","messages":[{"role":"user","content":"Reply with OK"}],"stream":false}'
```

**Service-direct text request:**

```bash
curl -s -X POST http://127.0.0.1:8088/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"qwen3-4B","messages":[{"role":"user","content":"Reply with OK"}],"stream":false}'
```

---

### Step 4: Container Logs (Last 300 Lines)

```bash
docker logs --tail 300 text-to-text
docker logs --tail 300 orchestrator
```

**Look for:**
- Model init errors
- Missing runtime libraries
- Busy/timeout errors
- JSON validation failures

---

### Step 5: Capture Reproducibility Evidence

```bash
date -u
git rev-parse --abbrev-ref HEAD
git rev-parse HEAD
docker compose version
bash scripts/validate-stack.sh --skip-start
```

**Store in your report:**
- Failing command
- Expected result
- Actual result
- Log path

---

## Common Quick Diagnoses

| Error | Cause | Fix |
|---|---|---|
| STT `429` / `rate_limited` | Service is busy/saturated | Use client-side backoff retry; do not rely on server automatic retry |
| `invalid_request_error` for `model` | Wrong model id for endpoint | Use direct model id on `:8088`, use `genie` on orchestrator `:8090` |
| `/ready` non-200 | Model not ready | Verify model files and runtime mounts |
| `upstream_error` from orchestrator | Backend unreachable or backend error | Check backend container logs |
| `400 + Unsupported model` | Model id mismatch | Query `/v1/models` first, then use returned canonical model id |

---

## Service-Specific Pain Points & Fixes

### 1) Text-To-Text

**Pain points:**
- None observed in recent audits.

**Hardening ideas:**
- Keep functional smoke test in CI (`/v1/chat/completions`) after each target rebuild.

---

### 2) Image-To-Text

**Pain points:**
- Strict single-active-session behavior can return `409 session_conflict` if a previous `X-Session-Id` is still active.
- Large `data:` image payloads can fail preprocessing command launch.

**Hardening ideas:**
- Add an explicit pre-test reset step (`POST /api/i2t/reset`) in all smoke docs.
- Prefer `https://` image URLs for large assets.
- Add explicit limit/error text in service docs and test harness.

---

### 3) Text-To-Image

**Pain points:**
- Direct generation returned: `{"error":{"message":"Direct runner command failed with rc=256","type":"server_error"}}`
- Backend requires auth (`Authorization: Bearer qti-demo-key`) for direct `:8084`.
- Target compose lacked newer orchestration knobs used for mixed I2T/ImageGen arbitration.

**Observed log clue:**
```
rc=256 detected; I2T arbitration is disabled (IMG_I2T_ARBITRATION_ENABLED=0)
```

**Hardening ideas:**
- Sync target compose with canonical repo compose before bring-up.
- Ensure arbitration envs are present when mixed I2T + ImageGen loads are expected.
- Keep direct `:8084` as debug path; prefer orchestrator `:8090`.

---

### 4) Speech-To-Text

**Pain points:**
- Docs/test examples with `model=whisper-tiny` failed on this target build.

**What worked:**
- Using `model=whisper-1` succeeded for `/v1/audio/transcriptions`.

**Hardening ideas:**
- Do not hard-code STT model id in smoke docs.
- First query `/health` or `/v1/models`, then use returned canonical model id.

---

### 5) Text-To-Speech

**Pain points:**
- Env variables are present but path composition is hard to reason about (duplicate segments and mixed delimiters in runtime logs).

**Hardening ideas:**
- Normalize path construction in entrypoint/run script.
- Keep one canonical `LD_LIBRARY_PATH` and one canonical `ADSP_LIBRARY_PATH` template.

---

### 6) Orchestrator

**Pain points:**
- Chat proxy path worked, but image generation route failed when backend returned `rc=256`.
- Behavior depends on environment flags present in target compose, so repo drift is high risk.

**Hardening ideas:**
- Include a startup self-check endpoint that reports critical arbitration flags.
- Fail fast at startup when required flags are missing for mixed workloads.

---

## Triage Paths

### TTS Path Triage

**First commands:**

```bash
docker compose config | sed -n '/text-to-speech:/,/orchestrator:/p'
docker exec text-to-speech /bin/bash -lc \
  "tr '\0' '\n' </proc/1/environ | egrep 'MODEL_PATH|QAIRT|LD_LIBRARY_PATH|DSP_LIBRARY_PATH|ADSP_LIBRARY_PATH'"
docker exec text-to-speech /bin/bash -lc \
  "ls -lah /opt/TTS_binary/MeloTTS /opt/host-libs /usr/lib/dsp /usr/lib/dsp/cdsp 2>/dev/null"
```

**Expected:**
- `LD_LIBRARY_PATH` begins with `/opt/qairt/current/qairt_245_flat_libs:/opt/TTS_binary/MeloTTS:/opt/host-libs`
- `DSP_LIBRARY_PATH=/opt/TTS_binary/MeloTTS`
- `ADSP_LIBRARY_PATH` contains `/opt/TTS_binary/MeloTTS;/usr/lib/dsp;/usr/lib/dsp/cdsp`
- `/opt/TTS_binary/MeloTTS` contains a `.qnn` plus `libtts_impl_skel.so`, `libQnnSystem.so`, and `libQnnHtpV73.so`

**Escalation path:**

1. If `/opt/host-libs` contains directories instead of shared-library files, fix `${HOST_RPC_LIB_DIR}` and recreate the container.
   - Quick host-side check:
   ```bash
   ls -ld /usr/lib/libcdsprpc.so /usr/lib/libcdsprpc.so.1 /usr/lib/libdmabufheap.so.0 2>/dev/null || true
   ```
   - If those are directories, force:
   ```bash
   echo 'HOST_RPC_LIB_DIR=/usr/lib/aarch64-linux-gnu' > .env
   docker compose up -d --force-recreate text-to-speech text-to-text speech-to-text image-to-text orchestrator
   ```

2. If `/usr/lib/dsp/fastrpc_shell_unsigned_3` is missing but `/usr/lib/dsp/cdsp/fastrpc_shell_unsigned_3` exists, create the compatibility alias or ensure the compose/run path includes `/usr/lib/dsp/cdsp`.

3. If the target deployment copy still shows legacy folders such as `Text-To-Speech/`, refresh the repo copy before debugging further.

4. Only after env and mount checks pass should you treat the model artifact as the likely root cause.

---

### Image-To-Text Triage (Orchestrator Path)

Use only OpenAI Responses route for I2T checks on orchestrator (`http://localhost:8090`):
- `POST /v1/responses`

#### Case 1: Strict Payload Validation (`input[]` required)

**First command:**

```bash
curl -sS -i -X POST http://localhost:8090/v1/responses \
  -H 'Content-Type: application/json' \
  -d '{"messages":[{"role":"user","content":"hello"}]}'
```

**Expected:**
- HTTP `400`
- Response body contains `"'input' array is required"`

**Escalation path:**
1. Convert payload to strict Responses format (`input[]`).
2. Remove any `pixel_values_path` fields from request bodies.
3. Retry request.

---

#### Case 2: Wrapper Route Removed

**First command:**

```bash
curl -sS -i -X POST http://localhost:8090/api/i2t/vision \
  -H 'Content-Type: application/json' \
  -d '{}'
```

**Expected:**
- HTTP `404` or `405` (wrapper removed by design)

**Escalation path:**
1. Move client calls to `/v1/responses`.
2. Keep session continuity with `X-Session-Id` on each turn.

---

#### Case 3: I2T Payload Sent to Chat-Completions Path

**First command:**

```bash
curl -sS -i -X POST http://localhost:8090/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"genie","messages":[{"role":"user","content":[{"type":"text","text":"describe image"},{"type":"image_url","image_url":{"url":"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+tmn8AAAAASUVORK5CYII="}}]}]}'
```

**Expected:**
- HTTP `400`
- Response body instructs: `Image-To-Text is exposed only via POST /v1/responses`

**Escalation path:**
1. Keep `/v1/chat/completions` for text-generation only.
2. Send image/session I2T requests to `/v1/responses`.

---

#### Case 4: `session_conflict`

**First command:**

```bash
curl -sS -i -X POST http://localhost:8090/v1/responses \
  -H 'Content-Type: application/json' \
  -H 'X-Session-Id: sid-a' \
  -d '{"model":"qwen2.5-vl-7b-instruct","input":[{"role":"user","content":[{"type":"input_text","text":"follow-up"}]}],"stream":false}'
```

**Expected (when another session owns backend context):**
- HTTP `409`
- Response body contains `error.code=session_conflict`

**Escalation path:**
1. Read active session id from `X-Active-Session-Id` response header if present.
2. Reset and align session:
   ```bash
   curl -sS -X POST http://localhost:8090/api/i2t/reset \
     -H 'Content-Type: application/json' \
     -d '{"session_id":"<active-session-id>"}'
   ```
3. Retry with the active session id.
4. If conflict repeats, run release gate and attach logs.

---

#### Case 5: Repeated Pipeline `status=4`

**First command:**

```bash
docker logs --tail 400 orchestrator | grep -E "status=4|pipeline status=4" || true
```

**Expected:**
- Transient `status=4` may auto-recover (reset+retry once)
- No persistent stuck state after retry

**Escalation path:**
1. Manual reset:
   ```bash
   curl -sS -X POST http://localhost:8090/api/i2t/reset \
     -H 'Content-Type: application/json' \
     -d '{"session_id":"<active-session-id>"}'
   ```
2. Re-run image analyze with `/v1/responses` (`input[]` + `input_image.image_url`).
3. If still persistent, restart image-to-text once:
   ```bash
   docker restart image-to-text
   ```
4. Run full gate for reproducibility evidence:
   ```bash
   bash scripts/validate-i2t-release-gate.sh
   ```

---

## Required Documentation Guardrails

1. **Host vs target tags** — Put before every command block.
2. **Preflight check** — Reject stale/legacy target compose files.
3. **Model-id autodetection** — Use for STT test commands.
4. **Mandatory functional test per service** — Not only `/health`.
5. **Single canonical six-service runbook** — Link from root README.

---

## Summary

This guide consolidates:
- **Error signatures** — Quick lookup for build and runtime failures
- **Service contracts** — What files must exist for each service
- **5-step playbook** — Systematic diagnosis workflow
- **Service pain points** — Known issues and hardening ideas
- **Triage paths** — Deep-dive diagnosis for TTS and I2T
- **Quick diagnoses** — Common errors and fixes

**Start with error signatures for quick lookup, then follow the 5-step playbook for systematic diagnosis.**
