#!/usr/bin/env python3
# ---------------------------------------------------------------------
# Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
"""
Unified HTTP suite runner for core services.

Reads a YAML suite definition, executes cases with a common schema, and writes:
- JSON report
- optional HTML report
"""

from __future__ import annotations

import argparse
import datetime as dt
import html
import json
import os
import re
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple
from urllib.parse import urlsplit, urlunsplit

import requests
import yaml


SENTINEL = object()


def utc_now_iso() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def _fmt_string(value: str, context: Dict[str, Any]) -> str:
    try:
        return value.format(**context)
    except Exception:
        return value


def deep_format(obj: Any, context: Dict[str, Any]) -> Any:
    if isinstance(obj, str):
        return _fmt_string(obj, context)
    if isinstance(obj, list):
        return [deep_format(item, context) for item in obj]
    if isinstance(obj, dict):
        return {k: deep_format(v, context) for k, v in obj.items()}
    return obj


def _generate_word_sequence(total_words: int, gibberish: bool = False) -> str:
    if total_words <= 0:
        return ""
    words: List[str] = []
    for i in range(1, total_words + 1):
        if gibberish:
            words.append(f"zzxq{i:04d}")
        else:
            words.append(f"word{i:04d}")
    return " ".join(words)


def _expand_magic_text(value: str) -> str:
    out = re.sub(
        r"__GEN_WORDS_(\d+)__",
        lambda m: _generate_word_sequence(int(m.group(1)), gibberish=False),
        value,
    )
    out = re.sub(
        r"__GEN_GIBBERISH_(\d+)__",
        lambda m: _generate_word_sequence(int(m.group(1)), gibberish=True),
        out,
    )
    return out


def _hydrate_filetext(obj: Any, suite_dir: Path, repo_root: Path) -> Any:
    if isinstance(obj, str):
        value = _expand_magic_text(obj)
        if value.startswith("filetext:"):
            path_raw = value.split(":", 1)[1].strip()
            file_path = resolve_fixture_path(path_raw, suite_dir, repo_root)
            return file_path.read_text(encoding="utf-8")
        return value
    if isinstance(obj, list):
        return [_hydrate_filetext(item, suite_dir, repo_root) for item in obj]
    if isinstance(obj, dict):
        return {k: _hydrate_filetext(v, suite_dir, repo_root) for k, v in obj.items()}
    return obj


def json_lookup(payload: Any, path: str) -> Any:
    cur: Any = payload
    for token in path.split("."):
        if isinstance(cur, list):
            try:
                idx = int(token)
            except Exception:
                return SENTINEL
            if idx < 0 or idx >= len(cur):
                return SENTINEL
            cur = cur[idx]
            continue
        if isinstance(cur, dict):
            if token not in cur:
                return SENTINEL
            cur = cur[token]
            continue
        return SENTINEL
    return cur


def parse_var_overrides(raw_vars: List[str]) -> Dict[str, str]:
    parsed: Dict[str, str] = {}
    for item in raw_vars:
        if "=" not in item:
            raise ValueError(f"Invalid --var value (expected key=value): {item}")
        k, v = item.split("=", 1)
        k = k.strip()
        if not k:
            raise ValueError(f"Invalid --var key in: {item}")
        parsed[k] = v
    return parsed


def replace_loopback_host(base_url: str, target_host: str) -> str:
    parsed = urlsplit(base_url)
    host = (parsed.hostname or "").strip().lower()
    if host not in {"127.0.0.0", "127.0.0.1", "localhost"}:
        return base_url

    if not target_host.strip():
        return base_url

    port = f":{parsed.port}" if parsed.port else ""
    userinfo = ""
    if parsed.username:
        userinfo = parsed.username
        if parsed.password:
            userinfo += f":{parsed.password}"
        userinfo += "@"

    new_netloc = f"{userinfo}{target_host}{port}"
    return urlunsplit((parsed.scheme, new_netloc, parsed.path, parsed.query, parsed.fragment))


def merge_headers(base: Dict[str, Any], override: Dict[str, Any]) -> Dict[str, str]:
    merged: Dict[str, Any] = dict(base)
    merged.update(override)
    out: Dict[str, str] = {}
    for key, value in merged.items():
        if value is None:
            continue
        out[str(key)] = str(value)
    return out


