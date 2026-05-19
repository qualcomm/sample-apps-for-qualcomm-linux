# ---------------------------------------------------------------------
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------

import streamlit as st
import requests
import json
import time
import hashlib
import os
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional

# ---------------------------
# Model Discovery & Prompt Formats
# ---------------------------

BUNDLE_DIR = "/opt/genie_bundle"

PROMPT_FORMATS = {
    "qwen": {
        "begin_system":    "<|im_start|>system\n",
        "end_system":      "\n<|im_end|>\n",
        "begin_user":      "<|im_start|>user\n",
        "end_user":        "\n<|im_end|>\n",
        "begin_assistant": "<|im_start|>assistant\n",
        "end_assistant":   "\n<|im_end|>\n",
    },
    "llama": {
        "begin_system":    "<|begin_of_text|><|start_header_id|>system<|end_header_id|>\n\n",
        "end_system":      "<|eot_id|>\n\n",
        "begin_user":      "<|start_header_id|>user<|end_header_id|>\n\n",
        "end_user":        "<|eot_id|>\n\n",
        "begin_assistant": "<|start_header_id|>assistant<|end_header_id|>\n\n",
        "end_assistant":   "<|eot_id|>\n\n",
    },
    # Add more families here (e.g. mistral, phi, gemma...)
}

DEFAULT_SYSTEM_PROMPTS = {
    "qwen":  "You are a helpful assistant.",
    "llama": "You are a helpful assistant.",
}

def _deep_find_first_str(obj: Any, keys: set[str]) -> Optional[str]:
    """Best-effort recursive lookup for a string value under any matching key."""
    if isinstance(obj, dict):
        for k, v in obj.items():
            if isinstance(k, str) and k.lower() in keys and isinstance(v, str) and v.strip():
                return v.strip()
            found = _deep_find_first_str(v, keys)
            if found:
                return found
    elif isinstance(obj, list):
        for it in obj:
            found = _deep_find_first_str(it, keys)
            if found:
                return found
    return None

def _infer_family_from_strings(values: list[str]) -> Optional[str]:
    haystack = " ".join(v.lower() for v in values if v)
    for family in PROMPT_FORMATS.keys():
        if family in haystack:
            return family
    # common aliases / patterns
    if "llama" in haystack:
        return "llama"
    if "qwen" in haystack:
        return "qwen"
    return None

def get_model_family_from_genie_config(config_path: str, bundle_name_fallback: str = "") -> str:
    """
    Determine model family from genie_config.json.

    Strategy (in order):
      1) Look for explicit fields anywhere in JSON (family/model_family/model-family).
      2) Infer from model artifact names (e.g., ctx-bins filenames).
      3) Fallback to bundle folder name heuristic.
      4) Final fallback: 'llama'
    """
    try:
        with open(config_path, "r", encoding="utf-8") as f:
            cfg = json.load(f)

        explicit = _deep_find_first_str(cfg, {"family", "model_family", "model-family"})
        if explicit:
            inferred = _infer_family_from_strings([explicit])
            if inferred:
                return inferred

        # Try to infer from common fields used by Genie config
        strings_to_scan: list[str] = []

        # tokenizer path
        tok_path = _deep_find_first_str(cfg, {"path", "tokenizer_path", "tokenizer-path"})
        if tok_path:
            strings_to_scan.append(tok_path)

        # ctx-bins list (common for binary model)
        try:
            ctx_bins = (
                cfg.get("dialog", {})
                   .get("engine", {})
                   .get("model", {})
                   .get("binary", {})
                   .get("ctx-bins", [])
            )
            if isinstance(ctx_bins, list):
                strings_to_scan.extend([str(x) for x in ctx_bins])
        except Exception:
            pass

        inferred = _infer_family_from_strings(strings_to_scan)
        if inferred:
            return inferred
    except Exception:
        # ignore parse/read errors; fall back to heuristic
        pass

    # Fallback heuristic: bundle name
    name = (bundle_name_fallback or "").lower()
    for family in PROMPT_FORMATS:
        if family in name:
            return family

    return "llama"

