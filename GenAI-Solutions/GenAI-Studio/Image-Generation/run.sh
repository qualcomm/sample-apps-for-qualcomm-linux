# ---------------------------------------------------------------------
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------

#! /bin/bash
. ~/miniconda3/bin/activate
conda activate py312
export LD_LIBRARY_PATH=/app/Image-Generation/ai-engine-direct-helper/samples/python/qai_libs/
export ADSP_LIBRARY_PATH=/app/Image-Generation/ai-engine-direct-helper/samples/python/qai_libs/
export DSP_ARCH=${DSP_ARCH}
python3 stable_diffusion_v1_5/stable_diffusion_v1_5.py