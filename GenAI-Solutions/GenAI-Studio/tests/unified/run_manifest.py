#!/usr/bin/env python3
# ---------------------------------------------------------------------
# Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
"""
Run all unified service suites from manifest.yaml and produce combined reports.
"""

from __future__ import annotations

import argparse
import datetime as dt
import html
import json
import sys
import time
from pathlib import Path
from typing import Any, Dict, List

import yaml

from run_http_suite import parse_var_overrides, run_suite


def utc_compact() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def parse_service_base_overrides(raw: List[str]) -> Dict[str, str]:
    overrides: Dict[str, str] = {}
    for item in raw:
        if "=" not in item:
            raise ValueError(f"Invalid --service-base value (expected service=url): {item}")
        service, url = item.split("=", 1)
        service = service.strip()
        url = url.strip()
        if not service or not url:
            raise ValueError(f"Invalid --service-base value: {item}")
        overrides[service] = url
    return overrides


def render_combined_html(report: Dict[str, Any]) -> str:
    rows = []
    for svc in report.get("services", []):
        summary = svc.get("summary", {})
        failed = int(summary.get("failed", 0))
        skipped = int(summary.get("skipped", 0))
        cls = "ok" if failed == 0 else "fail"
        if failed == 0 and skipped > 0:
            cls = "warn"
        issue_items = []
        for item in svc.get("issues", [])[:6]:
            issue_items.append(
                "<li>"
                f"<strong>{html.escape(str(item.get('state', '')))}</strong> "
                f"<code>{html.escape(str(item.get('id', '')))}</code> "
                f"{html.escape(str(item.get('name', '')))} :: "
                f"{html.escape(str(item.get('reason', '')))}"
                "</li>"
            )
        issues_html = "<ul>" + "".join(issue_items) + "</ul>" if issue_items else "None"
        rows.append(
            "<tr>"
            f"<td>{html.escape(str(svc.get('service', '')))}</td>"
            f"<td>{html.escape(str(svc.get('suite', '')))}</td>"
            f"<td>{summary.get('total', 0)}</td>"
            f"<td>{summary.get('executed', 0)}</td>"
            f"<td>{summary.get('passed', 0)}</td>"
            f"<td class='fail'>{summary.get('failed', 0)}</td>"
            f"<td class='warn'>{summary.get('skipped', 0)}</td>"
            f"<td>{html.escape(str(svc.get('elapsed_s', 0.0)))}</td>"
            f"<td class='{cls}'>{html.escape(str(svc.get('status', '')))}</td>"
            f"<td>{issues_html}</td>"
            "</tr>"
        )

    totals = report.get("totals", {})
    return f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <title>Unified Core-Services Report</title>
  <style>
    body {{ font-family: Arial, sans-serif; margin: 20px; color: #1b1f23; }}
    h1, h2 {{ margin: 0.4em 0; }}
    .meta {{ margin: 12px 0 18px; }}
    table {{ border-collapse: collapse; width: 100%; margin-top: 12px; }}
    th, td {{ border: 1px solid #d0d7de; padding: 7px 8px; text-align: left; font-size: 13px; }}
    th {{ background: #f6f8fa; }}
    .ok {{ color: #1a7f37; font-weight: 600; }}
    .fail {{ color: #cf222e; font-weight: 600; }}
    .warn {{ color: #9a6700; font-weight: 600; }}
  </style>
</head>
<body>
  <h1>Unified Core-Services Report</h1>
  <div class="meta">
    <div><strong>Generated:</strong> {html.escape(str(report.get("generated_at_utc", "")))}</div>
    <div><strong>Manifest:</strong> <code>{html.escape(str(report.get("manifest", "")))}</code></div>
    <div><strong>Total services:</strong> {totals.get("service_count", 0)}</div>
    <div><strong>Total cases:</strong> {totals.get("total_cases", 0)}</div>
    <div><strong>Total executed:</strong> {totals.get("executed_cases", 0)}</div>
    <div><strong>Total passed:</strong> <span class="ok">{totals.get("passed_cases", 0)}</span></div>
    <div><strong>Total failed:</strong> <span class="fail">{totals.get("failed_cases", 0)}</span></div>
    <div><strong>Total skipped:</strong> <span class="warn">{totals.get("skipped_cases", 0)}</span></div>
    <div><strong>Wall elapsed (s):</strong> {totals.get("wall_elapsed_s", 0.0)}</div>
  </div>
  <h2>Per-service summary</h2>
  <table>
    <thead>
      <tr><th>Service</th><th>Suite</th><th>Total</th><th>Executed</th><th>Passed</th><th>Failed</th><th>Skipped</th><th>Elapsed(s)</th><th>Status</th><th>Top Issues</th></tr>
    </thead>
    <tbody>
      {''.join(rows)}
    </tbody>
  </table>
</body>
</html>
"""


def main() -> int:
    parser = argparse.ArgumentParser(description="Run unified service suites from manifest.")
    parser.add_argument(
        "--manifest",
        default="tests/unified/manifest.yaml",
        help="Path to unified manifest YAML.",
    )
    parser.add_argument(
        "--output-dir",
        default="",
        help="Output directory. Default: tests/unified/reports/<UTC_TS>/",
    )
    parser.add_argument(
        "--services",
        default="",
        help="Comma-separated service filter (default runs all in manifest).",
    )
    parser.add_argument(
        "--service-base",
        action="append",
        default=[],
        help="Override suite base URL: service=url. Can repeat.",
    )
    parser.add_argument(
        "--target-host",
        default="",
        help="Target device IP/hostname used by all suites unless overridden per service.",
    )
    parser.add_argument(
        "--var",
        action="append",
        default=[],
        help="Global variable override for all suites: key=value. Can repeat.",
    )
    args = parser.parse_args()

    manifest_path = Path(args.manifest).resolve()
    if not manifest_path.exists():
        print(f"[fatal] manifest not found: {manifest_path}", file=sys.stderr)
        return 2

    try:
        service_base_overrides = parse_service_base_overrides(args.service_base)
        global_vars = parse_var_overrides(args.var)
    except Exception as exc:
        print(f"[fatal] {exc}", file=sys.stderr)
        return 2

    if args.target_host.strip():
        global_vars["target_host"] = args.target_host.strip()

    raw = yaml.safe_load(manifest_path.read_text(encoding="utf-8")) or {}
    services = raw.get("services", []) or []
    selected = {s.strip() for s in args.services.split(",") if s.strip()}

    if args.output_dir.strip():
        output_dir = Path(args.output_dir).resolve()
    else:
        output_dir = (manifest_path.parent / "reports" / utc_compact()).resolve()
    services_out_dir = output_dir / "services"
    services_out_dir.mkdir(parents=True, exist_ok=True)

    service_reports: List[Dict[str, Any]] = []
    aggregate_exit = 0
    started_all = time.perf_counter()

    for service_entry in services:
        service = str(service_entry.get("service", "")).strip()
        if not service:
            continue
        if selected and service not in selected:
            continue

        suite_rel = str(service_entry.get("suite", "")).strip()
        if not suite_rel:
            continue
        suite_path = Path(suite_rel)
        if not suite_path.is_absolute():
            suite_path = (manifest_path.parents[2] / suite_path).resolve()

        json_out = services_out_dir / f"{service}.json"
        html_out = services_out_dir / f"{service}.html"
        base_override = service_base_overrides.get(service)

        print(f"\n=== Running {service} ===")
        started_service = time.perf_counter()
        exit_code = run_suite(
            suite_path=suite_path,
            output_path=json_out,
            html_output_path=html_out,
            base_url_override=base_override,
            var_overrides=global_vars,
        )
        elapsed_service_s = round(time.perf_counter() - started_service, 3)
        aggregate_exit = max(aggregate_exit, exit_code)

        report = json.loads(json_out.read_text(encoding="utf-8"))
        issues: List[Dict[str, Any]] = []
        for row in report.get("results", []):
            if row.get("ok") and not row.get("skipped"):
                continue
            issues.append(
                {
                    "state": "SKIP" if row.get("skipped") else "FAIL",
                    "id": row.get("id", ""),
                    "name": row.get("name", ""),
                    "reason": row.get("reason", ""),
                    "status_code": row.get("status_code"),
                }
            )
        service_reports.append(
            {
                "service": service,
                "suite": str(suite_path),
                "status": "PASS" if report["summary"]["failed"] == 0 else "FAIL",
                "summary": report.get("summary", {}),
                "elapsed_s": elapsed_service_s,
                "issues": issues,
                "json_report": str(json_out),
                "html_report": str(html_out),
            }
        )

    wall_elapsed_s = round(time.perf_counter() - started_all, 3)
    totals = {
        "service_count": len(service_reports),
        "total_cases": sum(int(x["summary"].get("total", 0)) for x in service_reports),
        "executed_cases": sum(int(x["summary"].get("executed", 0)) for x in service_reports),
        "passed_cases": sum(int(x["summary"].get("passed", 0)) for x in service_reports),
        "failed_cases": sum(int(x["summary"].get("failed", 0)) for x in service_reports),
        "skipped_cases": sum(int(x["summary"].get("skipped", 0)) for x in service_reports),
        "wall_elapsed_s": wall_elapsed_s,
    }

    combined = {
        "generated_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "manifest": str(manifest_path),
        "totals": totals,
        "services": service_reports,
    }

    combined_json = output_dir / "report.json"
    combined_html = output_dir / "report.html"
    combined_json.write_text(json.dumps(combined, indent=2), encoding="utf-8")
    combined_html.write_text(render_combined_html(combined), encoding="utf-8")

    print("\n=== COMBINED SUMMARY ===")
    print(json.dumps(totals, indent=2))
    print(f"Combined JSON: {combined_json}")
    print(f"Combined HTML: {combined_html}")

    return aggregate_exit


if __name__ == "__main__":
    raise SystemExit(main())
