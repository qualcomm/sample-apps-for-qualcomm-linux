# --------------------------------------------------------------------
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------

# Use the official Ubuntu 24.04 image for ARM64 as the base image
FROM --platform=arm64 ubuntu:24.04

# Define the Build Argument (defaulting to v73)
ARG DSP_ARCH=v73
ARG QAIRT_VER=2.42.0.251225
# Install system dependencies
RUN apt-get update && \
    apt-get install -y \
        cmake make gcc g++ wget unzip git nlohmann-json3-dev \
        software-properties-common && \
    apt-add-repository -s ppa:ubuntu-qcom-iot/qcom-ppa && \
    apt install -y qcom-fastrpc1 qcom-libdmabufheap-dev qcom-fastrpc-dev qcom-dspservices-headers-dev && \
    rm -rf /var/lib/apt/lists/*

# Create working directory
WORKDIR /app/Text-Generation/

# Copy source files
COPY src/ ./src/

# Move into source directory
WORKDIR /app/Text-Generation/src/

RUN wget https://github.com/CrowCpp/Crow/releases/download/v1.2.1.2/Crow-1.2.1-Linux.tar.gz && \
    tar -xzf Crow-1.2.1-Linux.tar.gz && mv Crow-1.2.1-Linux crow && \
    git clone https://github.com/chriskohlhoff/asio.git && \
    # Logic to set QAIRT Version and Folder based on DSP_ARCH
    if [ "$DSP_ARCH" = "v75" ]; then \
        HEX_FOLDER="hexagon-v75"; \
    else \
        HEX_FOLDER="hexagon-v73"; \
    fi && \
    echo "Building for $DSP_ARCH" && \
    wget https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/${QAIRT_VER}/v${QAIRT_VER}.zip && \
    unzip v${QAIRT_VER}.zip && \
    mkdir -p build && cd build && cmake .. -DQAIRT_VER=${QAIRT_VER} && make -j8 && \
    cp chat_app /usr/bin/ && chmod +x /usr/bin/chat_app && \
    mkdir -p /usr/lib/rfsa/adsp && \
    # Use variables to copy the correct files
    cp /app/Text-Generation/src/qairt/${QAIRT_VER}/lib/${HEX_FOLDER}/unsigned/* /usr/lib/rfsa/adsp/ && \
    cp /app/Text-Generation/src/qairt/${QAIRT_VER}/lib/aarch64-oe-linux-gcc11.2/* /usr/lib/ && \
    chmod +x /app/Text-Generation/src/qairt/${QAIRT_VER}/bin/aarch64-oe-linux-gcc11.2/* && \
    cp /app/Text-Generation/src/qairt/${QAIRT_VER}/bin/aarch64-oe-linux-gcc11.2/* /usr/bin/ && \
    cd /app/ && rm -rf Text-Generation

# Clean up source directory
WORKDIR /app/

# Copy and prepare run script
COPY run.sh .
RUN chmod +x run.sh
