# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Text-to-Image (SD2.1 C++ service) - optimized multi-stage Docker build
#
# ─── Design ──────────────────────────────────────────────────────────────────
#   builder : genai-build-base:latest
#             Compiles image-generation-service using QAIRT/QNN headers already
#             present in the shared build base. Runtime libs remain external.
#
#   runtime : ubuntu-runtime:24.04
#             Ships ONLY the compiled service binary + run.sh
#
#   Model/runtime assets are NOT baked into the image. They are mounted at
#   runtime from the device, for example:
#     /opt/genai-studio-models/text-to-image/stable_diffusion_v2_1-qnn_context_binary-w8a16-qualcomm_qcs9075 -> /opt/runtime
#
#
#   Required under /opt/runtime:
#     - text_encoder.bin
#     - unet.bin
#     - vae.bin
#     - tokenizer/vocab.json
#     - tokenizer/merges.txt
#     - libQnnHtp.so, libQnnSystem.so, libQnnHtpV73Skel.so, etc.
#
#   The SD2.1 direct runner is built from:
#     core-services/text-to-image/deps/cpp_sd21_qnn/main_direct_qnn.cpp
#   and embedded into the image as:
#     /usr/bin/sd21_qnn_cpp_direct
#
#   Runtime transport note:
#     On target, the native working SD2.1 direct runner uses libxdsprpc.so.
#     The container must therefore expose both libxdsprpc.so and libcdsprpc.so
#     from the host/device runtime.
#
# ─── Build ───────────────────────────────────────────────────────────────────
#   DOCKER_BUILDKIT=1 docker build -f core-services/text-to-image/Dockerfile -t text-to-image:latest .
#
# ─── Run ─────────────────────────────────────────────────────────────────────
#   docker run -d --name ImageGen --network host --privileged \
#     -v /opt/genai-studio-models/text-to-image/stable_diffusion_v2_1-qnn_context_binary-w8a16-qualcomm_qcs9075:/opt/runtime \
#     text-to-image:latest
#
# ─── API ─────────────────────────────────────────────────────────────────────
#   GET  /health
#   GET  /v1/models
#   GET  /v1/images/generations/params
#   POST /v1/images/generations
#   POST /generate
# =============================================================================

# syntax=docker/dockerfile:1.7

FROM genai-build-base:latest AS builder

WORKDIR /app

# Copy service source + direct runner source (kept under core-services/text-to-image).
# Build context is repo root.
COPY src/ ./core-services/text-to-image/src/
COPY deps/cpp_sd21_qnn/main_direct_qnn.cpp ./core-services/text-to-image/deps/cpp_sd21_qnn/main_direct_qnn.cpp

# This service includes:
#   #include "httplib/httplib.h"
# and also:
#   #include "stb_image_write.h"
# The remote repo does not have src/third_party checked in, so provide both
# headers from the build image/network at build time.
RUN mkdir -p ./core-services/text-to-image/src/httplib ./core-services/text-to-image/src/crow && \
    cp /opt/headers/httplib/httplib.h ./core-services/text-to-image/src/httplib/httplib.h && \
    cp /opt/headers/httplib/httplib.h ./core-services/text-to-image/src/crow.h && \
    cp /opt/headers/httplib/httplib.h ./core-services/text-to-image/src/crow/crow.h && \
    cp /opt/headers/nlohmann/json.hpp ./core-services/text-to-image/src/json.hpp && \
    cp /opt/headers/stb/stb_image_write.h ./core-services/text-to-image/src/stb_image_write.h

RUN rm -rf ./core-services/text-to-image/build && \
    cmake -S ./core-services/text-to-image/src -B ./core-services/text-to-image/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DQNN_SDK_ROOT="${QAIRT_SDK_ROOT:-/opt/qairt}" && \
    cmake --build ./core-services/text-to-image/build --parallel "$(nproc)" && \
    cp ./core-services/text-to-image/build/image-generation-service /tmp/image-generation-service && \
    strip --strip-unneeded /tmp/image-generation-service || true

RUN g++ -std=c++17 -O3 -march=native -Wall -Wextra \
      -I/opt/qairt/include -I/opt/qairt/include/QNN \
      -o /tmp/sd21_qnn_cpp_direct ./core-services/text-to-image/deps/cpp_sd21_qnn/main_direct_qnn.cpp -ldl && \
    strip --strip-unneeded /tmp/sd21_qnn_cpp_direct || true

FROM ubuntu-runtime:24.04

WORKDIR /app/

# Runtime model/runtime mount
ENV MODEL_DIR=/opt/runtime
ENV PORT=8084
ENV IMAGE_GEN_CACHE_SIZE=16

# The mounted runtime dir provides model assets, precompiled contexts, direct
# runner binary, and QAIRT/QNN runtime libraries.
ENV LD_LIBRARY_PATH=/opt/runtime:/usr/lib:/usr/lib/aarch64-linux-gnu
ENV ADSP_LIBRARY_PATH=/opt/runtime;/usr/lib/rfsa/adsp;/usr/lib/dsp;/dsp;/usr/lib/dsp/cdsp1

COPY --link --chmod=0755 --from=builder /tmp/image-generation-service /usr/bin/image-generation-service
COPY --link --chmod=0755 --from=builder /tmp/sd21_qnn_cpp_direct /usr/bin/sd21_qnn_cpp_direct
COPY --link --chmod=0755 run.sh /app/run.sh
# Required by sd21_qnn_cpp_direct on target; without this, image generation
# may fail at runtime with: "Failed to load system lib: libatomic.so.1".
COPY --link --from=builder /usr/lib/aarch64-linux-gnu/libatomic.so.1 /usr/lib/aarch64-linux-gnu/libatomic.so.1
# Required by HTP transport path in the direct runner.
COPY --link --from=builder /usr/lib/aarch64-linux-gnu/libdmabufheap.so.0.0.0 /usr/lib/aarch64-linux-gnu/libdmabufheap.so.0.0.0

# Some environments provide only libcdsprpc.so.1; direct runner loads libcdsprpc.so.
RUN if [ -e /usr/lib/aarch64-linux-gnu/libcdsprpc.so.1 ] && [ ! -e /usr/lib/aarch64-linux-gnu/libcdsprpc.so ]; then \
      ln -s /usr/lib/aarch64-linux-gnu/libcdsprpc.so.1 /usr/lib/aarch64-linux-gnu/libcdsprpc.so; \
    fi && \
    if [ -e /usr/lib/aarch64-linux-gnu/libdmabufheap.so.0.0.0 ] && [ ! -e /usr/lib/aarch64-linux-gnu/libdmabufheap.so.0 ]; then \
      ln -s /usr/lib/aarch64-linux-gnu/libdmabufheap.so.0.0.0 /usr/lib/aarch64-linux-gnu/libdmabufheap.so.0; \
    fi && \
    if [ -e /usr/lib/aarch64-linux-gnu/libdmabufheap.so.0 ] && [ ! -e /usr/lib/aarch64-linux-gnu/libdmabufheap.so ]; then \
      ln -s /usr/lib/aarch64-linux-gnu/libdmabufheap.so.0 /usr/lib/aarch64-linux-gnu/libdmabufheap.so; \
    fi

EXPOSE 8084

CMD ["./run.sh"]
