#!/usr/bin/env python3
# ---------------------------------------------------------------------
# Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
"""Validate unified suites stay on a shared naming/count contract."""

from __future__ import annotations

from collections import Counter
import sys
from pathlib import Path
import re

import yaml

SERVICE_CODES = {
    "text-to-text": "TTT",
    "speech-to-text": "STT",
    "image-to-text": "I2T",
    "text-to-image": "T2I",
    "text-to-speech": "TTS",
    "orchestrator": "ORC",
}


def _validate_case_ids(
    case_ids: list[str],
    case_kinds: list[str],
    prefix: str,
) -> tuple[list[str], list[str], list[str], list[str]]:
    id_counter = Counter(case_ids)
    duplicate = [cid for cid, count in sorted(id_counter.items()) if count > 1]

    pattern = re.compile(rf"^{re.escape(prefix)}-(P|N)(\d{{2}})$")
    invalid: list[str] = []
    pos_nums: list[int] = []
    neg_nums: list[int] = []
    kind_mismatch: list[str] = []

    for cid, kind in zip(case_ids, case_kinds):
        m = pattern.fullmatch(cid)
        if not m:
            invalid.append(cid)
            continue
        marker = m.group(1)
        num = int(m.group(2))
        if marker == "P":
            pos_nums.append(num)
            if kind != "positive":
                kind_mismatch.append(cid)
        else:
            neg_nums.append(num)
            if kind != "negative":
                kind_mismatch.append(cid)

    missing: list[str] = []
    unexpected: list[str] = invalid
    if pos_nums:
        expected_pos = set(range(1, len(pos_nums) + 1))
        got_pos = set(pos_nums)
        for n in sorted(expected_pos - got_pos):
            missing.append(f"{prefix}-P{n:02d}")
        for n in sorted(got_pos - expected_pos):
            unexpected.append(f"{prefix}-P{n:02d}")
    if neg_nums:
        expected_neg = set(range(1, len(neg_nums) + 1))
        got_neg = set(neg_nums)
        for n in sorted(expected_neg - got_neg):
            missing.append(f"{prefix}-N{n:02d}")
        for n in sorted(got_neg - expected_neg):
            unexpected.append(f"{prefix}-N{n:02d}")

    return missing, unexpected, duplicate, kind_mismatch


def main() -> int:
    root = Path(__file__).resolve().parent
    suites = sorted((root / "suites").glob("*.yaml"))
    if not suites:
        print("[fatal] no suite YAML files found")
        return 2

    failed = False
    for suite in suites:
        raw = yaml.safe_load(suite.read_text(encoding="utf-8")) or {}
        cases = raw.get("cases", []) or []
        total = len(cases)
        meta = raw.get("meta", {}) or {}
        service = meta.get("service", suite.stem)
        service_code = SERVICE_CODES.get(service)
        case_ids = [str(case.get("id", "")) for case in cases]
        case_kinds = [str(case.get("kind", "")).lower() for case in cases]
        positive_kinds = sum(1 for kind in case_kinds if kind == "positive")
        negative_kinds = sum(1 for kind in case_kinds if kind == "negative")
        target_cases = meta.get("target_cases")
        expected_case_count = int(target_cases) if target_cases is not None else total

        checks: list[str] = []
        if total != expected_case_count:
            checks.append(f"total={total} expected={expected_case_count}")
        if target_cases is None:
            checks.append("meta.target_cases missing")
        if positive_kinds == 0:
            checks.append("no positive cases")
        if negative_kinds == 0:
            checks.append("no negative cases")

        if not service_code:
            checks.append("unknown service code mapping")
            missing_ids: list[str] = []
            unexpected_ids: list[str] = []
            duplicate_ids: list[str] = []
            kind_mismatch_ids: list[str] = []
        else:
            missing_ids, unexpected_ids, duplicate_ids, kind_mismatch_ids = _validate_case_ids(
                case_ids,
                case_kinds,
                service_code,
            )
            if missing_ids:
                checks.append(f"missing_ids={len(missing_ids)}")
            if unexpected_ids:
                checks.append(f"unexpected_ids={len(unexpected_ids)}")
            if duplicate_ids:
                checks.append(f"duplicate_ids={len(duplicate_ids)}")
            if kind_mismatch_ids:
                checks.append(f"kind_mismatch_ids={len(kind_mismatch_ids)}")

        ok = not checks
        state = "OK" if ok else "FAIL"
        print(f"{state:4s} {service:16s} cases={total:2d} file={suite}")
        if not ok:
            print(f"      issues: {', '.join(checks)}")
            if missing_ids:
                print(f"      missing: {', '.join(missing_ids[:6])}")
            if unexpected_ids:
                print(f"      unexpected: {', '.join(unexpected_ids[:6])}")
            if duplicate_ids:
                print(f"      duplicate: {', '.join(duplicate_ids[:6])}")
            if kind_mismatch_ids:
                print(f"      kind-mismatch: {', '.join(kind_mismatch_ids[:6])}")
        if not ok:
            failed = True

    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