def discover_models(bundle_dir: str = BUNDLE_DIR) -> list[dict]:
    """
    Scan bundle_dir for subfolders containing genie_config.json.
    Auto-detect model family and assign prompt format.
    """
    models = []
    if not os.path.isdir(bundle_dir):
        return models
    for entry in sorted(os.listdir(bundle_dir)):
        full_path = os.path.join(bundle_dir, entry)
        config_path = os.path.join(full_path, "genie_config.json")
        if os.path.isdir(full_path) and os.path.isfile(config_path):
            family = get_model_family_from_genie_config(config_path, bundle_name_fallback=entry)
            models.append({
                "name":          entry,
                "bundle":        entry,
                "config_path":   config_path,
                "base_dir":      full_path,
                "family":        family,
                "prompt_format": PROMPT_FORMATS[family],
                "system_prompt": DEFAULT_SYSTEM_PROMPTS.get(family, "You are a helpful assistant."),
            })
    return models

# ---------------------------
# Logger
# ---------------------------

@dataclass
class LogEntry:
    timestamp: str
    level: str
    message: str
    elapsed_ms: Optional[float] = None
    extra: Optional[Dict[str, Any]] = None

class SimpleLogger:
    def __init__(self):
        self.logs: List[LogEntry] = []

    def log(self, message: str, level: str = "INFO", extra: Dict[str, Any] | None = None, elapsed_ms: float | None = None):
        ts = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())
        self.logs.append(LogEntry(timestamp=ts, level=level.upper(), message=message, elapsed_ms=elapsed_ms, extra=extra or {}))

    def time_block(self, message: str, level: str = "INFO", extra_start: Dict[str, Any] | None = None, extra_end: Dict[str, Any] | None = None):
        class _TimerCtx:
            def __init__(self, outer, msg, lvl, xstart, xend):
                self.outer = outer
                self.msg = msg
                self.level = lvl
                self.extra_start = xstart or {}
                self.extra_end = xend or {}
            def __enter__(self):
                self.start = time.perf_counter()
                self.outer.log(f"{self.msg} - start", level=self.level, extra=self.extra_start)
                return self
            def __exit__(self, exc_type, exc, tb):
                elapsed_ms = (time.perf_counter() - self.start) * 1000.0
                status = "SUCCESS" if exc is None else "ERROR"
                self.outer.log(f"{self.msg} - end ({status})", level=self.level, extra=self.extra_end, elapsed_ms=elapsed_ms)
                return False
        return _TimerCtx(self, message, level, extra_start, extra_end)

    def get_logs(self) -> List[Dict[str, Any]]:
        from dataclasses import asdict
        return [asdict(e) for e in self.logs]

    def clear(self):
        self.logs.clear()

    def export_as_json(self) -> str:
        return json.dumps(self.get_logs(), indent=2)

# ---------------------------
# Page Config
# ---------------------------

st.set_page_config(page_title="GenAI Studio", layout="wide")
PAGE_KEY = "text2text"

if "logger" not in st.session_state:
    st.session_state.logger = SimpleLogger()
logger = st.session_state.logger

@dataclass
class GenerationSettings:
    system_prompt: str = "You are a helpful assistant."
    seed: int = 42
    temp: float = 0.8
    top_k: int = 40
    top_p: float = 0.95
    max_tokens: int = 200
    greedy: bool = False
    model_bundle: str = ""
    prompt_format: Dict[str, str] = field(default_factory=dict)

# ---------------------------
# Server URLs
# ---------------------------


SERVER_PROCESS_URL = 'http://0.0.0.0:8088/process'
SERVER_RESET_URL   = 'http://0.0.0.0:8088/reset_model'
SERVER_RELOAD_URL  = 'http://0.0.0.0:8088/reload_model'  # reload + (optionally) switch model

# ---------------------------
# Helper Functions
# ---------------------------

