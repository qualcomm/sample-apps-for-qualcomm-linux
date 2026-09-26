#!/bin/bash
# Copyright (c) 2024-2026 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
# Deterministic full-stack smoke validation using documented endpoints.
# ---------------------------------------------------------------------
set -u
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# shellcheck disable=SC1091
source "${REPO_ROOT}/scripts/lib/common.sh"

START_STACK=1
PRELOAD_I2T_CACHE=1
STATUS_TIMEOUT_SEC=420
EVIDENCE_BASE="${REPO_ROOT}/test-evidence"
TG_SMOKE_MODEL_ID_DEFAULT="genie"
STT_SMOKE_MODEL_ID_DEFAULT="whisper-1"
TG_SMOKE_MODEL_ID="${TG_SMOKE_MODEL_ID_DEFAULT}"
STT_SMOKE_MODEL_ID="${STT_SMOKE_MODEL_ID_DEFAULT}"

usage() {
    cat <<USAGE
Usage: $0 [--skip-start] [--skip-i2t-cache-preload] [--status-timeout-sec N] [--evidence-base DIR]

Options:
  --skip-start                 Do not call 'docker compose up -d'
  --skip-i2t-cache-preload     Skip Qwen2VLImageProcessor preload inside orchestrator
  --status-timeout-sec N       Timeout waiting for /api/status all-ok (default: 420)
  --evidence-base DIR          Evidence root directory (default: <repo>/test-evidence)
  --help                       Show this help
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-start)
            START_STACK=0
            shift
            ;;
        --skip-i2t-cache-preload)
            PRELOAD_I2T_CACHE=0
            shift
            ;;
        --status-timeout-sec)
            STATUS_TIMEOUT_SEC="${2:-}"
            shift 2
            ;;
        --evidence-base)
            EVIDENCE_BASE="${2:-}"
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            die "Unknown argument: $1"
            ;;
    esac
done

require_cmd docker
require_cmd curl
require_cmd python3
COMPOSE_CMD="$(detect_compose_cmd)"

RUN_TAG="$(date -u +%Y%m%dT%H%M%SZ)"
RUN_DIR="${EVIDENCE_BASE}/${RUN_TAG}"
LATEST_LINK="${EVIDENCE_BASE}/latest"
mkdir -p "${RUN_DIR}"
ln -sfn "${RUN_DIR}" "${LATEST_LINK}"

SUMMARY_FILE="${RUN_DIR}/summary.tsv"
COMMAND_LOG="${RUN_DIR}/commands.log"
touch "${SUMMARY_FILE}" "${COMMAND_LOG}"

record() {
    local status="$1"
    local test_id="$2"
    local note="$3"
    printf "%s\t%s\t%s\n" "${status}" "${test_id}" "${note}" | tee -a "${SUMMARY_FILE}"
}

run_logged() {
    local log_name="$1"
    shift
    local log_path="${RUN_DIR}/${log_name}.log"
    {
        echo "[$(timestamp_utc)] $*"
        "$@"
    } >"${log_path}" 2>&1
    local rc=$?
    echo "[$(timestamp_utc)] $* (rc=${rc})" >> "${COMMAND_LOG}"
    return "${rc}"
}

json_check() {
    local json_file="$1"
    local check_name="$2"
    python3 - "${json_file}" "${check_name}" <<'PY'
import json
import sys

path = sys.argv[1]
check = sys.argv[2]
with open(path, "r", encoding="utf-8") as f:
    data = json.load(f)

if check == "status_all_ok":
    services = data.get("services")
    if not isinstance(services, list) or not services:
        raise SystemExit(1)
    bad = []
    for svc in services:
        if not isinstance(svc, dict):
            bad.append("invalid-service-entry")
            continue
        name = str(svc.get("name", "unknown"))
        status = str(svc.get("status", "")).lower()
        if status != "ok":
            bad.append(f"{name}:{status}")
    if bad:
        print("NOT_OK " + ", ".join(bad))
        raise SystemExit(1)
    print("OK")
    raise SystemExit(0)

if check == "tg":
    choices = data.get("choices")
    if not isinstance(choices, list) or not choices:
        raise SystemExit(1)
    content = (((choices[0] or {}).get("message") or {}).get("content") or "").strip()
    if not content:
        raise SystemExit(1)
    print("OK")
    raise SystemExit(0)

if check == "stt":
    text = str(data.get("text", "")).strip()
    if not text:
        raise SystemExit(1)
    print("OK")
    raise SystemExit(0)

if check == "imagegen":
    items = data.get("data")
    if not isinstance(items, list) or not items:
        raise SystemExit(1)
    first = items[0] if isinstance(items[0], dict) else {}
    # Accept both OpenAI-compatible response formats:
    # - b64_json (inline payload)
    # - url (download URL)
    if not first.get("b64_json") and not first.get("url"):
        raise SystemExit(1)
    print("OK")
    raise SystemExit(0)

if check == "i2t_responses":
    output_text = str(data.get("output_text", "")).strip()
    if not output_text:
        raise SystemExit(1)
    print("OK")
    raise SystemExit(0)

raise SystemExit(2)
PY
}

