# GenAI Studio: Complete Documentation Index & Repository Guide

This document serves as the authoritative reference for repository structure, documentation organization, and codebase navigation.

**Getting started:** Begin with [README.md](../README.md) for initial deployment and setup, then consult this guide for targeted documentation lookup and architectural context.

---

## Quick Navigation by Task

| Your Task | Refer |
|---|---|
| **Full-stack bring-up** | [README.md](../README.md) |
| **Startup script flags and customization** | [docs/STARTUP_SCRIPT_GUIDE.md](STARTUP_SCRIPT_GUIDE.md) |
| **First-time target provisioning** | [docs/setup/DEVICE_SETUP.md](setup/DEVICE_SETUP.md) |
| **Troubleshooting runtime errors** | [docs/TROUBLESHOOTING_GUIDE.md](TROUBLESHOOTING_GUIDE.md) |
| **Service map, ports, and pointers to individual service docs** | [core-services/README.md](../core-services/README.md) |
| **Running validation tests** | [tests/unified/README.md](../tests/unified/README.md) |
| **Integrating external applications** | [solutions/README.md](../solutions/README.md) |

---

## All Documentation Files & Their Purpose

| File | Purpose |
|---|---|
| [README.md](../README.md) | **Primary entry point** — canonical host-vs-target flow: preflight, build, compose up, health, functional tests |
| [core-services/README.md](../core-services/README.md) | Service map, ports, and pointers to individual service docs |
| [core-services/text-to-text/README.md](../core-services/text-to-text/README.md) | T2T API behavior and service-local validation commands |
| [core-services/text-to-text/MODEL_SETUP.md](../core-services/text-to-text/MODEL_SETUP.md) | T2T model selection, download, and runtime configuration |
| [core-services/image-to-text/README.md](../core-services/image-to-text/README.md) | I2T `/v1/responses` usage and vision request examples |
| [core-services/image-to-text/MODEL_SETUP.md](../core-services/image-to-text/MODEL_SETUP.md) | I2T model selection, download, and runtime configuration |
| [core-services/text-to-image/README.md](../core-services/text-to-image/README.md) | T2I generation endpoints, payload shapes, and smoke checks |
| [core-services/text-to-image/MODEL_SETUP.md](../core-services/text-to-image/MODEL_SETUP.md) | T2I model selection, download, and runtime configuration |
| [core-services/speech-to-text/README.md](../core-services/speech-to-text/README.md) | STT transcription/realtime endpoints and model handling |
| [core-services/speech-to-text/MODEL_SETUP.md](../core-services/speech-to-text/MODEL_SETUP.md) | STT model selection, download, and runtime configuration |
| [core-services/text-to-speech/meloTTS/README.md](../core-services/text-to-speech/meloTTS/README.md) | TTS synthesis endpoints, runtime requirements, and smoke checks |
| [core-services/text-to-speech/meloTTS/Model-Generation.md](../core-services/text-to-speech/meloTTS/Model-Generation.md) | TTS model selection, download, and runtime configuration |
| [core-services/orchestrator/README.md](../core-services/orchestrator/README.md) | Unified API routes (`:8090`) and upstream service routing behavior |
| [docs/setup/DEVICE_SETUP.md](setup/DEVICE_SETUP.md) | Device provisioning and low-level platform checks |
| [docs/STARTUP_SCRIPT_GUIDE.md](STARTUP_SCRIPT_GUIDE.md) | Startup automation reference for `scripts/genai-studio.sh` and phase scripts (flags, customization, workflows) |
| [docs/TROUBLESHOOTING_GUIDE.md](TROUBLESHOOTING_GUIDE.md) | Complete troubleshooting reference: error signatures, 5-step playbook, service pain points, and triage paths |
| [tests/README.md](../tests/README.md) | Test folder organization and where suites live |
| [tests/unified/README.md](../tests/unified/README.md) | Unified service-by-service and full-stack test execution |
| [scripts/README.md](../scripts/README.md) | Optional automation scripts and reproducibility helpers |
| [solutions/README.md](../solutions/README.md) | Integration guides for external applications (web, mobile, C++, etc.) |

---

## Repository Structure

### Top-Level Layout

```text
genai-studio/
├── README.md                          # Primary entry point
├── docker-compose.yml                 # Full-stack orchestration
├── Dockerfile.base                    # Shared base image
├── Dockerfile.build-base              # Shared builder base (C++ services)
├── Dockerfile.runtime                 # Shared runtime base
├── core-services/                     # All AI services
├── docs/                              # Documentation
│   ├── setup/
│   │   └── DEVICE_SETUP.md
│   ├── TROUBLESHOOTING_GUIDE.md       # Error signatures, playbook, pain points
│   └── DOCUMENTATION_INDEX.md         # This file
├── scripts/                           # Automation and helpers
├── tests/                             # Test suites
├── solutions/                         # Integration examples
└── assets/                            # Static assets
```

---

### Shared Files (Repo Root)

