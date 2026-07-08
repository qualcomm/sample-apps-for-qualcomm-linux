# Core Services Layout

## Canonical Service Paths

All services are located under the `core-services/` directory structure:

- `core-services/text-to-text`
- `core-services/image-to-text`
- `core-services/text-to-image`
- `core-services/speech-to-text`
- `core-services/text-to-speech`
- `core-services/orchestrator`

**Note:** Legacy service folders at the repository root have been consolidated. All service sources and configurations are now maintained exclusively within `core-services/`. References in scripts, documentation, and deployment commands must use only the `core-services/*` canonical paths.

## Service Map

| Service | Source Directory | Dockerfile | Main Documentation | Model Setup | Port |
|---|---|---|---|---|---|
| Text-to-Text | [`core-services/text-to-text`](text-to-text) | [`core-services/text-to-text/Dockerfile`](text-to-text/Dockerfile) | [`core-services/text-to-text/README.md`](text-to-text/README.md) | [`core-services/text-to-text/MODEL_SETUP.md`](text-to-text/MODEL_SETUP.md) | 8088 |
| Image-to-Text | [`core-services/image-to-text`](image-to-text) | [`core-services/image-to-text/Dockerfile`](image-to-text/Dockerfile) | [`core-services/image-to-text/README.md`](image-to-text/README.md) | [`core-services/image-to-text/MODEL_SETUP.md`](image-to-text/MODEL_SETUP.md) | 8080 |
| Text-to-Image | [`core-services/text-to-image`](text-to-image) | [`core-services/text-to-image/Dockerfile`](text-to-image/Dockerfile) | [`core-services/text-to-image/README.md`](text-to-image/README.md) | [`core-services/text-to-image/MODEL_SETUP.md`](text-to-image/MODEL_SETUP.md) | 8084 |
| Speech-to-Text | [`core-services/speech-to-text`](speech-to-text) | [`core-services/speech-to-text/Dockerfile`](speech-to-text/Dockerfile) | [`core-services/speech-to-text/README.md`](speech-to-text/README.md) | [`core-services/speech-to-text/MODEL_SETUP.md`](speech-to-text/MODEL_SETUP.md) | 8081 |
| Text-to-Speech | [`core-services/text-to-speech/meloTTS`](text-to-speech/meloTTS) | [`core-services/text-to-speech/meloTTS/Dockerfile`](text-to-speech/meloTTS/Dockerfile) | [`core-services/text-to-speech/meloTTS/README.md`](text-to-speech/meloTTS/README.md) | [`core-services/text-to-speech/meloTTS/Model-Generation.md`](text-to-speech/meloTTS/Model-Generation.md) | 8083 |
| Orchestrator | [`core-services/orchestrator`](orchestrator) | [`core-services/orchestrator/Dockerfile`](orchestrator/Dockerfile) | [`core-services/orchestrator/README.md`](orchestrator/README.md) | [`core-services/orchestrator/SETUP.md`](orchestrator/SETUP.md) | 8090 |

## Private SDK Build Contexts

These are staged on target inside the repo checkout:

- STT SDK payload:
  - `core-services/speech-to-text/whisper_sdk`
- TTS SDK payload:
  - `core-services/text-to-speech/meloTTS/melo_sdk`
  - optional archive alternative: `core-services/text-to-speech/meloTTS/1.1.1.0.zip`

## Shared Files Outside `core-services`

These stay at repo root because they are shared by multiple services:

- [`docker-compose.yml`](../docker-compose.yml)
- [`Dockerfile.runtime`](../Dockerfile.runtime)
- [`Dockerfile.build-base`](../Dockerfile.build-base)
- [`Dockerfile.base`](../Dockerfile.base)
- [`README.md`](../README.md)

Shared setup docs now live under [`docs/setup/`](../docs/setup/).

Use [`README.md`](../README.md) as the main onboarding path.
