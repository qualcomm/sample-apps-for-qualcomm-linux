#!/usr/bin/env python3
# ---------------------------------------------------------------------
# Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
"""
OpenAI SDK smoke for I2T Responses multi-turn continuity.

Runs from host machine against orchestrator /v1 endpoint and validates:
1) image+text turn via client.responses.create(...)
2) text-only follow-up in same X-Session-Id

Outputs JSON report and optional HTML report.
"""

from __future__ import annotations

import argparse
import datetime as dt
import html
import json
import sys
import time
import uuid
from pathlib import Path
from typing import Any

from openai import OpenAI


TINY_PNG_DATA_URL = (
    "data:image/png;base64,"
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+tmn8AAAAASUVORK5CYII="
)


def utc_compact() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def _extract_output_text(resp: Any) -> str:
    # OpenAI SDK model exposes .output_text for responses API.
    txt = getattr(resp, "output_text", None)
    if isinstance(txt, str) and txt.strip():
        return txt.strip()

    # Fallback for compatibility with dict-like objects.
    if isinstance(resp, dict):
        out = resp.get("output_text")
        if isinstance(out, str) and out.strip():
            return out.strip()

    return ""


def render_html(report: dict[str, Any]) -> str:
    rows: list[str] = []
    for item in report.get("cases", []):
        ok = bool(item.get("ok"))
        cls = "ok" if ok else "fail"
        rows.append(
            "<tr>"
            f"<td>{html.escape(str(item.get('id', '')))}</td>"
            f"<td>{html.escape(str(item.get('name', '')))}</td>"
            f"<td class='{cls}'>{'PASS' if ok else 'FAIL'}</td>"
            f"<td>{html.escape(str(item.get('elapsed_ms', '')))}</td>"
            f"<td>{html.escape(str(item.get('reason', '')))}</td>"
            f"<td><pre>{html.escape(str(item.get('output_preview', '')))}</pre></td>"
            "</tr>"
        )

    summary = report.get("summary", {})
    return f"""<!doctype html>
<html lang=\"en\">
<head>
  <meta charset=\"utf-8\" />
  <title>I2T OpenAI SDK Multi-turn Report</title>
  <style>
    body {{ font-family: Arial, sans-serif; margin: 20px; color: #1b1f23; }}
    table {{ border-collapse: collapse; width: 100%; margin-top: 12px; }}
    th, td {{ border: 1px solid #d0d7de; padding: 8px; text-align: left; vertical-align: top; }}
    th {{ background: #f6f8fa; }}
    .ok {{ color: #1a7f37; font-weight: 700; }}
    .fail {{ color: #cf222e; font-weight: 700; }}
    pre {{ margin: 0; white-space: pre-wrap; }}
  </style>
</head>
<body>
  <h1>I2T OpenAI SDK Multi-turn Report</h1>
  <p><strong>Generated:</strong> {html.escape(str(report.get('generated_at_utc', '')))}</p>
  <p><strong>Base URL:</strong> <code>{html.escape(str(report.get('base_url', '')))}</code></p>
  <p><strong>Session ID:</strong> <code>{html.escape(str(report.get('session_id', '')))}</code></p>
  <p><strong>Summary:</strong> total={summary.get('total', 0)} passed={summary.get('passed', 0)} failed={summary.get('failed', 0)}</p>
  <table>
    <thead>
      <tr><th>ID</th><th>Name</th><th>Result</th><th>Elapsed (ms)</th><th>Reason</th><th>Output Preview</th></tr>
    </thead>
    <tbody>
      {''.join(rows)}
    </tbody>
  </table>
</body>
</html>
"""


def main() -> int:
    parser = argparse.ArgumentParser(description="Run OpenAI SDK I2T multi-turn smoke test.")
    parser.add_argument("--base-url", default="", help="OpenAI base URL (default uses --target-host).")
    parser.add_argument("--target-host", default="127.0.0.1", help="Target host used when --base-url is not set.")
    parser.add_argument("--port", type=int, default=8090, help="Orchestrator port (default: 8090).")
    parser.add_argument("--api-key", default="dummy-key", help="API key (dummy accepted).")
    parser.add_argument("--model", default="qwen2.5-vl-7b-instruct", help="Model id for I2T responses.")
    parser.add_argument("--session-id", default="", help="Optional fixed session id.")
    parser.add_argument("--max-output-tokens", type=int, default=96)
    parser.add_argument("--output", default="", help="JSON output report path.")
    parser.add_argument("--html-out", default="", help="Optional HTML output report path.")
    args = parser.parse_args()

    base_url = args.base_url.strip() or f"http://{args.target_host}:{args.port}/v1"
    session_id = args.session_id.strip() or f"openai-i2t-{uuid.uuid4().hex[:10]}"

    output_path: Path
    if args.output.strip():
        output_path = Path(args.output).resolve()
    else:
        output_path = Path(
            f"tests/unified/reports/i2t_openai_multiturn_{utc_compact()}.json"
        ).resolve()

    if args.html_out.strip():
        html_path = Path(args.html_out).resolve()
    else:
        html_path = output_path.with_suffix(".html")

    output_path.parent.mkdir(parents=True, exist_ok=True)

    client = OpenAI(base_url=base_url, api_key=args.api_key, timeout=240)

    cases: list[dict[str, Any]] = []

    def run_case(case_id: str, name: str, input_payload: list[dict[str, Any]]) -> None:
        started = time.perf_counter()
        ok = False
        reason = ""
        output_preview = ""
        try:
            resp = client.responses.create(
                model=args.model,
                input=input_payload,
                stream=False,
                max_output_tokens=args.max_output_tokens,
                extra_headers={"X-Session-Id": session_id},
            )
            output_text = _extract_output_text(resp)
            if output_text:
                ok = True
                output_preview = output_text[:300]
                reason = "ok"
            else:
                reason = "empty output_text"
        except Exception as exc:  # noqa: BLE001
            reason = f"{type(exc).__name__}: {exc}"

        cases.append(
            {
                "id": case_id,
                "name": name,
                "ok": ok,
                "reason": reason,
                "elapsed_ms": round((time.perf_counter() - started) * 1000.0, 3),
                "output_preview": output_preview,
            }
        )

    run_case(
        "OAI-I2T-P01",
        "responses.create image+text turn",
        [
            {
                "role": "user",
                "content": [
                    {"type": "input_text", "text": "Describe this image in one short sentence."},
                    {"type": "input_image", "image_url": TINY_PNG_DATA_URL},
                ],
            }
        ],
    )

    run_case(
        "OAI-I2T-P02",
        "responses.create follow-up same session",
        [
            {
                "role": "user",
                "content": [
                    {"type": "input_text", "text": "Now continue in one short sentence."}
                ],
            }
        ],
    )

    passed = sum(1 for c in cases if c.get("ok"))
    failed = len(cases) - passed

    report = {
        "generated_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "base_url": base_url,
        "model": args.model,
        "session_id": session_id,
        "summary": {
            "total": len(cases),
            "passed": passed,
            "failed": failed,
        },
        "cases": cases,
    }

    output_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    html_path.write_text(render_html(report), encoding="utf-8")

    print(json.dumps(report["summary"], indent=2))
    print(f"JSON: {output_path}")
    print(f"HTML: {html_path}")

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