def call_backend(url: str, payload: dict | None = None) -> tuple[bool, str]:
    try:
        if payload is None:
            response = requests.post(url, timeout=30)
        else:
            response = requests.post(
                url,
                data=json.dumps(payload),
                headers={"Content-Type": "application/json"},
                timeout=30,
            )
        if response.status_code == 200:
            try:
                data = response.json()
                msg = data.get("message") or data.get("status") or "Success."
            except Exception:
                msg = response.text or "Success."
            return True, msg
        else:
            return False, f"Error: {response.status_code} {response.text}"
    except Exception as e:
        return False, f"Request failed: {e}"

def notify_success(msg: str):
    # Avoid returning/printing DeltaGenerator objects in logs/output.
    if hasattr(st, "toast"):
        st.toast(msg, icon="✅")
    else:
        st.success(msg)

def notify_error(msg: str):
    # Avoid returning/printing DeltaGenerator objects in logs/output.
    if hasattr(st, "toast"):
        st.toast(msg, icon="❌")
    else:
        st.error(msg)

def reset_conversation():
    st.session_state[f"{PAGE_KEY}_messages"] = [{
        "role": "assistant",
        "content": "Hey there! 😄 How can I assist you today?"
    }]
    # Streamlit automatically reruns after widget callbacks; calling st.rerun()
    # inside a callback is a no-op and emits a warning. So don't call it here.

def _hash_payload(payload: dict) -> str:
    return hashlib.sha256(json.dumps(payload, sort_keys=True).encode("utf-8")).hexdigest()

# ---------------------------
# Backend Action Functions
# ---------------------------

def switch_model():
    """Switch to a new model bundle via /reload_model (unified endpoint)."""
    settings = st.session_state.gen_settings
    bundle = getattr(settings, "model_bundle", None)
    if not bundle:
        notify_error("No model selected.")
        return

    with logger.time_block("Model switch", level="INFO",
                           extra_start={"route": "/reload_model", "bundle": bundle},
                           extra_end={"route": "/reload_model"}):
        t0 = time.perf_counter()

        # Only switching model is required here.
        # (User can hit Reload after switching to apply settings if desired.)
        success, msg = call_backend(SERVER_RELOAD_URL, {
            "model_bundle": bundle,
            "system_prompt": settings.system_prompt,
            "max_tokens": settings.max_tokens,
        })
        elapsed_ms = (time.perf_counter() - t0) * 1000.0

        logger.log("Backend switch call outcome", level="INFO", elapsed_ms=elapsed_ms,
                   extra={"success": success, "message": msg, "bundle": bundle})

        if success:
            # Invalidate reload hash so next reload goes through
            st.session_state.pop("last_reload_hash", None)
            notify_success(f"Switched to: {bundle}")
            reset_conversation()
        else:
            notify_error(f"Switch failed: {msg}")

def reload_model():
    """Reload current model with updated generation settings."""
    settings = st.session_state.gen_settings

    sampler_block = json.dumps({
        "sampler": {
            "version": 1,
            "type":    "basic",
            "seed":    settings.seed,
            "temp":    settings.temp,
            "top-k":   settings.top_k,
            "top-p":   settings.top_p,
            "greedy":  settings.greedy,
        }
    }, indent=2)

    payload = {
        # Backend stores system_prompt (even though UI formats prompts).
        "system_prompt": settings.system_prompt,
        "max_tokens":    settings.max_tokens,
        "sampler_block": sampler_block,
        # Keep explicit model here so reload applies to the selected bundle.
        "model_bundle":  getattr(settings, "model_bundle", ""),
    }

    # Skip if settings unchanged
    try:
        new_hash = _hash_payload(payload)
        if st.session_state.get("last_reload_hash") == new_hash:
            logger.log("Reload skipped: settings unchanged", level="INFO", extra={"route": "/reload_model"})
            notify_success("Reload skipped — settings unchanged.")
            return
        st.session_state["last_reload_hash"] = new_hash
    except Exception as e:
        logger.log("Reload hash check failed", level="WARN", extra={"error": str(e)})

    with logger.time_block("Model reload", level="INFO",
                           extra_start={"route": "/reload_model",
                                        "system_prompt_len": len(settings.system_prompt),
                                        "sampler_block_len": len(sampler_block)},
                           extra_end={"route": "/reload_model"}):
        t0 = time.perf_counter()
        success, msg = call_backend(SERVER_RELOAD_URL, payload)
        elapsed_ms = (time.perf_counter() - t0) * 1000.0

        logger.log("Backend reload call outcome", level="INFO", elapsed_ms=elapsed_ms,
                   extra={
                       "success": success, "message": msg,
                       "payload_preview": {
                           "system_prompt": (settings.system_prompt[:256] + "…") if len(settings.system_prompt) > 256 else settings.system_prompt,
                           "seed": settings.seed, "temp": settings.temp,
                           "top_k": settings.top_k, "top_p": settings.top_p,
                           "greedy": settings.greedy, "max_tokens": settings.max_tokens,
                       }
                   })

        if success:
            notify_success(f"Model reload successful: {msg}")
            reset_conversation()
        else:
            notify_error(f"Model reload failed: {msg}")

