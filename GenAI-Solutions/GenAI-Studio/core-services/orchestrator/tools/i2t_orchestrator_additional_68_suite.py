#!/usr/bin/env python3
# ---------------------------------------------------------------------
# Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
# Additional I2T + Orchestrator validation matrix (68 cases)
#   - 14 positive cases
#   - 54 negative cases
# Intended to complement i2t_orchestrator_extensive_suite.py (70 cases)
# so combined coverage exceeds 100 total positive/negative checks.
# ---------------------------------------------------------------------

from __future__ import annotations

import json
import os
import time
from dataclasses import dataclass
from io import BytesIO
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Set, Tuple

import requests
from PIL import Image

I2T_BASE_URL = os.getenv("I2T_BASE_URL", "http://127.0.0.1:8080").rstrip("/")
ORCH_BASE_URL = os.getenv("ORCH_BASE_URL", "http://127.0.0.1:8090").rstrip("/")
TIMEOUT = float(os.getenv("I2T_ORCH_TEST_TIMEOUT", "180"))
WORKDIR = Path(os.getenv("I2T_ORCH_TEST_WORKDIR", "/tmp/i2t_orch_additional_suite"))

NEGATIVE_CODES: Set[int] = {400, 404, 405, 409, 415, 422, 500, 502, 503}


@dataclass
class CaseResult:
    case_id: int
    name: str
    kind: str  # positive | negative
    ok: bool
    status_code: int
    elapsed_ms: float
    details: str
    body_snippet: str


class Runner:
    def __init__(self) -> None:
        self.results: List[CaseResult] = []
        self.case_id = 0
        self.ctx: Dict[str, str] = {}

    def _next_id(self) -> int:
        self.case_id += 1
        return self.case_id

    def _record(self, result: CaseResult) -> None:
        self.results.append(result)
        tag = "PASS" if result.ok else "FAIL"
        print(
            f"{tag} T{result.case_id:02d} [{result.status_code}] ({result.kind}) "
            f"{result.name} ({result.elapsed_ms:.1f} ms)",
            flush=True,
        )
        if not result.ok:
            print(f"     ↳ {result.details}", flush=True)
            if result.body_snippet:
                print(f"     ↳ body: {result.body_snippet}", flush=True)

    def run_positive(
        self,
        name: str,
        call: Callable[[], requests.Response],
        check: Optional[Callable[[requests.Response], Tuple[bool, str]]] = None,
        expected_codes: Optional[Set[int]] = None,
    ) -> None:
        cid = self._next_id()
        t0 = time.perf_counter()
        code = -1
        body_snippet = ""
        ok = False
        details = ""
        try:
            resp = call()
            elapsed_ms = (time.perf_counter() - t0) * 1000.0
            code = resp.status_code
            body_snippet = (resp.text or "").replace("\n", " ")[:260]
            allowed = expected_codes or {200}
            status_ok = code in allowed
            check_ok, check_msg = (True, "ok") if check is None else check(resp)
            ok = status_ok and check_ok
            if not status_ok:
                details = f"expected {sorted(allowed)}, got {code}"
            elif not check_ok:
                details = check_msg
            else:
                details = "ok"
        except Exception as exc:  # noqa: BLE001
            elapsed_ms = (time.perf_counter() - t0) * 1000.0
            details = f"exception: {type(exc).__name__}: {exc}"

        self._record(
            CaseResult(
                case_id=cid,
                name=name,
                kind="positive",
                ok=ok,
                status_code=code,
                elapsed_ms=round(elapsed_ms, 3),
                details=details,
                body_snippet=body_snippet,
            )
        )

    def run_negative(
        self,
        name: str,
        call: Callable[[], requests.Response],
        expected_codes: Optional[Set[int]] = None,
    ) -> None:
        cid = self._next_id()
        t0 = time.perf_counter()
        code = -1
        body_snippet = ""
        ok = False
        details = ""
        try:
            resp = call()
            elapsed_ms = (time.perf_counter() - t0) * 1000.0
            code = resp.status_code
            body_snippet = (resp.text or "").replace("\n", " ")[:260]
            allowed = expected_codes or NEGATIVE_CODES
            ok = code in allowed
            if not ok:
                details = f"expected one of {sorted(allowed)}, got {code}"
            else:
                details = "ok"
        except Exception as exc:  # noqa: BLE001
            elapsed_ms = (time.perf_counter() - t0) * 1000.0
            details = f"exception: {type(exc).__name__}: {exc}"

        self._record(
            CaseResult(
                case_id=cid,
                name=name,
                kind="negative",
                ok=ok,
                status_code=code,
                elapsed_ms=round(elapsed_ms, 3),
                details=details,
                body_snippet=body_snippet,
            )
        )


