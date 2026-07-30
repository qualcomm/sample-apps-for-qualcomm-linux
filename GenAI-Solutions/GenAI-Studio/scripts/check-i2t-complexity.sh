#!/usr/bin/env bash
# ---------------------------------------------------------------------
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET_PATH="${1:-${REPO_ROOT}/core-services/image-to-text/src}"
CCN_WARN="${I2T_COMPLEXITY_CCN_WARN:-10}"
LENGTH_WARN="${I2T_COMPLEXITY_LENGTH_WARN:-120}"
TOP_N="${I2T_COMPLEXITY_TOP_N:-10}"

echo "=== I2T Complexity Check (warn-only) ==="
echo "target=${TARGET_PATH}"
echo "ccn_warn=${CCN_WARN} length_warn=${LENGTH_WARN} top_n=${TOP_N}"

if [[ ! -e "${TARGET_PATH}" ]]; then
  echo "FAIL: target path not found: ${TARGET_PATH}" >&2
  exit 1
fi

if ! command -v lizard >/dev/null 2>&1; then
  echo "WARN: lizard is not installed; skipping complexity analysis."
  exit 0
fi

tmp_output="$(mktemp)"
tmp_hotspots="$(mktemp)"
trap 'rm -f "${tmp_output}" "${tmp_hotspots}"' EXIT

lizard "${TARGET_PATH}" -l cpp -EIgnoreAssert > "${tmp_output}" || true

awk '
  /^------------------------------------------------$/ { collect=1; next }
  collect && $1 ~ /^[0-9]+$/ {
    print $2 "\t" $5 "\t" $1 "\t" $6
    next
  }
  collect && $1 !~ /^[0-9]+$/ { exit }
' "${tmp_output}" > "${tmp_hotspots}"

warn_count="$(
  awk -F '\t' -v ccn="${CCN_WARN}" -v len_limit="${LENGTH_WARN}" '
    ($1 + 0) > ccn || ($2 + 0) > len_limit { count++ }
    END { print count + 0 }
  ' "${tmp_hotspots}"
)"

echo "functions_over_threshold=${warn_count}"

if [[ "${warn_count}" -eq 0 ]]; then
  echo "WARN: no functions exceed the configured thresholds."
  exit 0
fi

echo "Top hotspots:"
awk -F '\t' -v ccn="${CCN_WARN}" -v len_limit="${LENGTH_WARN}" '
  ($1 + 0) > ccn || ($2 + 0) > len_limit { print }
' "${tmp_hotspots}" | sort -t $'\t' -k1,1nr -k2,2nr | head -n "${TOP_N}" | \
awk -F '\t' '
  BEGIN { print "CCN\tLEN\tNLOC\tLOCATION" }
  { print $1 "\t" $2 "\t" $3 "\t" $4 }
'

echo "WARN: thresholds are advisory only; this script does not fail the build."