def is_transient_failure(status_code: Optional[int], reason: str) -> bool:
    if status_code in {429, 502, 503, 504}:
        return True
    text = (reason or "").lower()
    transient_tokens = (
        "request error:",
        "connectionerror",
        "remotedisconnected",
        "failed to establish a new connection",
        "connection refused",
        "connection aborted",
        "read timed out",
        "temporarily unavailable",
        "upstream request failed",
        "body too small",
    )
    return any(token in text for token in transient_tokens)


def resolve_fixture_path(raw_path: str, suite_dir: Path, repo_root: Path) -> Path:
    p = Path(raw_path)
    if p.is_absolute():
        return p
    suite_candidate = (suite_dir / p).resolve()
    if suite_candidate.exists():
        return suite_candidate
    return (repo_root / p).resolve()


def _build_silence_wav_bytes(duration_s: float, sample_rate_hz: int = 16000) -> bytes:
    import struct

    num_samples = max(0, int(duration_s * sample_rate_hz))
    num_channels = 1
    bits_per_sample = 16
    block_align = num_channels * bits_per_sample // 8
    byte_rate = sample_rate_hz * block_align
    data_size = num_samples * block_align
    riff_size = 36 + data_size
    header = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF",
        riff_size,
        b"WAVE",
        b"fmt ",
        16,
        1,
        num_channels,
        sample_rate_hz,
        byte_rate,
        block_align,
        bits_per_sample,
        b"data",
        data_size,
    )
    return header + (b"\x00" * data_size)


def _build_pcm16_silence_bytes(duration_ms: int, sample_rate_hz: int = 16000) -> bytes:
    sample_count = max(0, int((duration_ms * sample_rate_hz) / 1000))
    return b"\x00\x00" * sample_count


def run_dependency_checks(
    dependencies: List[Dict[str, Any]],
    context: Dict[str, Any],
    default_timeout: float,
) -> Dict[str, Dict[str, Any]]:
    dep_results: Dict[str, Dict[str, Any]] = {}

    for dep in dependencies:
        name = str(dep.get("name", "")).strip()
        if not name:
            continue

        method = str(dep.get("method", "GET")).upper()
        url = _fmt_string(str(dep.get("url", "")), context)
        timeout = float(dep.get("timeout_s", default_timeout))
        expected = dep.get("statuses", [200])
        expected_set = {int(x) for x in expected}

        started = time.perf_counter()
        status_code: Optional[int] = None
        err = ""
        ok = False

        try:
            resp = requests.request(method=method, url=url, timeout=timeout)
            status_code = resp.status_code
            ok = status_code in expected_set
        except Exception as exc:
            err = f"{type(exc).__name__}: {exc}"

        dep_results[name] = {
            "name": name,
            "ok": ok,
            "method": method,
            "url": url,
            "status_code": status_code,
            "expected_statuses": sorted(expected_set),
            "error": err,
            "elapsed_ms": round((time.perf_counter() - started) * 1000.0, 3),
        }

    return dep_results