def make_test_image(seed: int = 7) -> bytes:
    img = Image.new("RGB", (128, 96), (seed * 10 % 255, 40, 180))
    buf = BytesIO()
    img.save(buf, format="JPEG", quality=92)
    return buf.getvalue()


def has_status_field(resp: requests.Response) -> Tuple[bool, str]:
    try:
        body = resp.json()
    except Exception:  # noqa: BLE001
        return False, "response is not JSON"
    return ("status" in body, "missing 'status' key" if "status" not in body else "ok")


def has_services_list(resp: requests.Response) -> Tuple[bool, str]:
    try:
        body = resp.json()
    except Exception:  # noqa: BLE001
        return False, "response is not JSON"
    if not isinstance(body, dict):
        return False, "response is not JSON object"
    services = body.get("services")
    if not isinstance(services, list) or len(services) == 0:
        return False, "missing services[]"
    return True, "ok"


def has_models_data(resp: requests.Response) -> Tuple[bool, str]:
    try:
        body = resp.json()
    except Exception:  # noqa: BLE001
        return False, "response is not JSON"
    data = body.get("data") if isinstance(body, dict) else None
    return (isinstance(data, list) and len(data) > 0, "missing non-empty data[]")


def has_choices(resp: requests.Response) -> Tuple[bool, str]:
    try:
        body = resp.json()
    except Exception:  # noqa: BLE001
        return False, "response is not JSON"
    choices = body.get("choices") if isinstance(body, dict) else None
    if not isinstance(choices, list) or not choices:
        return False, "missing choices[]"
    msg = choices[0].get("message", {}) if isinstance(choices[0], dict) else {}
    content = msg.get("content", "") if isinstance(msg, dict) else ""
    return (isinstance(content, str) and bool(content.strip()), "empty assistant content")


def has_html_content_type(resp: requests.Response) -> Tuple[bool, str]:
    ct = str(resp.headers.get("content-type", "")).lower()
    return ("text/html" in ct, "content-type is not text/html")


