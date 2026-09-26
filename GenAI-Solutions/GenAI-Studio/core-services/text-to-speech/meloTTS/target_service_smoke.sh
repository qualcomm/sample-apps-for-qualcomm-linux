#!/usr/bin/env bash
# Copyright (c) 2024-2026 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
set -euo pipefail

# Native on-device smoke test for tts-service.
# Usage (on target):
#   bash target_service_smoke.sh
# Optional env overrides:
#   APP_ROOT, MELO_RUN_ROOT, MODEL_FILE, PORT

APP_ROOT="${APP_ROOT:-/opt/genai-studio-run/meloTTS_zip_test/meloTTS}"
MELO_RUN_ROOT="${MELO_RUN_ROOT:-/opt/genai-studio-run/melo_tts_run}"
MODEL_FILE="${MODEL_FILE:-$MELO_RUN_ROOT/melo_tts_binary/melo_en.64_bit.qnn_v2.33.0_notebook_v73.qnn}"
PORT="${PORT:-18086}"

SERVICE_BIN="$APP_ROOT/src/build/tts-service"
LOG_FILE="$APP_ROOT/service_smoke_${PORT}.log"
OUT_WAV="$APP_ROOT/service_smoke_${PORT}.wav"

if [[ ! -x "$SERVICE_BIN" ]]; then
  echo "[smoke] missing binary: $SERVICE_BIN"
  echo "[smoke] run: cd $APP_ROOT && bash build.sh"
  exit 1
fi

if [[ ! -f "$MODEL_FILE" ]]; then
  echo "[smoke] missing model file: $MODEL_FILE"
  exit 1
fi

export LD_LIBRARY_PATH="$APP_ROOT/melo_sdk/libs/npu/rpc_libraries/linux/aarch64:$MELO_RUN_ROOT/libs:/usr/lib:${LD_LIBRARY_PATH:-}"
export DSP_LIBRARY_PATH="$MELO_RUN_ROOT/melo_tts_binary"
export ADSP_LIBRARY_PATH="/usr/lib/rfsa/adsp:$MELO_RUN_ROOT/melo_tts_binary:$APP_ROOT/melo_sdk/libs/npu/rpc_libraries/linux/aarch64"

echo "[smoke] starting service on port $PORT ..."
"$SERVICE_BIN" \
  --model-path "$MODEL_FILE" \
  --language English \
  --port "$PORT" \
  --speaking-rate 1.0 \
  --pitch 0 \
  --volume-gain 0 \
  --sample-rate 44100 \
  > "$LOG_FILE" 2>&1 &
PID=$!
trap 'kill $PID >/dev/null 2>&1 || true' EXIT

for i in $(seq 1 30); do
  if curl -fsS "http://127.0.0.1:$PORT/health" >/tmp/tts_service_health.json 2>/dev/null; then
    break
  fi
  sleep 1
  if [[ "$i" -eq 30 ]]; then
    echo "[smoke] service failed to become healthy"
    cat "$LOG_FILE" || true
    exit 1
  fi
done

curl -fsS -X POST "http://127.0.0.1:$PORT/v1/audio/speech" \
  -H "Content-Type: application/json" \
  -d '{"model":"tts-1","input":"Hello from Melo service smoke test.","voice":"alloy","response_format":"wav","speed":1.0}' \
  -o "$OUT_WAV"

python3 - <<PY
import os, wave, json
print("health:", json.load(open("/tmp/tts_service_health.json")))
p = "$OUT_WAV"
print("wav_size_bytes:", os.path.getsize(p))
with wave.open(p, "rb") as w:
    print({"nchannels": w.getnchannels(), "framerate": w.getframerate(), "sampwidth": w.getsampwidth(), "nframes": w.getnframes()})
PY

echo "[smoke] OK"
echo "[smoke] wav: $OUT_WAV"
echo "[smoke] log: $LOG_FILE"
