#!/usr/bin/env bash
# ---------------------------------------------------------------------
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SUITE_PATH="${REPO_ROOT}/tests/unified/suites/image-to-text.yaml"
REPORT_DIR="${I2T_GATE_REPORT_DIR:-${REPO_ROOT}/tests/unified/reports}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
JSON_OUT="${REPORT_DIR}/i2t_release_gate_${STAMP}.json"
HTML_OUT="${REPORT_DIR}/i2t_release_gate_${STAMP}.html"
OPENAI_JSON_OUT="${REPORT_DIR}/i2t_openai_multiturn_${STAMP}.json"
OPENAI_HTML_OUT="${REPORT_DIR}/i2t_openai_multiturn_${STAMP}.html"

if ! command -v python3 >/dev/null 2>&1; then
  echo "FAIL: python3 is required"
  exit 1
fi
if [[ ! -f "${SUITE_PATH}" ]]; then
  echo "FAIL: suite not found: ${SUITE_PATH}"
  exit 1
fi
if ! python3 - <<'PY' >/dev/null 2>&1
import openai  # noqa: F401
PY
then
  echo "FAIL: openai package is required. Install with: python3 -m pip install --user -r tests/unified/requirements.txt"
  exit 1
fi

resolve_target_host() {
  if [[ -n "${I2T_TARGET_HOST:-}" ]]; then
    echo "${I2T_TARGET_HOST}"
    return
  fi

  if [[ -n "${I2T_API_BASE:-}" ]]; then
    python3 - <<'PY' "${I2T_API_BASE}"
import sys
from urllib.parse import urlparse
base = sys.argv[1]
try:
    host = urlparse(base).hostname or ""
except Exception:
    host = ""
print(host or "127.0.0.1")
PY
    return
  fi

  echo "127.0.0.1"
}

TARGET_HOST="$(resolve_target_host)"

echo "=== I2T Release Gate (Responses Contract) ==="
echo "repo_root=${REPO_ROOT}"
echo "suite=${SUITE_PATH}"
echo "target_host=${TARGET_HOST}"
echo "json_out=${JSON_OUT}"
echo "html_out=${HTML_OUT}"

mkdir -p "${REPORT_DIR}"

python3 "${REPO_ROOT}/tests/unified/run_http_suite.py" \
  --suite "${SUITE_PATH}" \
  --target-host "${TARGET_HOST}" \
  --output "${JSON_OUT}" \
  --html-out "${HTML_OUT}"

python3 - <<'PY' "${JSON_OUT}" "${TARGET_HOST}"
import json
import pathlib
import sys
import urllib.request

report_path = pathlib.Path(sys.argv[1])
target_host = sys.argv[2]
report = json.loads(report_path.read_text(encoding="utf-8"))
summary = report.get("summary", {})
failed = int(summary.get("failed", 0))
passed = int(summary.get("passed", 0))
total = int(summary.get("total", 0))

print(f"\nRelease gate summary: passed={passed} failed={failed} total={total}")

# Direct endpoint smoke using strict /v1/responses input[]
body = (
    '{"model":"qwen2.5-vl-7b-instruct","input":[{"role":"user",'
    '"content":[{"type":"input_text","text":"Reply with OK."}]}],'
    '"stream":false,"max_output_tokens":32}'
).encode("utf-8")
req = urllib.request.Request(
    f"http://{target_host}:8080/v1/responses",
    data=body,
    headers={"Content-Type": "application/json"},
    method="POST",
)
smoke_ok = False
try:
    with urllib.request.urlopen(req, timeout=60) as resp:
        smoke_ok = (resp.status == 200)
except Exception:
    smoke_ok = False

print(f"Direct /v1/responses smoke: {'PASS' if smoke_ok else 'FAIL'}")

if failed != 0 or not smoke_ok:
    raise SystemExit(1)
PY

echo "Running OpenAI SDK multi-turn smoke..."
python3 "${REPO_ROOT}/tests/unified/openai_i2t_responses_multiturn.py" \
  --target-host "${TARGET_HOST}" \
  --output "${OPENAI_JSON_OUT}" \
  --html-out "${OPENAI_HTML_OUT}"

echo "PASS: I2T release gate succeeded"