def main() -> int:
    WORKDIR.mkdir(parents=True, exist_ok=True)
    runner = Runner()

    i2t = I2T_BASE_URL
    orch = ORCH_BASE_URL
    img = make_test_image(7)

    # -----------------------------------------------------------------
    # Positive cases (14)
    # -----------------------------------------------------------------
    runner.run_positive("I2T GET /health", lambda: requests.get(f"{i2t}/health", timeout=TIMEOUT), has_status_field)
    runner.run_positive("I2T GET /v1/models", lambda: requests.get(f"{i2t}/v1/models", timeout=TIMEOUT), has_models_data)
    runner.run_positive("I2T POST /v1/session/reset", lambda: requests.post(f"{i2t}/v1/session/reset", json={}, timeout=TIMEOUT), expected_codes={200, 400})
    runner.run_positive("Orch GET /api/status", lambda: requests.get(f"{orch}/api/status", timeout=TIMEOUT), has_services_list)
    runner.run_positive("Orch GET /api/i2t/models", lambda: requests.get(f"{orch}/api/i2t/models", timeout=TIMEOUT), has_models_data)

    def preprocess_and_store() -> requests.Response:
        resp = requests.post(
            f"{orch}/api/i2t/preprocess",
            files={"file": ("matrix.jpg", img, "image/jpeg")},
            data={"prompt": "Describe image"},
            timeout=TIMEOUT,
        )
        if resp.status_code == 200:
            try:
                body = resp.json()
                p = str(body.get("pixel_values_path", "")).strip()
                if p:
                    runner.ctx["pixel_values_path"] = p
            except Exception:  # noqa: BLE001
                pass
        return resp

    def preprocess_check(resp: requests.Response) -> Tuple[bool, str]:
        try:
            body = resp.json()
        except Exception:  # noqa: BLE001
            return False, "response is not JSON"
        p = str(body.get("pixel_values_path", "")).strip() if isinstance(body, dict) else ""
        return (bool(p), "missing pixel_values_path")

    runner.run_positive("Orch POST /api/i2t/preprocess valid", preprocess_and_store, preprocess_check)

    def i2t_vision_valid() -> requests.Response:
        p = runner.ctx.get("pixel_values_path", "inputs/pixel_values.raw")
        payload = {
            "messages": [{"role": "user", "content": "Describe this image in one concise sentence."}],
            "pixel_values_path": p,
            "stream": False,
            "max_tokens": 48,
        }
        return requests.post(f"{i2t}/v1/vision/chat/completions", json=payload, timeout=TIMEOUT)

    runner.run_positive("I2T POST /v1/vision/chat/completions valid", i2t_vision_valid, has_choices)

    runner.run_positive(
        "I2T POST /v1/chat/completions valid",
        lambda: requests.post(
            f"{i2t}/v1/chat/completions",
            json={"messages": [{"role": "user", "content": "Reply with one short word: hello"}], "stream": False, "max_tokens": 24},
            timeout=TIMEOUT,
        ),
        has_choices,
    )

    def orch_vision_valid() -> requests.Response:
        p = runner.ctx.get("pixel_values_path", "")
        payload = {
            "messages": [{"role": "user", "content": "Describe this image in one short sentence."}],
            "pixel_values_path": p,
            "stream": False,
            "max_tokens": 48,
        }
        return requests.post(f"{orch}/api/i2t/vision", json=payload, timeout=TIMEOUT)

    runner.run_positive("Orch POST /api/i2t/vision valid", orch_vision_valid, has_choices)

    runner.run_positive(
        "Orch POST /api/i2t/chat valid (__default__ session)",
        lambda: requests.post(
            f"{orch}/api/i2t/chat",
            json={
                "session_id": "__default__",
                "messages": [{"role": "user", "content": "Continue in one short sentence."}],
                "stream": False,
                "max_tokens": 32,
            },
            timeout=TIMEOUT,
        ),
        has_choices,
    )

    runner.run_positive(
        "Orch POST /api/i2t/reset",
        lambda: requests.post(f"{orch}/api/i2t/reset", json={"session_id": "__default__"}, timeout=TIMEOUT),
        expected_codes={200},
    )
    runner.run_positive("Orch GET /v1/models", lambda: requests.get(f"{orch}/v1/models", timeout=TIMEOUT), has_models_data)
    runner.run_positive(
        "Orch POST /v1/chat/completions text valid",
        lambda: requests.post(
            f"{orch}/v1/chat/completions",
            json={"messages": [{"role": "user", "content": "Answer with one word: ping"}], "stream": False, "max_tokens": 24},
            timeout=TIMEOUT,
        ),
        has_choices,
    )
    runner.run_positive("Orch GET / (HTML)", lambda: requests.get(f"{orch}/", timeout=TIMEOUT), has_html_content_type)

    # -----------------------------------------------------------------
    # Negative cases (54)
    # -----------------------------------------------------------------
    # Direct I2T preprocess negatives (4)
    runner.run_negative("I2T GET /v1/preprocess wrong method", lambda: requests.get(f"{i2t}/v1/preprocess", timeout=TIMEOUT))
    runner.run_negative("I2T POST /v1/preprocess no file", lambda: requests.post(f"{i2t}/v1/preprocess", files={}, timeout=TIMEOUT))
    runner.run_negative(
        "I2T POST /v1/preprocess empty file",
        lambda: requests.post(f"{i2t}/v1/preprocess", files={"file": ("e.jpg", b"", "image/jpeg")}, timeout=TIMEOUT),
    )
    runner.run_negative(
        "I2T POST /v1/preprocess invalid mime",
        lambda: requests.post(f"{i2t}/v1/preprocess", files={"file": ("x.txt", b"abc", "text/plain")}, timeout=TIMEOUT),
    )

    # Direct I2T chat negatives (16)
    bad_chat_payloads: List[Tuple[str, Any, bool]] = [
        ("chat invalid JSON", "{bad-json", True),
        ("chat missing messages", {"model": "local"}, False),
        ("chat messages string", {"messages": "hello"}, False),
        ("chat messages number", {"messages": 123}, False),
        ("chat messages empty list", {"messages": []}, False),
        ("chat no user message", {"messages": [{"role": "assistant", "content": "x"}]}, False),
        ("chat user missing content", {"messages": [{"role": "user"}]}, False),
        ("chat user content int", {"messages": [{"role": "user", "content": 9}]}, False),
        ("chat user content list", {"messages": [{"role": "user", "content": [1, 2]}]}, False),
        ("chat user empty content", {"messages": [{"role": "user", "content": ""}]}, False),
        ("chat invalid pixel path", {"messages": [{"role": "user", "content": "describe"}], "pixel_values_path": "/tmp/notfound.raw"}, False),
        ("chat null payload field", {"messages": None}, False),
        ("chat messages object", {"messages": {"role": "user", "content": "x"}}, False),
        ("chat malformed message object", {"messages": [{"foo": "bar"}]}, False),
        ("chat content null", {"messages": [{"role": "user", "content": None}]}, False),
        ("chat role null", {"messages": [{"role": None, "content": "x"}]}, False),
    ]

    for name, payload, is_raw in bad_chat_payloads:
        if is_raw:
            runner.run_negative(
                f"I2T POST /v1/chat/completions {name}",
                lambda p=payload: requests.post(
                    f"{i2t}/v1/chat/completions",
                    data=str(p).encode("utf-8"),
                    headers={"Content-Type": "application/json"},
                    timeout=TIMEOUT,
                ),
            )
        else:
            runner.run_negative(
                f"I2T POST /v1/chat/completions {name}",
                lambda p=payload: requests.post(f"{i2t}/v1/chat/completions", json=p, timeout=TIMEOUT),
            )

    # Direct I2T vision negatives (14)
    bad_vision_payloads: List[Tuple[str, Any, bool]] = [
        ("vision invalid JSON", "{bad-json", True),
        ("vision missing all", {}, False),
        ("vision missing pixel", {"messages": [{"role": "user", "content": "x"}]}, False),
        ("vision missing messages", {"pixel_values_path": "inputs/pixel_values.raw"}, False),
        ("vision messages string", {"messages": "x", "pixel_values_path": "inputs/pixel_values.raw"}, False),
        ("vision messages empty", {"messages": [], "pixel_values_path": "inputs/pixel_values.raw"}, False),
        ("vision no user role", {"messages": [{"role": "assistant", "content": "x"}], "pixel_values_path": "inputs/pixel_values.raw"}, False),
        ("vision bad pixel path", {"messages": [{"role": "user", "content": "x"}], "pixel_values_path": "/tmp/notfound.raw"}, False),
        ("vision user missing content", {"messages": [{"role": "user"}], "pixel_values_path": "inputs/pixel_values.raw"}, False),
        ("vision user content int", {"messages": [{"role": "user", "content": 1}], "pixel_values_path": "inputs/pixel_values.raw"}, False),
        ("vision content null", {"messages": [{"role": "user", "content": None}], "pixel_values_path": "inputs/pixel_values.raw"}, False),
        ("vision pixel null", {"messages": [{"role": "user", "content": "x"}], "pixel_values_path": None}, False),
        ("vision pixel int", {"messages": [{"role": "user", "content": "x"}], "pixel_values_path": 123}, False),
        ("vision role null", {"messages": [{"role": None, "content": "x"}], "pixel_values_path": "inputs/pixel_values.raw"}, False),
    ]

    for name, payload, is_raw in bad_vision_payloads:
        if is_raw:
            runner.run_negative(
                f"I2T POST /v1/vision/chat/completions {name}",
                lambda p=payload: requests.post(
                    f"{i2t}/v1/vision/chat/completions",
                    data=str(p).encode("utf-8"),
                    headers={"Content-Type": "application/json"},
                    timeout=TIMEOUT,
                ),
            )
        else:
            runner.run_negative(
                f"I2T POST /v1/vision/chat/completions {name}",
                lambda p=payload: requests.post(f"{i2t}/v1/vision/chat/completions", json=p, timeout=TIMEOUT),
            )

    # Orchestrator I2T API negatives (12)
    bad_orch_i2t: List[Tuple[str, str, Any, bool]] = [
        ("/api/i2t/vision", "invalid JSON", "{bad-json", True),
        ("/api/i2t/vision", "missing messages", {"pixel_values_path": runner.ctx.get("pixel_values_path", "")}, False),
        ("/api/i2t/vision", "missing pixel", {"messages": [{"role": "user", "content": "x"}]}, False),
        ("/api/i2t/vision", "bad pixel", {"messages": [{"role": "user", "content": "x"}], "pixel_values_path": "/tmp/no.raw", "stream": False}, False),
        ("/api/i2t/chat", "invalid JSON", "{bad-json", True),
        ("/api/i2t/chat", "missing session_id", {"messages": [{"role": "user", "content": "x"}]}, False),
        ("/api/i2t/chat", "missing messages", {"session_id": "__default__"}, False),
        ("/api/i2t/chat", "messages wrong type", {"session_id": "__default__", "messages": "x"}, False),
        ("/api/i2t/chat", "user missing content", {"session_id": "__default__", "messages": [{"role": "user"}]}, False),
        ("/api/i2t/chat", "content int", {"session_id": "__default__", "messages": [{"role": "user", "content": 1}]}, False),
        ("/api/i2t/reset", "invalid JSON (accepted as empty payload)", "{bad-json", True),
        ("/api/i2t/reset", "bad type (accepted as empty session_id)", {"session_id": 12345}, False),
    ]

    for path, name, payload, is_raw in bad_orch_i2t:
        url = f"{orch}{path}"
        if is_raw:
            custom_expected = {200} if path == "/api/i2t/reset" else None
            runner.run_negative(
                f"Orch POST {path} {name}",
                lambda u=url, p=payload: requests.post(
                    u,
                    data=str(p).encode("utf-8"),
                    headers={"Content-Type": "application/json"},
                    timeout=TIMEOUT,
                ),
                expected_codes=custom_expected,
            )
        else:
            custom_expected = {200} if path == "/api/i2t/reset" else None
            runner.run_negative(
                f"Orch POST {path} {name}",
                lambda u=url, p=payload: requests.post(u, json=p, timeout=TIMEOUT),
                expected_codes=custom_expected,
            )

    # Orchestrator OpenAI/proxy negatives (8)
    runner.run_negative(
        "Orch POST /v1/chat/completions invalid JSON",
        lambda: requests.post(
            f"{orch}/v1/chat/completions",
            data=b"{bad-json",
            headers={"Content-Type": "application/json"},
            timeout=TIMEOUT,
        ),
    )
    runner.run_negative(
        "Orch POST /v1/chat/completions missing messages",
        lambda: requests.post(f"{orch}/v1/chat/completions", json={"model": "local"}, timeout=TIMEOUT),
    )
    runner.run_negative(
        "Orch POST /v1/chat/completions messages wrong type",
        lambda: requests.post(f"{orch}/v1/chat/completions", json={"messages": "x"}, timeout=TIMEOUT),
    )
    runner.run_negative(
        "Orch POST /v1/audio/transcriptions missing file",
        lambda: requests.post(f"{orch}/v1/audio/transcriptions", files={}, timeout=TIMEOUT),
    )
    runner.run_negative(
        "Orch POST /v1/realtime/sessions unsupported model",
        lambda: requests.post(f"{orch}/v1/realtime/sessions", json={"model": "bad-model"}, timeout=TIMEOUT),
    )
    runner.run_negative(
        "Orch POST /v1/images/generations missing prompt",
        lambda: requests.post(f"{orch}/v1/images/generations", json={"model": "stable-diffusion-2-1"}, timeout=TIMEOUT),
    )
    runner.run_negative(
        "Orch POST /v1/images/edits missing image",
        lambda: requests.post(f"{orch}/v1/images/edits", files={}, data={"prompt": "x"}, timeout=TIMEOUT),
    )
    runner.run_negative(
        "Orch GET /v1/models/nonexistent",
        lambda: requests.get(f"{orch}/v1/models/nonexistent-model-id", timeout=TIMEOUT),
        expected_codes={404},
    )

    # -----------------------------------------------------------------
    total = len(runner.results)
    passed = sum(1 for r in runner.results if r.ok)
    failed = total - passed
    pos_total = sum(1 for r in runner.results if r.kind == "positive")
    pos_pass = sum(1 for r in runner.results if r.kind == "positive" and r.ok)
    neg_total = sum(1 for r in runner.results if r.kind == "negative")
    neg_pass = sum(1 for r in runner.results if r.kind == "negative" and r.ok)

    report = {
        "total": total,
        "passed": passed,
        "failed": failed,
        "positive_total": pos_total,
        "positive_passed": pos_pass,
        "negative_total": neg_total,
        "negative_passed": neg_pass,
        "i2t_base_url": I2T_BASE_URL,
        "orch_base_url": ORCH_BASE_URL,
        "pixel_values_path": runner.ctx.get("pixel_values_path", ""),
        "results": [r.__dict__ for r in runner.results],
    }

    ts = int(time.time())
    out = WORKDIR / f"i2t_orchestrator_additional_68_report_{ts}.json"
    out.write_text(json.dumps(report, indent=2), encoding="utf-8")

    print("\n" + "=" * 60, flush=True)
    print(
        f"Additional suite: {passed}/{total} passed, {failed} failed "
        f"(positive {pos_pass}/{pos_total}, negative {neg_pass}/{neg_total})",
        flush=True,
    )
    print(f"Report: {out}", flush=True)
    print("=" * 60, flush=True)

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