def evaluate_response(
    kind: str,
    expect: Dict[str, Any],
    response: requests.Response,
    response_text: str,
    response_json: Optional[Any],
) -> Tuple[bool, str]:
    status = response.status_code

    statuses = expect.get("statuses")
    status_class = expect.get("status_class")
    if statuses is None and status_class is None:
        status_class = "2xx" if kind == "positive" else "non-2xx"

    if statuses is not None:
        expected = {int(x) for x in statuses}
        if status not in expected:
            return False, f"status={status}, expected={sorted(expected)}"
    elif status_class == "2xx":
        if not (200 <= status < 300):
            return False, f"status={status}, expected=2xx"
    elif status_class == "non-2xx":
        if 200 <= status < 300:
            return False, f"status={status}, expected=non-2xx"
    else:
        return False, f"invalid status_class={status_class!r}"

    body_contains = expect.get("body_contains", [])
    for token in body_contains:
        token_s = str(token)
        if token_s not in response_text:
            return False, f"missing body token: {token_s!r}"

    ctype_contains = expect.get("content_type_contains")
    if ctype_contains is not None:
        ctype = str(response.headers.get("content-type", ""))
        if str(ctype_contains).lower() not in ctype.lower():
            return False, f"content-type mismatch: got={ctype!r}, expected contains={ctype_contains!r}"

    min_body_bytes = expect.get("min_body_bytes")
    if min_body_bytes is not None:
        body_size = len(response.content or b"")
        if body_size < int(min_body_bytes):
            return False, f"body too small: {body_size} < {int(min_body_bytes)}"

    json_keys = expect.get("json_keys", [])
    if json_keys:
        if response_json is None:
            return False, "response is not valid JSON"
        for key in json_keys:
            value = json_lookup(response_json, str(key))
            if value is SENTINEL:
                return False, f"missing json key path: {key}"

    body_contains_any_ci = expect.get("body_contains_any_ci", [])
    if body_contains_any_ci:
        text_lc = response_text.lower()
        if not any(str(token).lower() in text_lc for token in body_contains_any_ci):
            return False, f"missing any body token (ci): {body_contains_any_ci}"

    body_contains_all_ci = expect.get("body_contains_all_ci", [])
    for token in body_contains_all_ci:
        if str(token).lower() not in response_text.lower():
            return False, f"missing body token (ci): {token!r}"

    json_path_contains_any_ci = expect.get("json_path_contains_any_ci", [])
    for item in json_path_contains_any_ci:
        if not isinstance(item, dict):
            return False, "json_path_contains_any_ci entry must be object"
        path = str(item.get("path", "")).strip()
        if not path:
            return False, "json_path_contains_any_ci missing path"
        keywords = item.get("any_keywords", []) or []
        if not keywords:
            return False, f"json_path_contains_any_ci missing any_keywords for path={path}"
        min_keywords = int(item.get("min_keywords", 1))
        if response_json is None:
            return False, f"response is not valid JSON for path={path}"
        value = json_lookup(response_json, path)
        if value is SENTINEL:
            return False, f"missing json key path: {path}"
        haystack = str(value).lower()
        matched = sum(1 for kw in keywords if str(kw).lower() in haystack)
        if matched < min_keywords:
            return (
                False,
                f"path {path!r} matched {matched} keyword(s), required {min_keywords}, options={keywords}",
            )

    return True, "ok"


def apply_captures(
    capture_map: Dict[str, str],
    response: requests.Response,
    response_json: Optional[Any],
    context: Dict[str, Any],
) -> List[str]:
    notes: List[str] = []
    for var_name, selector in capture_map.items():
        selector_s = str(selector).strip()
        if not selector_s:
            continue

        if selector_s.startswith("header:"):
            header_name = selector_s.split(":", 1)[1].strip()
            value = response.headers.get(header_name)
        elif selector_s.startswith("json:"):
            path = selector_s.split(":", 1)[1].strip()
            value = None if response_json is None else json_lookup(response_json, path)
            if value is SENTINEL:
                value = None
        else:
            path = selector_s
            value = None if response_json is None else json_lookup(response_json, path)
            if value is SENTINEL:
                value = None

        if value is not None:
            context[var_name] = value
            notes.append(f"{var_name}={value}")

    return notes


