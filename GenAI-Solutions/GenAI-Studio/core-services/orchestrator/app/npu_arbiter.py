#!/usr/bin/env python3
# ---------------------------------------------------------------------
# Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
# Shared NPU arbitration for orchestrator-routed heavyweight inference.
# ---------------------------------------------------------------------

import asyncio
import os
import uuid
from contextlib import asynccontextmanager
from typing import Any, Callable


def _env_bool(name: str, default: bool) -> bool:
    """Parse boolean env var with common truthy/falsey forms."""
    raw = os.getenv(name)
    if raw is None:
        return default
    return raw.strip().lower() in ("1", "true", "yes", "y", "on")


NPU_ARB_ENABLED = _env_bool("NPU_ARB_ENABLED", True)
NPU_ARB_LOG_WAITS = _env_bool("NPU_ARB_LOG_WAITS", False)
NPU_ARB_TIMEOUT_SEC = float(os.getenv("NPU_ARB_TIMEOUT_SEC", "180"))

_npu_lock = asyncio.Lock()
_lease_handles: dict[str, Callable[[], None]] = {}


class NpuBusyError(Exception):
    """Raised when NPU arbitration lock wait exceeds configured timeout."""

    def __init__(self, owner: str, timeout_sec: float) -> None:
        self.owner = owner
        self.timeout_sec = timeout_sec
        super().__init__(
            f"NPU is busy (owner={owner}); timed out waiting {timeout_sec:.1f}s for accelerator slot"
        )


def _make_lock_releaser() -> Callable[[], None]:
    """Return an idempotent releaser for the global NPU lock."""
    released = False

    def _release() -> None:
        nonlocal released
        if released:
            return
        released = True
        try:
            _npu_lock.release()
        except RuntimeError:
            # Best-effort release for cleanup paths.
            return

    return _release


async def acquire_npu_slot_handle(
    owner: str, timeout_sec: float | None = None
) -> tuple[dict[str, Any], Callable[[], None]]:
    """
    Acquire NPU lock and return (metadata, idempotent release callback).

    Useful for long-lived/streaming responses where the lock must be held
    until response close callback executes.
    """
    if not NPU_ARB_ENABLED:
        return {"enabled": False, "wait_ms": 0.0, "owner": owner}, (lambda: None)

    wait_timeout = timeout_sec if timeout_sec is not None else NPU_ARB_TIMEOUT_SEC
    loop = asyncio.get_event_loop()
    t0 = loop.time()

    try:
        await asyncio.wait_for(_npu_lock.acquire(), timeout=wait_timeout)
    except asyncio.TimeoutError as exc:
        raise NpuBusyError(owner=owner, timeout_sec=wait_timeout) from exc

    wait_ms = round((loop.time() - t0) * 1000.0, 3)
    if NPU_ARB_LOG_WAITS and wait_ms > 0.0:
        print(f"[npu-arbiter] owner={owner} wait_ms={wait_ms}")

    return {"enabled": True, "wait_ms": wait_ms, "owner": owner}, _make_lock_releaser()


async def acquire_npu_lease(
    owner: str, timeout_sec: float | None = None
) -> dict[str, Any]:
    """
    Acquire a lease-backed NPU lock handle for external services.

    External callers (for example direct service containers) can acquire
    and release the same process-wide lock through HTTP routes.
    """
    meta, release = await acquire_npu_slot_handle(owner, timeout_sec=timeout_sec)
    if not meta.get("enabled", True):
        return {
            "enabled": False,
            "owner": owner,
            "wait_ms": 0.0,
            "lease_id": "",
        }
    lease_id = str(uuid.uuid4())
    _lease_handles[lease_id] = release
    return {
        "enabled": True,
        "owner": owner,
        "wait_ms": float(meta.get("wait_ms", 0.0)),
        "lease_id": lease_id,
    }


async def release_npu_lease(lease_id: str) -> bool:
    """
    Release an external NPU lease token.

    Returns True when a lease existed and was released.
    """
    release = _lease_handles.pop(lease_id, None)
    if release is None:
        return False
    release()
    return True


def active_npu_leases() -> int:
    """Return the count of currently held external lease tokens."""
    return len(_lease_handles)


@asynccontextmanager
async def acquire_npu_slot(owner: str, timeout_sec: float | None = None):
    """
    Acquire process-wide NPU lock for orchestrator-routed heavy workloads.

    The lock serializes Image-To-Text and Image-Generation calls to prevent
    concurrent QNN deviceCreate failures on shared accelerator hardware.
    """
    meta, release = await acquire_npu_slot_handle(owner, timeout_sec=timeout_sec)
    try:
        yield meta
    finally:
        release()