def reset_model():
    """Reset model conversation state on backend."""
    with logger.time_block("Model reset", level="INFO",
                           extra_start={"route": "/reset_model"},
                           extra_end={"route": "/reset_model"}):
        t0 = time.perf_counter()
        success, msg = call_backend(SERVER_RESET_URL)
        elapsed_ms = (time.perf_counter() - t0) * 1000.0

        logger.log("Backend reset call outcome", level="INFO", elapsed_ms=elapsed_ms,
                   extra={"success": success, "message": msg})

        if success:
            notify_success(f"Model reset successful: {msg}")
            reset_conversation()
        else:
            notify_error(f"Model reset failed: {msg}")

# ---------------------------
# Initialize Settings
# ---------------------------

if "gen_settings" not in st.session_state:
    st.session_state.gen_settings = GenerationSettings()

# ---------------------------
# ✅ SINGLE Sidebar Block
# ---------------------------

with st.sidebar:
    st.title("GenAI Studio")

    # --- Model Selector ---
    st.subheader("Model")
    models = discover_models()

    def _on_model_change():
        """Auto-switch backend model when dropdown selection changes (no button)."""
        if not models:
            return

        selected_name = st.session_state.get("model_select")
        if not selected_name:
            return

        selected_model = next((m for m in models if m["name"] == selected_name), None)
        if not selected_model:
            return

        # Update UI-side settings immediately (prompt format + default system prompt)
        st.session_state.gen_settings.system_prompt = selected_model["system_prompt"]
        st.session_state.gen_settings.model_bundle = selected_model["bundle"]
        st.session_state.gen_settings.prompt_format = selected_model["prompt_format"]
        st.session_state["last_selected_model"] = selected_name

        # Switch backend model immediately
        bundle = selected_model["bundle"]
        with logger.time_block(
            "Model switch (auto)",
            level="INFO",
            extra_start={"route": "/reload_model", "bundle": bundle},
            extra_end={"route": "/reload_model"},
        ):
            t0 = time.perf_counter()
            # Also apply current generation settings on switch (keeps backend in sync).
            sampler_block = json.dumps({
                "sampler": {
                    "version": 1,
                    "type":    "basic",
                    "seed":    st.session_state.gen_settings.seed,
                    "temp":    st.session_state.gen_settings.temp,
                    "top-k":   st.session_state.gen_settings.top_k,
                    "top-p":   st.session_state.gen_settings.top_p,
                    "greedy":  st.session_state.gen_settings.greedy,
                }
            }, indent=2)

            success, msg = call_backend(SERVER_RELOAD_URL, {
                "model_bundle": bundle,
                "system_prompt": st.session_state.gen_settings.system_prompt,
                "max_tokens": st.session_state.gen_settings.max_tokens,
                "sampler_block": sampler_block,
            })
            elapsed_ms = (time.perf_counter() - t0) * 1000.0

            logger.log(
                "Backend auto-switch call outcome",
                level="INFO",
                elapsed_ms=elapsed_ms,
                extra={"success": success, "message": msg, "bundle": bundle},
            )

            if success:
                st.session_state.pop("last_reload_hash", None)
                notify_success(f"Switched to: {bundle}")
                reset_conversation()
            else:
                notify_error(f"Switch failed: {msg}")

    if models:
        model_names = [m["name"] for m in models]

        # Set a stable default selection
        if "model_select" not in st.session_state:
            st.session_state["model_select"] = model_names[0]

        selected_name = st.selectbox(
            "Select Model",
            model_names,
            key="model_select",
            on_change=_on_model_change,
        )
        selected_model = next(m for m in models if m["name"] == selected_name)

        st.caption(f"Family: `{selected_model['family']}`")

        # First-load initialization (don’t call backend here; only set initial state)
        if st.session_state.get("last_selected_model") is None:
            st.session_state.gen_settings.system_prompt = selected_model["system_prompt"]
            st.session_state.gen_settings.model_bundle = selected_model["bundle"]
            st.session_state.gen_settings.prompt_format = selected_model["prompt_format"]
            st.session_state["last_selected_model"] = selected_name
    else:
        st.warning("⚠️ No models found in /opt/genie_bundle/")

    st.divider()

    # --- Generation Settings ---
    st.subheader("Generation Settings")

    # System prompt — auto-filled from model family, still editable
    st.session_state.gen_settings.system_prompt = st.text_area(
        "System Prompt", st.session_state.gen_settings.system_prompt
    )
    st.session_state.gen_settings.max_tokens = st.number_input(
        "Max Tokens", value=st.session_state.gen_settings.max_tokens, step=10
    )
    st.session_state.gen_settings.seed = st.number_input(
        "Seed", value=st.session_state.gen_settings.seed, step=1
    )
    st.session_state.gen_settings.temp = st.slider(
        "Temperature", 0.0, 1.0, st.session_state.gen_settings.temp
    )
    st.session_state.gen_settings.top_k = st.number_input(
        "Top-K", value=st.session_state.gen_settings.top_k, step=1
    )
    st.session_state.gen_settings.top_p = st.slider(
        "Top-P", 0.0, 1.0, st.session_state.gen_settings.top_p
    )
    st.session_state.gen_settings.greedy = st.checkbox(
        "Enable Greedy Decoding", value=st.session_state.gen_settings.greedy
    )

    st.divider()

    col1, col2 = st.columns(2)
    with col1:
        if st.button("♻️ Reload", use_container_width=True):
            reload_model()
    with col2:
        if st.button("🔄 Reset", use_container_width=True):
            reset_model()

