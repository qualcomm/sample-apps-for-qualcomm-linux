# ---------------------------------------------------------------------
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------

FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
ARG MELO_SDK_SOURCE_URL=""
ARG MELO_SDK_SOURCE_REF=""
ARG MELO_SDK_SUBDIR="melo_sdk"
ARG MELO_SDK_ARCHIVE_SHA256=""

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        git \
        unzip \
        software-properties-common \
        cmake \
        make \
        gcc \
        g++ \
        wget \
        nlohmann-json3-dev && \
    apt-add-repository -y ppa:ubuntu-qcom-iot/qcom-ppa && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        qcom-fastrpc-dev \
        qcom-dspservices-headers-dev \
        qcom-fastrpc1 && \
    rm -rf /var/lib/apt/lists/*

# Pre-download httplib header-only library from GitHub
ARG HTTPLIB_VERSION=v0.15.3
RUN mkdir -p /opt/headers/httplib && \
    wget -q --tries=3 --timeout=30 \
        "https://raw.githubusercontent.com/yhirose/cpp-httplib/${HTTPLIB_VERSION}/httplib.h" \
        -O /opt/headers/httplib/httplib.h

WORKDIR /app/core-services/text-to-speech/meloTTS

COPY . ./

RUN set -eux; \
    mkdir -p /tmp/melo-sdk-extract /opt/melo-sdk-source; \
    if [ -n "${MELO_SDK_SOURCE_URL}" ]; then \
        echo "[build] Fetching Melo SDK source from ${MELO_SDK_SOURCE_URL}"; \
        if printf '%s' "${MELO_SDK_SOURCE_URL}" | grep -Eq '\.git$'; then \
            if [ -n "${MELO_SDK_SOURCE_REF}" ]; then \
                git clone --depth=1 --branch "${MELO_SDK_SOURCE_REF}" "${MELO_SDK_SOURCE_URL}" /opt/melo-sdk-source; \
            else \
                git clone --depth=1 "${MELO_SDK_SOURCE_URL}" /opt/melo-sdk-source; \
            fi; \
        else \
            curl -fsSL "${MELO_SDK_SOURCE_URL}" -o /tmp/melo-sdk-source-archive; \
            if [ -n "${MELO_SDK_ARCHIVE_SHA256}" ]; then \
                printf '%s  %s\n' "${MELO_SDK_ARCHIVE_SHA256}" "/tmp/melo-sdk-source-archive" | sha256sum -c -; \
            fi; \
            if printf '%s' "${MELO_SDK_SOURCE_URL}" | grep -Eq '\.zip$'; then \
                unzip -q /tmp/melo-sdk-source-archive -d /tmp/melo-sdk-extract; \
            else \
                tar -xf /tmp/melo-sdk-source-archive -C /tmp/melo-sdk-extract; \
            fi; \
            cp -a /tmp/melo-sdk-extract/. /opt/melo-sdk-source/; \
        fi; \
    else \
        if [ -d /app/core-services/text-to-speech/meloTTS/melo_sdk/src/npu/rpc/tts_wrapper ]; then \
            echo "[build] Using bundled melo_sdk directory from build context"; \
            cp -a /app/core-services/text-to-speech/meloTTS/melo_sdk /opt/melo-sdk-source/melo_sdk; \
        else \
            echo "[build] Using bundled fallback archive: 1.1.1.0.zip"; \
            if [ ! -f /app/core-services/text-to-speech/meloTTS/1.1.1.0.zip ]; then \
                echo "[build] ERROR: neither melo_sdk directory nor fallback archive 1.1.1.0.zip found in build context."; \
                echo "[build] Provide --build-arg MELO_SDK_SOURCE_URL=<git-or-archive-url> or add melo_sdk/ (or 1.1.1.0.zip)."; \
                exit 1; \
            fi; \
            unzip -q /app/core-services/text-to-speech/meloTTS/1.1.1.0.zip -d /tmp/melo-sdk-extract; \
            cp -a /tmp/melo-sdk-extract/. /opt/melo-sdk-source/; \
        fi; \
    fi; \
    SDK_CANDIDATE=""; \
    if [ -d "/opt/melo-sdk-source/${MELO_SDK_SUBDIR}/src/npu/rpc/tts_wrapper" ]; then \
        SDK_CANDIDATE="/opt/melo-sdk-source/${MELO_SDK_SUBDIR}"; \
    elif [ -d "/opt/melo-sdk-source/src/npu/rpc/tts_wrapper" ]; then \
        SDK_CANDIDATE="/opt/melo-sdk-source"; \
    else \
        SDK_CANDIDATE="$(find /opt/melo-sdk-source -maxdepth 6 -type d -name melo_sdk | head -n1 || true)"; \
    fi; \
    if [ -z "${SDK_CANDIDATE}" ] || [ ! -d "${SDK_CANDIDATE}/src/npu/rpc/tts_wrapper" ]; then \
        echo "[build] Could not locate Melo SDK root with src/npu/rpc/tts_wrapper"; \
        find /opt/melo-sdk-source -maxdepth 3 -type d | sed 's#^#  - #' ; \
        exit 1; \
    fi; \
    cp -a "${SDK_CANDIDATE}" /opt/melo-sdk; \
    echo "[build] Melo SDK root: /opt/melo-sdk"

RUN mkdir -p /opt/hexagon-sdk/ipc/fastrpc/remote/ship/UbuntuARM_aarch64 && \
    mkdir -p /opt/hexagon-sdk/ipc/fastrpc/rpcmem/inc && \
    mkdir -p /opt/hexagon-sdk/incs/stddef && \
    cp /usr/include/remote.h /opt/hexagon-sdk/incs/remote.h && \
    cp /usr/include/rpcmem.h /opt/hexagon-sdk/ipc/fastrpc/rpcmem/inc/rpcmem.h && \
    cp /usr/include/AEEStdErr.h /opt/hexagon-sdk/incs/AEEStdErr.h && \
    ln -sf /usr/lib/aarch64-linux-gnu/libcdsprpc.so.1.0.0 /opt/hexagon-sdk/ipc/fastrpc/remote/ship/UbuntuARM_aarch64/libcdsprpc.so
#the last step is silently failing???
RUN cmake -S /opt/melo-sdk/src/npu/rpc/tts_wrapper \
          -B /tmp/libtts-build \
          -DLINUX=1 \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_CXX_FLAGS="-DMAX_DOMAIN_URI_SIZE=32" \
          -DHEXAGON_SDK_ROOT=/opt/hexagon-sdk && \
    cmake --build /tmp/libtts-build --parallel "$(nproc)" && \
    install -D /tmp/libtts-build/libtts.so /opt/melo-sdk/libs/npu/rpc_libraries/linux/aarch64/libtts.so

RUN mkdir -p /app/core-services/text-to-speech/meloTTS/src/third_party && \
    cp /opt/headers/httplib/httplib.h /app/core-services/text-to-speech/meloTTS/src/third_party/httplib.h

RUN cmake -S /app/core-services/text-to-speech/meloTTS/src \
          -B /tmp/tts-service-build \
          -DCMAKE_BUILD_TYPE=Release \
          -DMELO_SDK_ROOT=/opt/melo-sdk && \
    cmake --build /tmp/tts-service-build --parallel "$(nproc)" && \
    install -D /tmp/tts-service-build/tts-service /usr/local/bin/tts-service && \
    strip --strip-unneeded /usr/local/bin/tts-service || true

FROM ubuntu-runtime:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

WORKDIR /app

COPY --from=build /usr/local/bin/tts-service /usr/bin/tts-service
COPY --from=build /opt/melo-sdk/libs/npu/rpc_libraries/linux/aarch64/libtts.so /usr/lib/libtts.so
COPY run.sh /app/run.sh

RUN chmod +x /app/run.sh && ldconfig

CMD ["/bin/bash", "/app/run.sh", "--model-path", "/opt/TTS_binary/MeloTTS", "--language", "English", "--port", "8083"]
