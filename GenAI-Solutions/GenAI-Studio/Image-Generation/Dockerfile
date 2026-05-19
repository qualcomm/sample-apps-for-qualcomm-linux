# ---------------------------------------------------------------------
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------

FROM --platform=arm64 ubuntu:24.04

# Define Build Arguments
ARG DSP_ARCH=v73
ARG QAIRT_VER=2.43.0.260128
ENV DSP_ARCH=${DSP_ARCH}
ENV QAIRT_VER=${QAIRT_VER}

# Update the package list
RUN apt-get update && \
    apt-get clean && \
    apt-get install -y \
        cmake make gcc g++ wget unzip git && \
    apt-get install -y software-properties-common && \
    apt-add-repository -s ppa:ubuntu-qcom-iot/qcom-ppa && \
    apt install -y qcom-fastrpc1 qcom-libdmabufheap-dev qcom-fastrpc-dev qcom-dspservices-headers-dev && \
    rm -rf /var/lib/apt/lists/*

RUN mkdir -p /app/Image-Generation/
WORKDIR /app/Image-Generation/
RUN git clone https://github.com/quic/ai-engine-direct-helper.git --recursive && \
    cd ai-engine-direct-helper && \
    git pull --recurse-submodules && \
    git checkout d3f02dd63be6868634574086cf97cfb89380f8aa

ARG DSP_ARCH
ARG QAIRT_VER
COPY stable_diffusion_v1_5.py /app/Image-Generation/ai-engine-direct-helper/samples/python/stable_diffusion_v1_5/stable_diffusion_v1_5.py

COPY requirements.txt /app/Image-Generation/ai-engine-direct-helper/
WORKDIR /app/Image-Generation/ai-engine-direct-helper

RUN wget https://repo.anaconda.com/miniconda/Miniconda3-latest-Linux-aarch64.sh && \
    bash Miniconda3-latest-Linux-aarch64.sh -b && \
    rm Miniconda3-latest-Linux-aarch64.sh

RUN . ~/miniconda3/bin/activate && \
    conda tos accept && \
    conda create -n py312 python=3.12 && \
    conda activate py312 && \
    pip install -r requirements.txt

RUN wget https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/${QAIRT_VER}/v${QAIRT_VER}.zip && \
    unzip v${QAIRT_VER}.zip && chmod +x qairt/${QAIRT_VER}/bin/aarch64-oe-linux-gcc11.2/* && \
    export QNN_SDK_ROOT=/app/Image-Generation/ai-engine-direct-helper/qairt/${QAIRT_VER}/ && \
    if [ "$DSP_ARCH" = "v75" ]; then \
       export QAI_HEXAGONARCH="75"; \
    else \
       export QAI_HEXAGONARCH="73"; \
    fi && \
    QAIRT_SHORT=$(echo ${QAIRT_VER} | cut -d'.' -f1-3) && \
    . ~/miniconda3/bin/activate && \
    conda activate py312 && \
    python setup.py bdist_wheel && \
    pip install dist/qai_appbuilder-${QAIRT_SHORT}.${QAI_HEXAGONARCH}-cp312-cp312-linux_aarch64.whl && \
    mkdir -p /app/Image-Generation/ai-engine-direct-helper/samples/python/qai_libs/ && \
    if [ "$DSP_ARCH" = "v75" ]; then \
        cp qairt/${QAIRT_VER}/lib/hexagon-v75/unsigned/* /app/Image-Generation/ai-engine-direct-helper/samples/python/qai_libs/; \
    else \
        cp qairt/${QAIRT_VER}/lib/hexagon-v73/unsigned/* /app/Image-Generation/ai-engine-direct-helper/samples/python/qai_libs/; \
    fi && \
    cp qairt/${QAIRT_VER}/lib/aarch64-oe-linux-gcc11.2/* /app/Image-Generation/ai-engine-direct-helper/samples/python/qai_libs/ && \
    rm -rf v${QAIRT_VER}.zip qairt

RUN mv /root/miniconda3/envs/py312/lib/libstdc++.so.6 /root/miniconda3/envs/py312/lib/libstdc++.so.6.bak
WORKDIR /app/Image-Generation/ai-engine-direct-helper/samples/python
COPY run.sh /app/Image-Generation/ai-engine-direct-helper/samples/python/
RUN chmod +x run.sh