- **`docker-compose.yml`** — full stack orchestration, common runtime mounts and env wiring
- **`Dockerfile.runtime`** — shared runtime base for all services
- **`Dockerfile.build-base`** — shared builder base for C++ services (requires repo-root `qairt-sdk/` during clean builds)
- **`Dockerfile.base`** — compatibility or legacy shared base file if still referenced locally
- **Shared setup and onboarding docs** — `README.md`, `docs/` files
- **Shared scripts and validation** — `scripts/` folder

### Service-Specific Files (core-services/)

- Service `Dockerfile`
- Service source code
- Service README
- Service model/setup notes
- Service tests

---

## Core Services Tree

```text
core-services/
├── README.md                          # Service map and ports
├── text-to-text/
│   ├── Dockerfile
│   ├── README.md
│   └── MODEL_SETUP.md
├── image-to-text/
│   ├── Dockerfile
│   ├── README.md
│   └── MODEL_SETUP.md
├── text-to-image/
│   ├── Dockerfile
│   ├── README.md
│   └── MODEL_SETUP.md
├── speech-to-text/
│   ├── Dockerfile
│   ├── README.md
│   └── MODEL_SETUP.md
├── text-to-speech/
│   └── meloTTS/
│       ├── Dockerfile
│       ├── README.md
│       └── Model-Generation.md
└── orchestrator/
    ├── Dockerfile
    └── README.md
```

---

## Private SDK Staging Layout

Private SDK payloads should be prepared on host and staged onto the target checkout here:

```text
core-services/speech-to-text/whisper_sdk
core-services/text-to-speech/meloTTS/melo_sdk
```

**Why these live in the repo checkout on target:**

- Docker builds need them in the target build context
- Service docs can point to one canonical path
- Users don't need to remember extra external dependency folders 
---

## Runtime Model Layout

Runtime model folders should stay **outside the repo** under `/opt/`:

```text
/opt/genai-studio-models/text-to-text/
/opt/genai-studio-models/image-to-text/
/opt/genai-studio-models/text-to-image/
/opt/genai-studio-models/speech-to-text/
/opt/genai-studio-models/text-to-speech/
/opt/genai-studio-cache/huggingface/
/opt/qairt/current/
/opt/qairt/current/qairt_245_flat_libs/
```

**Model folder location:**

- Models go in `/opt/genai-studio-models/` on the target device
- This keeps them outside the repo for easier syncing and clean builds
- `docker-compose.yml` mounts them from `/opt/` — no changes needed per service
- **Not required** — you can mount models from anywhere; `/opt/` is just the recommended convention

---

## Solutions & Integration Examples

The `solutions/` folder contains drop-in integration guides for external applications:

| Solution | Framework | What It Shows |
|---|---|---|
| [android-application/maid](../solutions/android-application/maid/GENAI_STUDIO_INTEGRATION.md) | React Native / Expo (Android) | Streaming chat completions via OpenAI provider — no code changes needed |
| [anything-llm](../solutions/anything-llm/GENAI_STUDIO_INTEGRATION.md) | Docker · Windows app · Ubuntu app | Full RAG assistant with chat, TTS, and STT routed through GenAI Studio |
| [cpp-application](../solutions/cpp-application/GENAI_STUDIO_INTEGRATION.md) | C++ (header-only) | Chat completions, model listing, and STT using `openai-cpp` library |
| [web-application/image-generation](../solutions/web-application/image-generation/GENAI_STUDIO_INTEGRATION.md) | Python / Streamlit | Text-to-image generation replacing DALL-E |
| [web-application/text-to-speech](../solutions/web-application/text-to-speech/GENAI_STUDIO_INTEGRATION.md) | Python / Gradio | Text-to-speech replacing OpenAI TTS |

All solutions work with GenAI Studio's OpenAI-compatible REST API on port `8090`.

---

## Recommended Reading Order for New Users

1. **[README.md](../README.md)** — Get the full-stack running
2. **This file (docs/DOCUMENTATION_INDEX.md)** — Understand the layout and find what you need
3. **[docs/setup/DEVICE_SETUP.md](setup/DEVICE_SETUP.md)** — Only if target provisioning or platform checks fail
4. **[docs/TROUBLESHOOTING_GUIDE.md](TROUBLESHOOTING_GUIDE.md)** — Only when troubleshooting is needed
5. **Service README and model doc** — Only for the specific service you're working with
6. **[solutions/README.md](../solutions/README.md)** — When integrating external applications

---

## Summary

This unified documentation index consolidates:

- **Navigation guidance** — Quick task-based routing to the right documentation
- **File organization rules** — Where shared vs. service-specific files belong
- **Repository structure** — Complete tree layouts for root, services, SDKs, and models
- **Documentation reference** — Purpose and location of every key document
- **Integration examples** — Solutions folder overview
- **Reading order** — Recommended path for new users

Use the **Quick Navigation by Task** table at the top to find what you need, or follow the **Recommended Reading Order** for a comprehensive onboarding experience.
