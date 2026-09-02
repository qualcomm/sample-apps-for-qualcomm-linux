# Unified Core-Service Test Suites

This folder is the canonical place for unified core-service API tests.

It standardizes:
- case format (YAML)
- runner behavior
- result schema (JSON + HTML)
- per-service case volume (45 cases each: 23 positive + 22 negative)

## Quick Navigation

- [What is included](#1-what-is-included)
- [Service coverage](#2-service-coverage)
- [Setup](#3-setup-host-machine)
- [Run one service suite](#4-run-one-service-suite)
- [Run all core services](#5-run-all-core-services-single-command)
- [OpenAI SDK I2T Multi-turn Smoke](#6-openai-sdk-i2t-multi-turn-smoke)
- [Dependency-aware behavior](#7-dependency-aware-behavior)
- [Runtime Budget](#8-runtime-budget-host-driven-full-run)
- [Troubleshooting](#9-troubleshooting)

## 1) What is included

- `manifest.yaml`
  - one entry per core runtime service
- `suites/*.yaml`
  - unified service test cases (45 per service)
- `run_http_suite.py`
  - executes one suite YAML
- `run_manifest.py`
  - executes all suites from manifest and writes a combined report
- `fixtures/`
  - shared test fixtures used by multiple suites
  - includes shared STT WAV fixtures under `tests/unified/fixtures/stt/`

## 2) Service coverage

Canonical suites in this folder:
- `text-to-text` -> `tests/unified/suites/text-to-text.yaml` (45)
- `speech-to-text` -> `tests/unified/suites/speech-to-text.yaml` (45)
- `image-to-text` -> `tests/unified/suites/image-to-text.yaml` (45; strict Responses `input[]` flow for direct + wrappers)
- `text-to-image` -> `tests/unified/suites/text-to-image.yaml` (45)
- `text-to-speech` -> `tests/unified/suites/text-to-speech.yaml` (45)
- `orchestrator` -> `tests/unified/suites/orchestrator.yaml` (45)

Naming convention (same for all suites):
- Positive case IDs: `<SERVICE_CODE>-P01` ... `<SERVICE_CODE>-P23`
- Negative case IDs: `<SERVICE_CODE>-N01` ... `<SERVICE_CODE>-N22`
- Service codes: `TTT`, `STT`, `I2T`, `T2I`, `TTS`, `ORC`

Notes:
- `core-services/platform` does not expose a standalone HTTP API service, so no independent suite exists for it.
- Cross-service long-form orchestrator suites remain under `core-services/orchestrator/tools`.
- Magic payload macros are supported in suite YAML:
  - `__GEN_WORDS_1000__` generates deterministic 1000-word input text.
  - `__GEN_GIBBERISH_1000__` generates deterministic 1000-word dummy/gibberish text.

## 3) Setup (host machine)

From repo root:

```bash
python3 -m venv .venv-tests
source .venv-tests/bin/activate
python3 -m pip install --upgrade pip
python3 -m pip install -r tests/unified/requirements.txt
```

or (no venv):

```bash
python3 -m pip install --user -r tests/unified/requirements.txt
```

Run from host machine where target endpoints are reachable.

Do not run unified suites from inside target containers.

## 4) Run one service suite

### Basic example (text-to-text):

```bash
python3 tests/unified/run_http_suite.py \
  --suite tests/unified/suites/text-to-text.yaml \
  --target-host <TARGET_DEVICE_IP> \
  --output tests/unified/reports/ttt_report.json
```

### Run against a different target device:

```bash
python3 tests/unified/run_http_suite.py \
  --suite tests/unified/suites/text-to-text.yaml \
  --target-host <ANOTHER_TARGET_DEVICE_IP> \
  --output tests/unified/reports/ttt_remote.json
```

### Override suite variables:

```bash
python3 tests/unified/run_http_suite.py \
  --suite tests/unified/suites/text-to-text.yaml \
  --target-host <TARGET_DEVICE_IP> \
  --var model_id=qwen3-4B \
  --output tests/unified/reports/ttt_custom.json
```

## 5) Run all core services (single command)

```bash
python3 tests/unified/run_manifest.py --target-host <TARGET_DEVICE_IP>
```

This generates:
- **Per-service reports:** `tests/unified/reports/<timestamp>/services/*.json|*.html`
- **Combined reports:** `tests/unified/reports/<timestamp>/report.json|report.html`

### Run only selected services:

```bash
python3 tests/unified/run_manifest.py --services text-to-text,speech-to-text
```

### Run against a different target device:

```bash
python3 tests/unified/run_manifest.py \
  --target-host <ANOTHER_TARGET_DEVICE_IP>
```

## 6) OpenAI SDK I2T Multi-turn Smoke

Run host-side OpenAI package validation against orchestrator `/v1/responses`:

```bash
python3 tests/unified/openai_i2t_responses_multiturn.py \
  --target-host <TARGET_DEVICE_IP> \
  --output tests/unified/reports/i2t_openai_multiturn.json
```

### Validation scope:
- **First turn:** image + text input
- **Second turn:** follow-up text in the same `X-Session-Id`
- **Output:** JSON + HTML report

## 7) Dependency-aware behavior

Suites can declare dependencies (for example, orchestrator depends on TG/STT/TTS/IMG/I2T).

### Behavior when a dependency is unavailable:
- Dependent cases are marked `SKIPPED_DEPENDENCY`
- Test run continues (does not halt)
- Report shows skipped count and reason

This keeps diagnostics useful even when part of the stack is unavailable.

### Exit code behavior:
- `run_manifest.py` returns non-zero **only** when there are true failed cases
- Skipped dependency cases do **not** fail the pipeline

## 8) Runtime Budget (Host-Driven Full Run)

### Recommended execution mode:
Run from host machine against target device IP.

### Timing estimates:
Based on 45-case suites (scaled from historical 30-case data):
- **Estimated median full run:** ~25-35 minutes
- **Recommended CI timeout:** 45 minutes
- **Recommended hard timeout (slower targets):** 60 minutes

### Full aggregate run:

```bash
python3 tests/unified/run_manifest.py --target-host <TARGET_DEVICE_IP>
```

The combined HTML report (`report.html`) includes:
- Per-service top issue snippets (failures + skips)
- Wall time per service

## 9) Troubleshooting

### Connection refused or timeout

**Problem:** Tests fail with connection errors to target host.

**Solution:**
- Verify target device is running and services are accessible
- Check firewall rules allow connections from host to target
- Confirm correct IP address and port: `curl http://<TARGET_DEVICE_IP>:<PORT>/health`
- Ensure you're using the target IP, not `localhost`

### SKIPPED_DEPENDENCY cases

**Problem:** Many cases marked as `SKIPPED_DEPENDENCY`.

**Solution:**
- Check that all dependent services are running on target
- Review the dependency chain in `manifest.yaml`
- Run individual service suites to isolate which service is down

### Slow test execution

**Problem:** Tests take longer than expected.

**Solution:**
- Check target device CPU/memory usage
- Reduce concurrent requests if applicable (check runner configuration)
- Consider running individual suites instead of full manifest
- Review network latency between host and target

### Python environment issues

**Problem:** `ModuleNotFoundError` or version conflicts.

**Solution:**
- Ensure virtual environment is activated: `source .venv-tests/bin/activate`
- Reinstall requirements: `python3 -m pip install -r tests/unified/requirements.txt --force-reinstall`
- Check Python version: `python3 --version` (3.8+ recommended)

### Report generation fails

**Problem:** JSON or HTML reports not created.

**Solution:**
- Verify output directory exists: `mkdir -p tests/unified/reports`
- Check write permissions on output directory
- Ensure `--output` path is writable
- Review runner logs for specific errors