select_smoke_models() {
    local models_file="$1"
    local out_file="$2"
    python3 - "${models_file}" > "${out_file}" <<'PY'
import json
import shlex
import sys

models_path = sys.argv[1]

with open(models_path, "r", encoding="utf-8") as f:
    payload = json.load(f)

raw_items = payload.get("data") if isinstance(payload, dict) else None
ids = []
if isinstance(raw_items, list):
    for item in raw_items:
        if isinstance(item, dict):
            mid = item.get("id")
            if isinstance(mid, str) and mid and mid not in ids:
                ids.append(mid)

def first_present(candidates):
    for c in candidates:
        if c in ids:
            return c
    return None

tg = first_present(["llama3.2-3B", "genie"])
if tg is None:
    blocked = ("whisper", "transcribe", "tts", "diffusion", "dall-e", "gpt-image", "image")
    for mid in ids:
        low = mid.lower()
        if any(token in low for token in blocked):
            continue
        tg = mid
        break
if tg is None:
    tg = ids[0] if ids else "genie"

stt = first_present([
    "whisper-tiny",
    "whisper-1",
    "gpt-4o-transcribe",
    "gpt-4o-mini-transcribe",
    "gpt-4o-mini-transcribe-2025-12-15",
    "gpt-4o-transcribe-diarize",
])
if stt is None:
    for mid in ids:
        low = mid.lower()
        if "whisper" in low or "transcribe" in low:
            stt = mid
            break
if stt is None:
    stt = "whisper-1"

print(f"TG_SMOKE_MODEL_ID={shlex.quote(tg)}")
print(f"STT_SMOKE_MODEL_ID={shlex.quote(stt)}")
PY
}

wait_for_stack_ready() {
    local deadline=$((SECONDS + STATUS_TIMEOUT_SEC))
    local status_file="${RUN_DIR}/orchestrator_status.json"

    if ! wait_for_http_ok "http://localhost:8090/health" 180 3; then
        return 1
    fi

    while (( SECONDS < deadline )); do
        if curl -fsS "http://localhost:8090/api/status" > "${status_file}" 2>"${RUN_DIR}/orchestrator_status.err"; then
            if json_check "${status_file}" "status_all_ok" > "${RUN_DIR}/orchestrator_status_check.log" 2>&1; then
                return 0
            fi
        fi
        sleep 5
    done

    return 1
}

failures=0
cd "${REPO_ROOT}"
load_versions_manifest "${REPO_ROOT}"

log_info "Evidence directory: ${RUN_DIR}"
record "INFO" "ENV" "repo_root=${REPO_ROOT}"
record "INFO" "ENV" "run_tag=${RUN_TAG}"
record "INFO" "ENV" "head_commit=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"

record "INFO" "PRECHECK" "Skipped (not part of validate pipeline)."

if [[ "${START_STACK}" == "1" ]]; then
    if run_logged "compose_up" ${COMPOSE_CMD} up -d; then
        record "PASS" "STACK_START" "${COMPOSE_CMD} up -d"
    else
        record "FAIL" "STACK_START" "${COMPOSE_CMD} up -d"
        failures=$((failures + 1))
    fi
else
    record "INFO" "STACK_START" "Skipped (--skip-start)"
fi

if wait_for_stack_ready; then
    record "PASS" "STATUS" "all services reported status=ok"
else
    record "FAIL" "STATUS" "stack did not become healthy within timeout=${STATUS_TIMEOUT_SEC}s"
    failures=$((failures + 1))
fi

if curl -fsS "http://localhost:8090/health" > "${RUN_DIR}/orchestrator_health.txt" 2>"${RUN_DIR}/orchestrator_health.err"; then
    record "PASS" "ORCH_HEALTH" "GET /health"
else
    record "FAIL" "ORCH_HEALTH" "GET /health"
    failures=$((failures + 1))
fi