# ---------------------------
# Client Logs
# ---------------------------

st.subheader("Client Logs")
logs = logger.get_logs()

with st.expander(f"Recent Logs (count={len(logs)})", expanded=False):
    for i, e in enumerate(logs[-50:], 1):
        st.markdown(f"**[{i}] {e['timestamp']} — {e['level']} — {e['message']}**")
        if e.get("elapsed_ms") is not None:
            st.write(f"Elapsed: {e['elapsed_ms']:.1f} ms")
        if e.get("extra"):
            st.code(json.dumps(e["extra"], indent=2))

if logs:
    st.download_button(
        "📥 Download Logs (JSON)",
        data=logger.export_as_json(),
        file_name="client_logs.json",
        mime="application/json",
        use_container_width=True
    )

st.markdown("---")

# ---------------------------
# Chat History
# ---------------------------

if f"{PAGE_KEY}_messages" not in st.session_state:
    st.session_state[f"{PAGE_KEY}_messages"] = [{
        "role": "assistant",
        "content": "Hey there! 😄 How can I assist you today?"
    }]

st.subheader("Chat History")
for message in st.session_state[f"{PAGE_KEY}_messages"]:
    with st.chat_message(message["role"]):
        st.write(message["content"])
    st.markdown("---")

# ---------------------------
# Chat Input & Backend Interaction
# ---------------------------

settings = st.session_state.gen_settings

