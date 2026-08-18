# GenAI Studio API Contracts

This document defines cross-service contracts used by docs, clients, and tests.

## 1) Health and Readiness

- `/health`: process liveness only.
- `/ready`: inference readiness (model/runtime can accept work now).

Current readiness policy in this repo:

- Text-Generation exposes and enforces `/ready`.
- Orchestrator `/api/status` reports both liveness and readiness details per service.
- Services without `/ready` are reported as `ready_status=unsupported` (liveness-only).
- STT readiness gate is operationally defined as:
  - STT is reachable via orchestrator `/api/status`
  - one minimal STT transcription smoke request succeeds.

## 1.1 STT Traffic Policy

- Client STT traffic should go through orchestrator (`:8090`) only.
- Direct STT service access (`:8081`) is for isolated backend/debug checks.

## 2) Text-Generation Model ID Contract

To avoid model routing ambiguity:

- Direct Text-Generation (`:8088`):
  - request `model` must match active model id from `GET /v1/models`
  - unknown model ids return deterministic `400 invalid_request_error`
- Orchestrator Text route (`:8090`):

Environment knobs:

- `TG_ORCHESTRATOR_MODEL_ID` (default `genie`)
- `TG_DIRECT_MODEL_ID` (default `qwen3-4B`)

## 3) `/v1/chat/completions` Routing Policy

Orchestrator route selection:

- If payload includes vision intent (image URL content or I2T session continuation), route to Image-To-Text flow.
- Otherwise route to Text-Generation flow.

## 4) Error Envelope

Canonical error shape:

```json
{
  "error": {
    "message": "human readable message",
    "type": "invalid_request_error",
    "code": "invalid_request"
  }
}
```

Guidelines:

- `type` is required.
- `code` is required and deterministic.
- For STT saturation/capacity outcomes, user-facing code should be `rate_limited` with HTTP `429`.

## 5) Legacy Endpoint Policy

Public documentation policy:

- keep: `POST /reload_model` (compatibility/operations)
- hide other legacy compatibility endpoints from public docs

Engineering policy:

- hidden legacy endpoints may remain in compatibility tests until formal deprecation/removal.
- new behavior must be added only to `/v1/*` endpoints.