if curl -fsS "http://localhost:8090/v1/models" > "${RUN_DIR}/models.json" 2>"${RUN_DIR}/models.err"; then
    if select_smoke_models "${RUN_DIR}/models.json" "${RUN_DIR}/selected_models.env" 2>"${RUN_DIR}/model_select.err"; then
        # shellcheck disable=SC1090
        source "${RUN_DIR}/selected_models.env"
        record "PASS" "MODEL_SELECT" "tg_model=${TG_SMOKE_MODEL_ID} stt_model=${STT_SMOKE_MODEL_ID}"
    else
        record "INFO" "MODEL_SELECT" "selector failed; using defaults tg=${TG_SMOKE_MODEL_ID_DEFAULT} stt=${STT_SMOKE_MODEL_ID_DEFAULT}"
    fi
else
    record "INFO" "MODEL_SELECT" "GET /v1/models failed; using defaults tg=${TG_SMOKE_MODEL_ID_DEFAULT} stt=${STT_SMOKE_MODEL_ID_DEFAULT}"
fi

cat > "${RUN_DIR}/tg_payload.json" <<EOF
{"model":"${TG_SMOKE_MODEL_ID}","messages":[{"role":"user","content":"Reply with OK"}],"stream":false}
EOF

if curl -fsS -X POST "http://localhost:8090/v1/chat/completions" \
    -H "Content-Type: application/json" \
    --data-binary "@${RUN_DIR}/tg_payload.json" \
    > "${RUN_DIR}/tg.json" 2> "${RUN_DIR}/tg.err" \
    && json_check "${RUN_DIR}/tg.json" "tg" > "${RUN_DIR}/tg.check" 2>&1; then
    record "PASS" "TG" "POST /v1/chat/completions model=${TG_SMOKE_MODEL_ID}"
else
    record "FAIL" "TG" "POST /v1/chat/completions model=${TG_SMOKE_MODEL_ID}"
    failures=$((failures + 1))
fi

if curl -fsS -X POST "http://localhost:8090/v1/audio/transcriptions" \
    -F "file=@tests/unified/fixtures/stt/1.wav" \
    -F "model=${STT_SMOKE_MODEL_ID}" \
    > "${RUN_DIR}/stt.json" 2> "${RUN_DIR}/stt.err" \
    && json_check "${RUN_DIR}/stt.json" "stt" > "${RUN_DIR}/stt.check" 2>&1; then
    record "PASS" "STT" "POST /v1/audio/transcriptions model=${STT_SMOKE_MODEL_ID}"
else
    record "FAIL" "STT" "POST /v1/audio/transcriptions model=${STT_SMOKE_MODEL_ID}"
    failures=$((failures + 1))
fi

if curl -fsS -o "${RUN_DIR}/tts.wav" -X POST "http://localhost:8090/v1/audio/speech" \
    -H "Content-Type: application/json" \
    --data-raw '{"model":"tts-1","voice":"alloy","input":"Hello from GenAI Studio","response_format":"wav"}' \
    2> "${RUN_DIR}/tts.err"; then
    if [[ -s "${RUN_DIR}/tts.wav" ]]; then
        record "PASS" "TTS" "POST /v1/audio/speech"
    else
        record "FAIL" "TTS" "output file empty"
        failures=$((failures + 1))
    fi
else
    record "FAIL" "TTS" "POST /v1/audio/speech"
    failures=$((failures + 1))
fi

if curl -fsS -X POST "http://localhost:8090/v1/images/generations" \
    -H "Content-Type: application/json" \
    --max-time 300 \
    --data-raw '{"model":"stable-diffusion-2-1","prompt":"a mountain at sunrise","size":"512x512"}' \
    > "${RUN_DIR}/imagegen.json" 2> "${RUN_DIR}/imagegen.err" \
    && json_check "${RUN_DIR}/imagegen.json" "imagegen" > "${RUN_DIR}/imagegen.check" 2>&1; then
    record "PASS" "IMAGEGEN" "POST /v1/images/generations"
else
    record "FAIL" "IMAGEGEN" "POST /v1/images/generations"
    failures=$((failures + 1))
fi

if [[ "${PRELOAD_I2T_CACHE}" == "1" ]]; then
    if docker exec -i image-to-text /opt/i2t-venv/bin/python3 - <<'PY' > "${RUN_DIR}/i2t_cache_preload.log" 2>&1
from transformers import Qwen2VLImageProcessor
Qwen2VLImageProcessor.from_pretrained("Qwen/Qwen2.5-VL-7B-Instruct")
print("Qwen2VLImageProcessor cache ready")
PY
    then
        record "PASS" "I2T_CACHE" "Qwen2VLImageProcessor preload in image-to-text"
    else
        record "FAIL" "I2T_CACHE" "Qwen2VLImageProcessor preload in image-to-text"
        failures=$((failures + 1))
    fi
