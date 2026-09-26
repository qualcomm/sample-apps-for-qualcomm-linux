# Startup Script Guide

This guide explains the startup pipeline scripts under `scripts/`, their flags, and how to customize behavior for your device/workflow.

## 1) Main Entrypoint: `scripts/genai-studio.sh`

This is the orchestrator script for bring-up.

Default behavior:

1. `device-setup`
2. `model-setup` (mapped to `scripts/phases/model_gen.sh`)
3. `build`
4. `validate`

Current implementation detail:

- In `--phase all`, `model-setup` and `build` run in parallel after `device-setup`.
- `validate` runs only after both complete successfully.

### Flags

- `--target <ip>`: accepted but remote execution is currently not implemented.
- `--user <username>`: accepted for future remote mode.
- `--password <password>`: accepted for future remote mode.
- `--phase <phase...>`: run selected phases.
  - valid phases: `device-setup`, `model-setup`, `build`, `validate`, `all`
- `--resume`: resume from checkpoint state (`.genai-state/`) (default on).
- `--no-resume`: ignore checkpoints and start from beginning.
- `--force`: rerun completed phases.
- `--force-clean`: passes force-clean behavior into build phase.
- `--skip-start`: used by validate phase to skip `docker compose up -d`.
- `--help`

### Typical usage

```bash
# Full pipeline
bash scripts/genai-studio.sh --phase all

# Build + validate only
bash scripts/genai-studio.sh --phase build validate

# Resume after interruption
bash scripts/genai-studio.sh --resume
```

## 2) Phase Scripts Used by Main Pipeline

## 2.1 `scripts/phases/device-setup.sh`

Purpose:

- Detect platform profile (Ubuntu / QLI variants)
- Validate/install prerequisites (Docker, compose/CDI checks, QAIRT checks)
- Generate/update `.env`

Flags:

- `--provision` (default)
- `--validate-only`
- `--force`
- `--help`

Examples:

```bash
bash scripts/phases/device-setup.sh
bash scripts/phases/device-setup.sh --validate-only
bash scripts/phases/device-setup.sh --force
```

## 2.2 `scripts/phases/model_gen.sh` (mapped from `model-setup`)

Purpose:

- Download/prepare/validate model bundles for:
  - T2T, I2T, STT, T2I, TTS source + TTS packed `.qnn`
- Skip download when artifacts already exist.
- Support force redownload/regeneration.

Key flags:

- Service selection:
  - `--service text-to-text|image-to-text|speech-to-text|text-to-image|text-to-speech`
- Download toggles:
  - `--download-t2t`, `--skip-download-t2t`
  - `--download-i2t`, `--skip-download-i2t`
  - `--download-stt`, `--skip-download-stt`
  - `--download-t2i`, `--skip-download-t2i`
  - `--download-tts`, `--skip-download-tts`
  - `--download-all`, `--download-none`, `--validate-only`
- TTS packing:
  - `--generate-tts-qnn`, `--skip-generate-tts-qnn`
- Force behavior:
  - `--force-download`
- Source overrides (URL or local zip path):
  - `--t2t-url`, `--i2t-url`, `--stt-url`, `--t2i-url`, `--tts-url`
- Target dir overrides:
  - `--t2t-model-dir`, `--i2t-model-dir`, `--stt-model-dir`, `--t2i-model-dir`
  - `--tts-source-dir`, `--tts-model-dir`
  - `--tts-conversion-root`, `--tts-packer-script`, `--tts-sdk-root`

Examples:

```bash
# Default all services (incremental)
bash scripts/phases/model_gen.sh

# Only I2T
bash scripts/phases/model_gen.sh --service image-to-text

# Force refresh I2T
bash scripts/phases/model_gen.sh --service image-to-text --force-download

# Validate-only mode
bash scripts/phases/model_gen.sh --validate-only
```

## 2.3 `scripts/phases/build-stack.sh`

Purpose:

- Build base images + service images in dependency order.
- Optional cache/image cleanup before rebuild.

Flags:

- `--force`
- `--force-clean`
- `--help`
- Optional service list args:
  - `text-to-text`, `image-to-text`, `text-to-image`, `speech-to-text`, `text-to-speech`, `orchestrator`, `all`

Examples:

```bash
# Build all
bash scripts/phases/build-stack.sh

# Build selected services
bash scripts/phases/build-stack.sh text-to-text orchestrator

# Clean and rebuild
bash scripts/phases/build-stack.sh --force-clean
```

## 2.4 `scripts/phases/validate-stack.sh`

Purpose:

- Start compose stack (unless skipped)
- Wait for orchestrator health
- Check `/api/status`
- Run repo validation script (`scripts/validate-stack.sh --skip-start`) if present

Flags:

- `--skip-start`
- `--help`

Examples:

```bash
bash scripts/phases/validate-stack.sh
bash scripts/phases/validate-stack.sh --skip-start
```

## 3) Customization Guide

Most customization is done through environment variables in shell or `.env`.

Common model/runtime paths:

- `TG_MODEL_HOST_DIR`
- `I2T_MODEL_HOST_DIR` (default host path pattern under `/opt/genai-studio-models/image-to-text/...`)
- `STT_MODEL_HOST_DIR`
- `IMAGEGEN_MODEL_DIR`
- `TTS_MODEL_HOST_DIR`
- `TG_QAIRT_LIBS_HOST_DIR`
- `I2T_QAIRT_FLAT_LIB_DIR`
- `STT_QNN_LIB_HOST_DIR`
- `IMG_QAIRT_LIBS_HOST_DIR`
- `TTS_QAIRT_FLAT_LIB_DIR`

Service-specific model IDs:

- `TG_MODEL_NAME`, `TG_MODEL_DIR`, `GENIE_CONFIG`, `BASE_DIR`
- `TG_DIRECT_MODEL_ID`, `TG_ORCHESTRATOR_MODEL_ID`
- `I2T_MODEL_NAME`, `I2T_MODEL_DIR`
- optional I2T config knobs:
  - `I2T_IMAGE_ENCODER_CONFIG`
  - `I2T_TEXT_ENCODER_CONFIG`
  - `I2T_TEXT_DECODER_CONFIG`
  - `I2T_INPUTS_DIR`
  - `I2T_PATCH_JSON_ABS_PATHS`

## 4) Features Present Today

- Checkpoint/resume across phases via `.genai-state/`
- Force rerun support (`--force`)
- Parallel execution in full flow (`model-setup` + `build`)
- Ctrl+C cleanup trap in `genai-studio.sh` for phase background jobs and common descendants
- Automatic compose health/status validation

## 5) Practical Workflows

### Fast iterative loop

```bash
# Run setup once
bash scripts/genai-studio.sh --phase device-setup

# During development
bash scripts/genai-studio.sh --phase model-setup build
bash scripts/genai-studio.sh --phase validate --skip-start
```

### Recover after docker group refresh exit

If a phase exits with refresh-needed guidance (exit code `2`):

```bash
newgrp docker
bash scripts/genai-studio.sh --resume
```

## 6) Related Scripts (outside main phase runner)

- `scripts/build-all.sh`: canonical wrapper for full image build matrix with skip flags.
- `scripts/download-qairt-sdk.sh`: prepares local `qairt-sdk/` slice from an installed QAIRT root.
- `scripts/compose-doctor.sh`: host/path preflight checks.
- `scripts/validate-stack.sh`: validation wrapper used by phase-4 script.

## 7) Notes

- `--target/--user/--password` flags are parsed but remote execution is intentionally blocked for now.
- If you customize paths, keep compose mounts and service env defaults aligned to avoid runtime mismatches.
