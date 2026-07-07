# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear

#!/bin/bash

# Parse command line arguments for API token
for i in "$@"; do
  case $i in
    --api-token=*|--api-key=*)
      API_TOKEN="${i#*=}"
      shift
      ;;
    *)
      echo "Usage: $0 --api-token=your_api_token_here or --api-key=your_api_key_here"
      exit 1
      ;;
  esac
done

# Check if API token is provided
if [ -z "$API_TOKEN" ]; then
  echo "API token is required. Usage: $0 --api-token=your_api_token_here or --api-key=your_api_key_here"
  exit 1
fi

# Define the Miniconda installer URL
#MINICONDA_URL="https://repo.anaconda.com/miniconda/Miniconda3-latest-Linux-x86_64.sh"
MINICONDA_URL="https://repo.anaconda.com/miniconda/Miniconda3-py310_25.7.0-2-Linux-x86_64.sh"

# Define the installation directory
INSTALL_DIR="$PWD/miniconda3"

# Define the home directory for aihub assets
export HOME=$PWD

# Download the Miniconda installer
echo "Downloading Miniconda installer..."
wget $MINICONDA_URL -O miniconda.sh

# Make the installer executable
chmod +x miniconda.sh

# Run the installer
echo "Installing Miniconda..."
./miniconda.sh -b -p $INSTALL_DIR

# Initialize Conda
echo "Initializing Conda..."
$INSTALL_DIR/bin/conda init

# Clean up
rm miniconda.sh

# Activate Conda
echo "Activating Conda..."
source $INSTALL_DIR/bin/activate

conda tos accept --override-channels --channel https://repo.anaconda.com/pkgs/main
conda tos accept --override-channels --channel https://repo.anaconda.com/pkgs/r

# Create a Python 3.10 environment
echo "Creating Python 3.10 environment..."
$INSTALL_DIR/bin/conda create -y -n py310 python=3.10

# Activate the Python 3.10 environment
echo "Activating Python 3.10 environment..."
source $INSTALL_DIR/bin/activate py310

# Install the qai_hub package
echo "Installing qai_hub package..."
pip install python-git
pip install qai-hub
pip install "qai-hub-models[yolov8-det]"
pip install torch==2.8.0
pip install torchvision==0.23.0

# Configure qai_hub with API token
echo "Configuring qai_hub..."
qai-hub configure --api_token "$API_TOKEN"

# Export models
echo "Exporting YOLOv8 quantized model..."
python -m qai_hub_models.models.yolov8_det.export --quantize w8a8 --skip-profiling --skip-inferencing

echo "Miniconda installation, activation, Python 3.10 environment creation, and qai_hub package installation complete."