if "req_counter" not in st.session_state:
    st.session_state.req_counter = 0

if prompt := st.chat_input("Type your message..."):
    st.session_state[f"{PAGE_KEY}_messages"].append({"role": "user", "content": prompt})

    with st.chat_message("user"):
        st.write(prompt)

    st.session_state.req_counter += 1
    req_id = f"process-{st.session_state.req_counter}"

    def _short(s: str, limit: int = 2048) -> str:
        return s if s is None or len(s) <= limit else (s[:limit] + "…")

    greedy_str = str(bool(getattr(settings, "greedy", False))).lower()
    sampler_block = json.dumps({
        "sampler": {
            "version": 1,
            "type":    "basic",
            "seed":    getattr(settings, "seed", 42),
            "temp":    getattr(settings, "temp", 0.8),
            "top-k":   getattr(settings, "top_k", 40),
            "top-p":   getattr(settings, "top_p", 0.95),
            "greedy":  getattr(settings, "greedy", False),
        }
    }, indent=2)

    with st.chat_message("assistant"):
        with st.spinner("Thinking..."):
            t0 = time.perf_counter()
            answer = None
            error = None
            http_status = None

            try:
                fmt = getattr(settings, "prompt_format", {}) or PROMPT_FORMATS["llama"]
                sys_prompt = getattr(settings, "system_prompt", "You are a helpful assistant.")
                formatted_prompt = (
                    fmt["begin_system"] + sys_prompt + fmt["end_system"] +
                    fmt["begin_user"] + prompt + fmt["end_user"] +
                    fmt["begin_assistant"]
                )

                resp = requests.post(
                    SERVER_PROCESS_URL,
                    data=json.dumps({
                        # Backend now treats prompt as final; PromptHandler is bypassed.
                        "prompt": formatted_prompt,
                        "prompt_is_formatted": True,
                        # Backend stores system_prompt for observability / external clients.
                        "system_prompt": getattr(settings, "system_prompt", ""),
                        # Ensure backend uses the same generation settings the UI shows.
                        "max_tokens": getattr(settings, "max_tokens", 200),
                        "temperature": getattr(settings, "temp", 0.8),
                        "top_k": getattr(settings, "top_k", 40),
                        "top_p": getattr(settings, "top_p", 0.95),
                        "seed": getattr(settings, "seed", 42),
                        # Also send the selected bundle so /process can switch models if needed.
                        "model_bundle": getattr(settings, "model_bundle", ""),
                    }),
                    headers={"Content-Type": "application/json"},
                    # LLM inference can exceed 60s; allow longer before showing an error.
                    timeout=300,
                )
                http_status = resp.status_code
                if resp.status_code == 200:
                    answer = resp.json().get("answer", "No answer from LLM")
                else:
                    error = f"Error: {resp.status_code} {resp.text}"
                    answer = error
            except Exception as e:
                error = f"Request failed: {e}"
                answer = error

            elapsed_ms = (time.perf_counter() - t0) * 1000.0

            st.write(answer)
            st.session_state[f"{PAGE_KEY}_messages"].append({
                "role": "assistant", "content": answer
            })

            logger.log(
                f"[{req_id}] /process",
                level="INFO",
                elapsed_ms=elapsed_ms,
                extra={
                    "status":       "success" if error is None else "failure",
                    "http_status":  http_status,
                    "user_prompt":  _short(prompt),
                    "answer":       _short(answer, 4096),
                    "system_prompt": _short(getattr(settings, "system_prompt", ""), 2048),
                    "sampler_block": _short(sampler_block, 4096),
                    "model_bundle":  getattr(settings, "model_bundle", ""),
                    "settings": {
                        "seed":       getattr(settings, "seed", 42),
                        "temp":       getattr(settings, "temp", 0.8),
                        "top_k":      getattr(settings, "top_k", 40),
                        "top_p":      getattr(settings, "top_p", 0.95),
                        "greedy":     getattr(settings, "greedy", False),
                        "max_tokens": getattr(settings, "max_tokens", 200),
                    }
                }
            )