else
    record "INFO" "I2T_CACHE" "Skipped (--skip-i2t-cache-preload)"
fi

# Deterministic guard for single-session I2T backend: clear KV state before
# responses checks so repeated smoke runs do not inherit stale sessions.
if curl -fsS -X POST "http://localhost:8080/v1/session/reset" \
    -H "Content-Type: application/json" \
    --data-raw '{}' \
    > "${RUN_DIR}/i2t_reset_guard.json" 2> "${RUN_DIR}/i2t_reset_guard.err"; then
    record "PASS" "I2T_RESET_GUARD" "POST /v1/session/reset before I2T_RESPONSES_VISION"
else
    record "FAIL" "I2T_RESET_GUARD" "POST /v1/session/reset before I2T_RESPONSES_VISION"
    failures=$((failures + 1))
fi

I2T_SESSION_ID="smoke-i2t-${RUN_TAG}"
I2T_TINY_PNG_DATA_URL="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+tmn8AAAAASUVORK5CYII="

cat > "${RUN_DIR}/i2t_vision_payload.json" <<EOF
{"session_id":"${I2T_SESSION_ID}","model":"qwen2.5-vl-7b-instruct","input":[{"role":"user","content":[{"type":"input_text","text":"Describe this image in one short sentence."},{"type":"input_image","image_url":"${I2T_TINY_PNG_DATA_URL}"}]}],"stream":false,"max_output_tokens":96}
EOF

if python3 -m json.tool "${RUN_DIR}/i2t_vision_payload.json" > "${RUN_DIR}/i2t_payload_build.log" 2>&1; then
        if curl -fsS -X POST "http://localhost:8090/v1/responses" \
            -H "Content-Type: application/json" \
            -H "X-Session-Id: ${I2T_SESSION_ID}" \
            --data-binary "@${RUN_DIR}/i2t_vision_payload.json" \
            > "${RUN_DIR}/i2t_vision.json" 2> "${RUN_DIR}/i2t_vision.err" \
            && json_check "${RUN_DIR}/i2t_vision.json" "i2t_responses" > "${RUN_DIR}/i2t_vision.check" 2>&1; then
            record "PASS" "I2T_RESPONSES_VISION" "POST /v1/responses with image input[]"
        else
            record "FAIL" "I2T_RESPONSES_VISION" "POST /v1/responses with image input[]"
            failures=$((failures + 1))
        fi

        cat > "${RUN_DIR}/i2t_chat_payload.json" <<EOF
{"session_id":"${I2T_SESSION_ID}","model":"qwen2.5-vl-7b-instruct","input":[{"role":"user","content":[{"type":"input_text","text":"Continue in one short sentence."}]}],"stream":false,"max_output_tokens":64}
EOF
        if curl -fsS -X POST "http://localhost:8090/v1/responses" \
            -H "Content-Type: application/json" \
            -H "X-Session-Id: ${I2T_SESSION_ID}" \
            --data-binary "@${RUN_DIR}/i2t_chat_payload.json" \
            > "${RUN_DIR}/i2t_chat.json" 2> "${RUN_DIR}/i2t_chat.err" \
            && json_check "${RUN_DIR}/i2t_chat.json" "i2t_responses" > "${RUN_DIR}/i2t_chat.check" 2>&1; then
            record "PASS" "I2T_RESPONSES_CHAT" "POST /v1/responses follow-up text"
        else
            record "FAIL" "I2T_RESPONSES_CHAT" "POST /v1/responses follow-up text"
            failures=$((failures + 1))
        fi
else
    record "FAIL" "I2T_VISION_PAYLOAD" "build strict input[] vision payload"
    failures=$((failures + 1))
fi

if ${COMPOSE_CMD} ps > "${RUN_DIR}/compose_ps.txt" 2> "${RUN_DIR}/compose_ps.err"; then
    record "PASS" "COMPOSE_PS" "${COMPOSE_CMD} ps"
else
    record "FAIL" "COMPOSE_PS" "${COMPOSE_CMD} ps"
    failures=$((failures + 1))
fi

if [[ "${failures}" -eq 0 ]]; then
    log_info "Smoke validation passed. Evidence: ${RUN_DIR}"
    exit 0
fi

log_error "Smoke validation failed (${failures} checks). Evidence: ${RUN_DIR}"
exit 1