def render_html_report(report: Dict[str, Any]) -> str:
    meta = report.get("meta", {})
    summary = report.get("summary", {})
    dependency = report.get("dependencies", {})
    results = report.get("results", [])

    dep_rows = []
    for _, dep in dependency.items():
        cls = "ok" if dep.get("ok") else "fail"
        dep_rows.append(
            "<tr>"
            f"<td>{html.escape(str(dep.get('name', '')))}</td>"
            f"<td>{html.escape(str(dep.get('method', '')))}</td>"
            f"<td>{html.escape(str(dep.get('url', '')))}</td>"
            f"<td class='{cls}'>{html.escape(str(dep.get('status_code')))}</td>"
            f"<td>{html.escape(str(dep.get('error', '')))}</td>"
            "</tr>"
        )

    case_rows = []
    for row in results:
        if row.get("skipped"):
            cls = "skip"
            verdict = "SKIP"
        elif row.get("ok"):
            cls = "ok"
            verdict = "PASS"
        else:
            cls = "fail"
            verdict = "FAIL"
        case_rows.append(
            "<tr>"
            f"<td>{html.escape(str(row.get('id', '')))}</td>"
            f"<td>{html.escape(str(row.get('kind', '')))}</td>"
            f"<td>{html.escape(str(row.get('name', '')))}</td>"
            f"<td>{html.escape(str(row.get('method', '')))}</td>"
            f"<td>{html.escape(str(row.get('path', '')))}</td>"
            f"<td class='{cls}'>{verdict}</td>"
            f"<td>{html.escape(str(row.get('status_code')))}</td>"
            f"<td>{html.escape(str(row.get('reason', '')))}</td>"
            "</tr>"
        )

    return f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <title>Unified Test Report - {html.escape(str(meta.get('service', 'unknown')))}</title>
  <style>
    body {{ font-family: Arial, sans-serif; margin: 20px; color: #1b1f23; }}
    h1, h2 {{ margin: 0.4em 0; }}
    .meta, .summary {{ margin: 12px 0 18px; }}
    table {{ border-collapse: collapse; width: 100%; margin: 12px 0 20px; }}
    th, td {{ border: 1px solid #d0d7de; padding: 6px 8px; text-align: left; font-size: 13px; vertical-align: top; }}
    th {{ background: #f6f8fa; }}
    .ok {{ color: #1a7f37; font-weight: 600; }}
    .fail {{ color: #cf222e; font-weight: 600; }}
    .skip {{ color: #9a6700; font-weight: 600; }}
    code {{ background: #f6f8fa; padding: 2px 4px; }}
  </style>
</head>
<body>
  <h1>Unified Test Report</h1>
  <div class="meta">
    <div><strong>Service:</strong> {html.escape(str(meta.get("service", "")))}</div>
    <div><strong>Base URL:</strong> <code>{html.escape(str(meta.get("base_url", "")))}</code></div>
    <div><strong>Suite:</strong> <code>{html.escape(str(meta.get("suite_file", "")))}</code></div>
    <div><strong>Generated:</strong> {html.escape(str(meta.get("generated_at_utc", "")))}</div>
  </div>
  <h2>Summary</h2>
  <div class="summary">
    <div>Total: {summary.get("total", 0)}</div>
    <div>Executed: {summary.get("executed", 0)}</div>
    <div>Passed: <span class="ok">{summary.get("passed", 0)}</span></div>
    <div>Failed: <span class="fail">{summary.get("failed", 0)}</span></div>
    <div>Skipped: <span class="skip">{summary.get("skipped", 0)}</span></div>
  </div>
  <h2>Dependencies</h2>
  <table>
    <thead><tr><th>Name</th><th>Method</th><th>URL</th><th>Status</th><th>Error</th></tr></thead>
    <tbody>
      {''.join(dep_rows) if dep_rows else '<tr><td colspan="5">None</td></tr>'}
    </tbody>
  </table>
  <h2>Cases</h2>
  <table>
    <thead>
      <tr><th>ID</th><th>Kind</th><th>Name</th><th>Method</th><th>Path</th><th>Result</th><th>HTTP</th><th>Reason</th></tr>
    </thead>
    <tbody>
      {''.join(case_rows)}
    </tbody>
  </table>
</body>
</html>
"""


def run_suite(
    suite_path: Path,
    output_path: Path,
    html_output_path: Optional[Path],
    base_url_override: Optional[str],
    var_overrides: Dict[str, str],
) -> int:
    repo_root = Path(__file__).resolve().parents[2]
    suite_dir = suite_path.parent
    raw = yaml.safe_load(suite_path.read_text(encoding="utf-8")) or {}

    meta_cfg = raw.get("meta", {}) or {}
    defaults = raw.get("defaults", {}) or {}
    cases = raw.get("cases", []) or []
    dependencies_cfg = raw.get("dependencies", []) or []
    suite_vars = raw.get("variables", {}) or {}

    context: Dict[str, Any] = {}
    for key, value in suite_vars.items():
        if isinstance(value, str):
            context[key] = os.path.expandvars(value)
        else:
            context[key] = value
    context.update(var_overrides)

    service = str(meta_cfg.get("service", "unknown"))
    target_host = str(context.get("target_host", "")).strip()
    suite_base = _fmt_string(str(meta_cfg.get("base_url", "")).strip(), context)

    base_url = ((base_url_override or "").strip() or suite_base).rstrip("/")
    if not base_url:
        fallback_host = target_host or "127.0.0.1"
        base_url = f"http://{fallback_host}:8080"

    if target_host:
        base_url = replace_loopback_host(base_url, target_host)

    context["base_url"] = base_url

    default_timeout = float(defaults.get("timeout_s", 30))
    default_headers = defaults.get("headers", {}) or {}

    dep_checks = run_dependency_checks(
        deep_format(dependencies_cfg, context),
        context,
        default_timeout=default_timeout,
    )

    results: List[Dict[str, Any]] = []
    case_outcomes: Dict[str, str] = {}
    positive_total = 0
    negative_total = 0
    passed = 0
    failed = 0
    skipped = 0

    for index, case_raw in enumerate(cases, start=1):
        case = _hydrate_filetext(deep_format(case_raw, context), suite_dir, repo_root)
        case_id = str(case.get("id", f"CASE-{index:03d}"))
        kind = str(case.get("kind", "positive"))
        name = str(case.get("name", case_id))
        request_cfg = case.get("request", {}) or {}
        expect_cfg = case.get("expect", {}) or {}
        capture_cfg = case.get("capture", {}) or {}
        requires = case.get("requires", []) or []
        requires = [str(x) for x in requires]
        requires_cases = case.get("requires_cases", []) or []
        requires_cases = [str(x) for x in requires_cases]

        if kind == "positive":
            positive_total += 1
        else:
            negative_total += 1

        blocked_deps = [d for d in requires if not dep_checks.get(d, {}).get("ok", False)]
        blocked_case_prereqs = [c for c in requires_cases if case_outcomes.get(c) != "pass"]
        if blocked_deps or blocked_case_prereqs:
            skipped += 1
            reason_parts: List[str] = []
            if blocked_deps:
                reason_parts.append(f"SKIPPED_DEPENDENCY: {', '.join(blocked_deps)}")
            if blocked_case_prereqs:
                reason_parts.append(f"SKIPPED_CASE_PREREQ: {', '.join(blocked_case_prereqs)}")
            reason = " ; ".join(reason_parts)
            results.append(
                {
                    "index": index,
                    "id": case_id,
                    "kind": kind,
                    "name": name,
                    "skipped": True,
                    "ok": False,
                    "reason": reason,
                    "status_code": None,
                    "elapsed_ms": 0.0,
                    "method": str(request_cfg.get("method", "GET")).upper(),
                    "path": str(request_cfg.get("path", request_cfg.get("url", ""))),
                    "url": None,
                    "response_preview": "",
                }
            )
            case_outcomes[case_id] = "skip"
            print(f"[{index:03d}/{len(cases):03d}] SKIP {case_id} ({name}) :: {reason}")
            continue

        method = str(request_cfg.get("method", "GET")).upper()
        path = str(request_cfg.get("path", ""))
        explicit_url = str(request_cfg.get("url", "")).strip()
        # Prefer request-level timeout, but keep case-level timeout as backward-compatible fallback.
        timeout_s = float(request_cfg.get("timeout_s", case.get("timeout_s", default_timeout)))
        if explicit_url:
            url = _fmt_string(explicit_url, context)
        else:
            url = f"{base_url}{path}"

        headers = merge_headers(
            deep_format(default_headers, context),
            deep_format(request_cfg.get("headers", {}) or {}, context),
        )

        json_body = request_cfg.get("json", None)
        if json_body is not None:
            json_body = deep_format(json_body, context)
        raw_body = request_cfg.get("raw_body", None)
        if isinstance(raw_body, str):
            raw_body = _fmt_string(raw_body, context)
        form_body = request_cfg.get("form", None)
        if form_body is not None:
            form_body = deep_format(form_body, context)

        file_specs = request_cfg.get("files", []) or []
        files_payload: Dict[str, Tuple[str, Any, str]] = {}
        opened_files = []
        missing_fixtures: List[str] = []
        try:
            for spec in file_specs:
                field = str(spec.get("field", "")).strip()
                if not field:
                    continue

                content_type = str(spec.get("content_type", "application/octet-stream"))
                filename = str(spec.get("filename", f"{field}.bin"))

                if "fixture" in spec:
                    fixture_raw = _fmt_string(str(spec.get("fixture", "")), context)
                    fixture_path = resolve_fixture_path(fixture_raw, suite_dir, repo_root)
                    if not fixture_path.exists():
                        missing_fixtures.append(str(fixture_path))
                        continue
                    fh = fixture_path.open("rb")
                    opened_files.append(fh)
                    files_payload[field] = (filename or fixture_path.name, fh, content_type)
                elif "generated_wav_silence_seconds" in spec:
                    dur_s = float(spec.get("generated_wav_silence_seconds", 0.5))
                    sample_rate_hz = int(spec.get("sample_rate_hz", 16000))
                    wav_bytes = _build_silence_wav_bytes(dur_s, sample_rate_hz=sample_rate_hz)
                    files_payload[field] = (filename, wav_bytes, content_type)
                elif "generated_pcm_silence_ms" in spec:
                    dur_ms = int(spec.get("generated_pcm_silence_ms", 500))
                    sample_rate_hz = int(spec.get("sample_rate_hz", 16000))
                    pcm_bytes = _build_pcm16_silence_bytes(dur_ms, sample_rate_hz=sample_rate_hz)
                    files_payload[field] = (filename, pcm_bytes, content_type)
                elif "inline_text" in spec:
                    text = _fmt_string(str(spec.get("inline_text", "")), context)
                    files_payload[field] = (filename, text.encode("utf-8"), content_type)
                else:
                    missing_fixtures.append(f"invalid file spec for field {field}")

            if missing_fixtures:
                skipped += 1
                results.append(
                    {
                        "index": index,
                        "id": case_id,
                        "kind": kind,
                        "name": name,
                        "skipped": True,
                        "ok": False,
                        "reason": f"SKIPPED_DEPENDENCY: missing fixture(s): {', '.join(missing_fixtures)}",
                        "status_code": None,
                        "elapsed_ms": 0.0,
                        "method": method,
                        "path": path or explicit_url,
                        "url": url,
                        "response_preview": "",
                    }
                )
                case_outcomes[case_id] = "skip"
                print(f"[{index:03d}/{len(cases):03d}] SKIP {case_id} ({name}) :: missing fixture(s)")
                continue

            started = time.perf_counter()
            status_code: Optional[int] = None
            response_preview = ""
            reason = ""
            ok = False
            captured_notes: List[str] = []
            retry_cfg = case.get("retry", {}) if isinstance(case.get("retry"), dict) else {}
            retry_attempts = int(
                retry_cfg.get(
                    "attempts",
                    defaults.get("transient_retry_attempts", 1),
                )
            )
            if retry_attempts < 1:
                retry_attempts = 1
            retry_sleep_s = float(
                retry_cfg.get(
                    "sleep_s",
                    defaults.get("transient_retry_sleep_s", 1.5),
                )
            )
            attempt = 0
            while True:
                attempt += 1
                retry_after_s = 0.0
                try:
                    kwargs: Dict[str, Any] = {
                        "method": method,
                        "url": url,
                        "headers": headers,
                        "timeout": timeout_s,
                    }
                    if json_body is not None:
                        kwargs["json"] = json_body
                    elif raw_body is not None:
                        kwargs["data"] = raw_body
                    elif form_body is not None:
                        kwargs["data"] = form_body
                    if files_payload:
                        kwargs["files"] = files_payload

                    response = requests.request(**kwargs)
                    status_code = response.status_code
                    retry_after_raw = response.headers.get("Retry-After", "")
                    try:
                        retry_after_s = float(retry_after_raw)
                    except Exception:
                        retry_after_s = 0.0
                    response_bytes = response.content or b""
                    response_preview = response_bytes[:320].decode("utf-8", errors="ignore")
                    response_text = response_bytes[:16384].decode("utf-8", errors="ignore")

                    try:
                        response_json = response.json()
                    except Exception:
                        response_json = None

                    ok, reason = evaluate_response(
                        kind=kind,
                        expect=expect_cfg,
                        response=response,
                        response_text=response_text,
                        response_json=response_json,
                    )
                    elapsed_ms = round((time.perf_counter() - started) * 1000.0, 3)
                    if ok and "max_elapsed_ms" in expect_cfg:
                        max_elapsed_ms = float(expect_cfg.get("max_elapsed_ms"))
                        if elapsed_ms > max_elapsed_ms:
                            ok = False
                            reason = f"elapsed_ms={elapsed_ms} exceeds max_elapsed_ms={max_elapsed_ms}"
                    if ok and capture_cfg:
                        captured_notes = apply_captures(capture_cfg, response, response_json, context)
                except Exception as exc:
                    elapsed_ms = round((time.perf_counter() - started) * 1000.0, 3)
                    reason = f"request error: {type(exc).__name__}: {exc}"
                    ok = False

                if ok:
                    break
                if attempt >= retry_attempts or not is_transient_failure(status_code, reason):
                    break
                print(
                    f"[retry] {case_id} transient failure on attempt {attempt}/{retry_attempts}: {reason}"
                )
                time.sleep(max(retry_sleep_s, retry_after_s))

            if ok:
                passed += 1
                state = "PASS"
                case_outcomes[case_id] = "pass"
            else:
                failed += 1
                state = "FAIL"
                case_outcomes[case_id] = "fail"

            if captured_notes:
                reason = f"{reason}; captured={'; '.join(captured_notes)}"

            results.append(
                {
                    "index": index,
                    "id": case_id,
                    "kind": kind,
                    "name": name,
                    "skipped": False,
                    "ok": ok,
                    "reason": reason,
                    "status_code": status_code,
                    "elapsed_ms": elapsed_ms,
                    "method": method,
                    "path": path or explicit_url,
                    "url": url,
                    "response_preview": response_preview,
                }
            )
            print(f"[{index:03d}/{len(cases):03d}] {state} {case_id} [{kind}] status={status_code} :: {name}")
        finally:
            for fh in opened_files:
                try:
                    fh.close()
                except Exception:
                    pass

    total = len(cases)
    executed = total - skipped
    summary = {
        "total": total,
        "executed": executed,
        "passed": passed,
        "failed": failed,
        "skipped": skipped,
        "pass_rate_percent": round((passed / executed * 100.0), 3) if executed else 0.0,
        "positive_total": positive_total,
        "negative_total": negative_total,
        "positive_passed": sum(1 for r in results if r.get("kind") == "positive" and r.get("ok")),
        "negative_passed": sum(1 for r in results if r.get("kind") != "positive" and r.get("ok")),
    }

    report = {
        "meta": {
            "service": service,
            "suite_file": str(suite_path),
            "base_url": base_url,
            "generated_at_utc": utc_now_iso(),
        },
        "dependencies": dep_checks,
        "summary": summary,
        "results": results,
    }

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(report, indent=2, ensure_ascii=True), encoding="utf-8")

    if html_output_path is not None:
        html_output_path.parent.mkdir(parents=True, exist_ok=True)
        html_output_path.write_text(render_html_report(report), encoding="utf-8")

    print("\n=== SUMMARY ===")
    print(json.dumps(summary, indent=2))
    print(f"JSON report: {output_path}")
    if html_output_path is not None:
        print(f"HTML report: {html_output_path}")

    return 1 if failed > 0 else 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Run unified HTTP service test suite from YAML.")
    parser.add_argument("--suite", required=True, help="Path to suite YAML file.")
    parser.add_argument("--output", required=True, help="Path to JSON report output.")
    parser.add_argument("--html-out", default="", help="Optional path to HTML report output.")
    parser.add_argument("--base-url", default="", help="Override base URL from suite.")
    parser.add_argument(
        "--target-host",
        default="",
        help="Target device IP/hostname. Replaces loopback host in suite URLs.",
    )
    parser.add_argument("--var", action="append", default=[], help="Variable override in key=value format.")
    args = parser.parse_args()

    suite_path = Path(args.suite).resolve()
    output_path = Path(args.output).resolve()
    html_output_path: Optional[Path]
    if args.html_out.strip():
        html_output_path = Path(args.html_out).resolve()
    else:
        html_output_path = output_path.with_suffix(".html")

    if not suite_path.exists():
        print(f"[fatal] suite file not found: {suite_path}", file=sys.stderr)
        return 2

    try:
        vars_override = parse_var_overrides(args.var)
    except Exception as exc:
        print(f"[fatal] {exc}", file=sys.stderr)
        return 2

    if args.target_host.strip():
        vars_override["target_host"] = args.target_host.strip()

    return run_suite(
        suite_path=suite_path,
        output_path=output_path,
        html_output_path=html_output_path,
        base_url_override=args.base_url.strip() or None,
        var_overrides=vars_override,
    )


if __name__ == "__main__":
    raise SystemExit(main())
