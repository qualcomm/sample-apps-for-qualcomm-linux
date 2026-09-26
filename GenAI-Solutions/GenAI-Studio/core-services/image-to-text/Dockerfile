# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Image-to-Text (llamachat VLM) - optimized multi-stage Docker build
#
# ─── Size target ─────────────────────────────────────────────────────────────
#   < 700 MB  (ubuntu-runtime:24.04 base = 688 MB + llamachat binary)
#
# ─── Two stages ──────────────────────────────────────────────────────────────
#   builder  : genai-build-base:latest (cmake + gcc + Qualcomm dev headers)
#              Compiles llamachat VLM binary. NOT shipped.
#
#   runtime  : ubuntu-runtime:24.04 (minimal flat layer, no firmware)
#              Ships ONLY: llamachat binary
#              libGenie.so + QNN libs provided at runtime via volume + CDI
#
# Prerequisites
#   Run from repo root via scripts/build-all.sh (handles all prereqs):
#     bash scripts/build-all.sh
#
#   Or manually:
#     bash scripts/pull-ubuntu-arm64.sh
#     bash scripts/create-runtime-base.sh
#     DOCKER_BUILDKIT=1 docker build -f Dockerfile.build-base -t genai-build-base:latest .
#     DOCKER_BUILDKIT=1 docker build -t image-to-text:responses-v1 core-services/image-to-text/
# =============================================================================

# syntax=docker/dockerfile:1.7

# ═══════════════════════════════════════════════════════════════════════════════
# Stage 1: Builder — compile llamachat VLM binary
# Uses genai-build-base which already has httplib + nlohmann/json headers.
# ═══════════════════════════════════════════════════════════════════════════════
FROM genai-build-base:latest AS builder

WORKDIR /app/core-services/image-to-text

# Copy C++ source files
COPY src/ ./src/

# QAIRT SDK (Genie + QNN headers + libGenie.so) is already baked into genai-build-base:latest
# at /opt/qairt/ — no need to COPY qairt-sdk/ here.
# ENV QAIRT_SDK_ROOT=/opt/qairt is set in genai-build-base.

# Reuse pre-downloaded header-only libs from genai-build-base (no wget needed)
RUN mkdir -p ./src/third_party/httplib ./src/third_party/nlohmann && \
    cp /opt/headers/httplib/httplib.h  ./src/third_party/httplib/httplib.h && \
    cp /opt/headers/nlohmann/json.hpp  ./src/third_party/nlohmann/json.hpp

# Build llamachat VLM binary
# QAIRT_SDK_ROOT=/opt/qairt is set in the base image ENV
# LINK_QNN_RUNTIME_LIBS=OFF: QNN libs provided at runtime via volume mount
RUN rm -rf ./src/build && \
    mkdir -p ./src/build && \
    cd ./src/build && \
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DQNN_SDK_ROOT="${QAIRT_SDK_ROOT}" \
        -DQNN_LIB_SUBDIR=aarch64-oe-linux-gcc11.2 \
        -DLINK_QNN_RUNTIME_LIBS=OFF && \
    make -j"$(nproc)" llamachat && \
    cp llamachat /tmp/llamachat && \
    strip --strip-unneeded /tmp/llamachat || true

# ═══════════════════════════════════════════════════════════════════════════════
# Stage 2: Runtime — minimal flat image (target: < 700 MB)
#
# Uses ubuntu-runtime:24.04 — pre-built minimal base (688 MB flat layer).
# Created by: bash scripts/create-runtime-base.sh
#
# Contains: qcom-fastrpc1, libstdc++6, libatomic1, curl, bash (no firmware)
#
# NOT included (provided at runtime via volume + CDI device):
#   libGenie.so    : from -v /opt/genai-studio-models/image-to-text/.../files:...
#   libQnnHtp.so   : from --device qualcomm.com/device=cdi-hw-acc
#   libQnnSystem.so: from --device qualcomm.com/device=cdi-hw-acc
# ═══════════════════════════════════════════════════════════════════════════════
FROM ubuntu-runtime:24.04

# Python runtime for in-container image preprocessing (OpenAI-style image_url).
# Ubuntu 24.04 enforces PEP 668, so install deps into an isolated venv.
RUN apt-get update && \
    apt-get install -y --no-install-recommends python3 python3-pip python3-venv && \
    python3 -m venv /opt/i2t-venv && \
    /opt/i2t-venv/bin/pip install --no-cache-dir --upgrade pip setuptools wheel && \
    /opt/i2t-venv/bin/pip install --no-cache-dir \
      numpy \
      pillow \
      "transformers>=4.37.0,<5.0.0" && \
    rm -rf /var/lib/apt/lists/* /root/.cache/pip

# Model dir provides libGenie.so, libQnnHtp.so, libQnnSystem.so at runtime
ENV MODEL_DIR=/opt/I2T_binary/files
ENV LD_LIBRARY_PATH=/opt/I2T_binary/files:/usr/lib
ENV ADSP_LIBRARY_PATH=/opt/I2T_binary/files:/usr/lib:/dsp
ENV PATH=/opt/i2t-venv/bin:${PATH}
ENV I2T_PREPROCESS_PYTHON=/opt/i2t-venv/bin/python
ENV I2T_PREPROCESS_SCRIPT=/opt/i2t/preprocess.py

# Copy ONLY the compiled binary from builder stage
COPY --link --chmod=0755 --from=builder /tmp/llamachat /usr/bin/llamachat
COPY --link --chmod=0755 src/preprocess.py /opt/i2t/preprocess.py

WORKDIR /app/
COPY --link --chmod=0755 run.sh /app/run.sh

# Port 8080 — llamachat VLM HTTP server
EXPOSE 8080

# ─── Runtime volume mounts (provided by docker run / docker compose) ──────────
# -v /opt/genai-studio-models/image-to-text/.../files:/opt/I2T_binary/files
#     Provides: libGenie.so, libQnnHtp.so, libQnnSystem.so, model files
# --device qualcomm.com/device=cdi-hw-acc → /dev/fastrpc-cdsp + QNN runtime libs
CMD ["./run.sh"]
