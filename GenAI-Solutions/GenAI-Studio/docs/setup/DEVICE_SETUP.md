# Device Setup (Qualcomm Ubuntu 24.04 / QLI 1.x / QLI 2.0)

> This document is intended for **initial target device provisioning only**.

Ensure that all commands in this guide are executed on the **target device** that is being provisioned.

---

For a complete automated setup, run the following provisioning script:

```bash
bash scripts/phases/device-setup.sh
```
#### NOTE: Run with sudo on ubuntu.

For startup script flags, phase behavior, and customization options, see
[`../STARTUP_SCRIPT_GUIDE.md`](../STARTUP_SCRIPT_GUIDE.md).

---

For manual device setup or any debug follow the following steps.

Please follow the steps for your specific OS

- [Ubuntu](#ubuntu-24.04)
- [QLI 1.x](#qli-1x)
- [QLI 2.0](#qli-20)

## Ubuntu 24.04

### 0) Already-Provisioned Quick Verify

Run this first to verify your target device is already provisioned. If all checks pass, you may skip the remaining setup steps and proceed directly to [README.md](../../README.md) for the bring-up flow.

```bash
docker --version
docker compose version
docker buildx version
ls /etc/cdi/
ls -l /dev/fastrpc-cdsp
ls -ld /opt/genai-studio-models /opt/qairt /opt/qairt/current
```

### 1) Core Packages + Qualcomm Runtime + Docker

This section installs essential system utilities, Qualcomm runtime libraries, and Docker. The process is divided into three parts:

#### 1a) Update system and install core utilities

```bash
sudo apt-get update
sudo apt-get install -y curl ca-certificates gnupg lsb-release jq unzip rsync git
```

These tools are required for package management, downloading files, and system configuration.

### 1b) Add Qualcomm PPA and install Qualcomm runtime packages

```bash
if ! grep -Rqs 'ubuntu-qcom-iot/qcom-ppa' /etc/apt/sources.list /etc/apt/sources.list.d 2>/dev/null; then
  sudo add-apt-repository -y ppa:ubuntu-qcom-iot/qcom-ppa
fi
sudo apt-get update
sudo apt-get install -y \
  qcom-fastrpc1 qcom-libdmabufheap-dev qcom-fastrpc-dev qcom-dspservices-headers-dev \
  libqnn1 qnn-tools libsnpe1 snpe-tools
```

These packages provide:
- **qcom-fastrpc1**: FastRPC runtime for DSP communication
- **qcom-libdmabufheap-dev**: DMA buffer heap support for efficient memory sharing
- **qcom-fastrpc-dev**: FastRPC development headers
- **qcom-dspservices-headers-dev**: DSP service headers
- **libqnn1 & qnn-tools**: Qualcomm Neural Network runtime and tools
- **libsnpe1 & snpe-tools**: Snapdragon Neural Processing Engine

### 1c) Install Docker from official repository

```bash
sudo apt-get remove -y docker.io docker-doc docker-compose podman-docker containerd runc || true
sudo install -m 0755 -d /etc/apt/keyrings
curl -fsSL https://download.docker.com/linux/ubuntu/gpg | sudo gpg --dearmor -o /etc/apt/keyrings/docker.gpg
sudo chmod a+r /etc/apt/keyrings/docker.gpg
echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] https://download.docker.com/linux/ubuntu $(. /etc/os-release && echo "$VERSION_CODENAME") stable" | \
  sudo tee /etc/apt/sources.list.d/docker.list >/dev/null
sudo apt-get update
sudo apt-get install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin docker-compose
sudo systemctl enable --now docker
sudo usermod -aG docker "$USER"
```

This installs Docker from the official repository and configures it to:
- Start automatically on boot (`systemctl enable --now`)
- Allow your user to run Docker commands without `sudo` (`usermod -aG docker`)

**Important:** After running `usermod -aG docker "$USER"`, you must open a new login shell for the group changes to take effect. You can do this by logging out and back in, or running:

```bash
newgrp docker
```

### 2) Enable Docker CDI + Install Qualcomm CDI Spec

Container Device Interface (CDI) allows Docker containers to access hardware accelerators. This section enables CDI and installs the Qualcomm hardware acceleration specification.

### 2a) Enable CDI in Docker daemon

```bash
sudo install -m 0755 -d /etc/docker
printf '{\n  "features": {\n    "cdi": true\n  }\n}\n' | sudo tee /etc/docker/daemon.json >/dev/null

sudo install -d /etc/cdi
sudo cp "/opt/sample-apps-for-qualcomm-linux/GenAI-Solutions/GenAI-Studio/cdi/2.x/docker-run-cdi-hw-acc.json" /etc/cdi/docker-run-cdi-hw-acc.json # Copy the corresponding cdi file from the repository
```

**Finalize Setup**
```bash
sudo chmod 644 /etc/cdi/docker-run-cdi-hw-acc.json

sudo systemctl restart docker
grep -n 'qualcomm.com/device=cdi-hw-acc' /etc/cdi/docker-run-cdi-hw-acc.json
```

### 3) Validate DSP Runtime

```bash
qnn-platform-validator --backend dsp --testBackend || true
```

### 4) Create Standard Runtime Layout

```bash
sudo mkdir -p /opt/genai-studio-models/{text-to-text,speech-to-text,text-to-speech,image-to-text,text-to-image}
sudo mkdir -p /opt/genai-studio-cache/huggingface
sudo mkdir -p /opt/qairt
sudo chown -R "$USER":"$USER" /opt/genai-studio-models /opt/genai-studio-cache /opt/qairt
```

### 5) Install QAIRT (Skip if already present)

QAIRT (Qualcomm AI Runtime) is the core runtime for executing AI models on Qualcomm hardware. This step checks if it's already installed and installs it if needed.

### 5a) Check if QAIRT is already installed

```bash
if [ -d /opt/qairt/current/include/Genie ] && [ -d /opt/qairt/current/lib ]; then
  echo "QAIRT present at /opt/qairt/current (skip install)."
else
  echo "QAIRT missing. Run install block below."
fi
```

Install block (only if missing):

### 5b) Download and install QAIRT (only if missing)

```bash
[ -f ./versions.env ] && set -a && source ./versions.env && set +a
QAIRT_VER="${QAIRT_VERSION:-2.45.0.260326}"
QAIRT_ZIP=/tmp/v${QAIRT_VER}.zip
QAIRT_URL="https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/${QAIRT_VER}/v${QAIRT_VER}.zip"

curl -fL "$QAIRT_URL" -o "$QAIRT_ZIP"
TMP_UNZIP=$(mktemp -d)
unzip -q "$QAIRT_ZIP" -d "$TMP_UNZIP"
sudo mkdir -p "/opt/qairt/${QAIRT_VER}"
if [ -d "$TMP_UNZIP/qairt/${QAIRT_VER}" ]; then
  sudo rsync -a "$TMP_UNZIP/qairt/${QAIRT_VER}/" "/opt/qairt/${QAIRT_VER}/"
else
  sudo rsync -a "$TMP_UNZIP/" "/opt/qairt/${QAIRT_VER}/"
fi
rm -rf "$TMP_UNZIP"
sudo ln -sfn "/opt/qairt/${QAIRT_VER}" /opt/qairt/current
sudo ln -sfn /opt/qairt/current/bin /opt/qairt/bin
sudo ln -sfn /opt/qairt/current/include /opt/qairt/include
sudo ln -sfn /opt/qairt/current/lib /opt/qairt/lib
```

This script:
1. **Loads version from environment**: Checks for `versions.env` to allow version override
2. **Downloads QAIRT**: Fetches the specified version from Qualcomm's software center
3. **Extracts and installs**: Unzips the archive and copies files to `/opt/qairt/${QAIRT_VER}`
4. **Creates symlinks**: Sets up `/opt/qairt/current` to point to the installed version, and creates convenience symlinks for `bin`, `include`, and `lib` directories

The symlink approach allows easy version switching by updating the `/opt/qairt/current` link.

### 6) Build QAIRT Flat Lib Bundle (Required by Compose)

```bash
QAIRT_FLAT_DIR="${QAIRT_FLAT_LIB_DIR:-/opt/qairt/current/qairt_245_flat_libs}"
rm -rf "${QAIRT_FLAT_DIR}"
mkdir -p "${QAIRT_FLAT_DIR}"
cp -a /opt/qairt/current/lib/aarch64-oe-linux-gcc11.2/*.so* "${QAIRT_FLAT_DIR}/"
cp -a /opt/qairt/current/lib/hexagon-v73/unsigned/libQnnHtpV73*.so "${QAIRT_FLAT_DIR}/"
cp -a /opt/qairt/current/lib/hexagon-v73/unsigned/libqnnhtpv73.cat "${QAIRT_FLAT_DIR}/"
cp -a /opt/qairt/current/lib/hexagon-v73/unsigned/libsnpehtpv73.cat "${QAIRT_FLAT_DIR}/"
```

Do not bulk-copy `hexagon-v73/unsigned/*.so*` into flat dir (can cause ASR ELF class failures).
**Note**: For IQ8 (QCS8275) and Ventuno Q please use the appropriate htp version (v75).

### 7) Verify Host RPC Path 

```bash
# Detect FASTRPC shell location
unset FAST_RPC_SHELL_PATH

for d in /usr/lib/dsp /usr/lib/dsp/cdsp; do
  if [ -f "$d/fastrpc_shell_unsigned_3" ]; then
    export FAST_RPC_SHELL_PATH="$d/fastrpc_shell_unsigned_3"
    break
  fi
done

[ -n "$FAST_RPC_SHELL_PATH" ] || {
  echo "ERROR: fastrpc_shell_unsigned_3 not found"
  exit 1
}

echo "FAST_RPC_SHELL_PATH=$FAST_RPC_SHELL_PATH" > .env

cat .env

ls -ld /usr/lib/dsp /usr/lib/dsp/cdsp 2>/dev/null || true
ls -l /usr/lib/dsp/cdsp/fastrpc_shell_unsigned_3 2>/dev/null || true
```

### 8) Final Readiness

```bash
docker --version
docker compose version
docker buildx version
ls -ld /opt/genai-studio-models /opt/qairt /opt/qairt/current /opt/qairt/current/qairt_245_flat_libs
```

## QLI 1.x

### 0) Already-Provisioned Quick Verify

Run this first to verify your target device is already provisioned. If all checks pass, you may skip the remaining setup steps and proceed directly to [README.md](../../README.md) for the bring-up flow.

```bash
docker --version
docker-compose version
docker buildx version
ls /etc/cdi/
ls -l /dev/fastrpc-cdsp
ls -ld /opt/genai-studio-models /opt/qairt /opt/qairt/current
```

**Note:** If you encounter the error `docker: 'compose' is not a docker command` on QLI 1.x, it might be a SELinux permission issue. Run:

```bash
setenforce 0
getenforce
```
This should show `permissive` and then try again.

### 1) Create Standard Runtime Layout

```bash
mkdir -p /opt/genai-studio-models/{text-to-text,speech-to-text,text-to-speech,image-to-text,text-to-image}
mkdir -p /opt/genai-studio-cache/huggingface
mkdir -p /opt/qairt
chown -R "$USER":"$USER" /opt/genai-studio-models /opt/genai-studio-cache /opt/qairt
```

### 2) Install QAIRT (Skip if already present)

QAIRT (Qualcomm AI Runtime) is the core runtime for executing AI models on Qualcomm hardware. This step checks if it's already installed and installs it if needed.

### 2a) Check if QAIRT is already installed

```bash
if [ -d /opt/qairt/current/include/Genie ] && [ -d /opt/qairt/current/lib ]; then
  echo "QAIRT present at /opt/qairt/current (skip install)."
else
  echo "QAIRT missing. Run install block below."
fi
```

Install block (only if missing):

### 2b) Download and install QAIRT (only if missing)

```bash
[ -f ./versions.env ] && set -a && source ./versions.env && set +a
QAIRT_VER="${QAIRT_VERSION:-2.45.0.260326}"
QAIRT_ZIP=/tmp/v${QAIRT_VER}.zip
QAIRT_URL="https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/${QAIRT_VER}/v${QAIRT_VER}.zip"

curl -fL "$QAIRT_URL" -o "$QAIRT_ZIP"
TMP_UNZIP=$(mktemp -d)
unzip -q "$QAIRT_ZIP" -d "$TMP_UNZIP"
mkdir -p "/opt/qairt/${QAIRT_VER}"
if [ -d "$TMP_UNZIP/qairt/${QAIRT_VER}" ]; then
  rsync -a "$TMP_UNZIP/qairt/${QAIRT_VER}/" "/opt/qairt/${QAIRT_VER}/"
else
  rsync -a "$TMP_UNZIP/" "/opt/qairt/${QAIRT_VER}/"
fi
rm -rf "$TMP_UNZIP"
ln -sfn "/opt/qairt/${QAIRT_VER}" /opt/qairt/current
ln -sfn /opt/qairt/current/bin /opt/qairt/bin
ln -sfn /opt/qairt/current/include /opt/qairt/include
ln -sfn /opt/qairt/current/lib /opt/qairt/lib
```

This script:
1. **Loads version from environment**: Checks for `versions.env` to allow version override
2. **Downloads QAIRT**: Fetches the specified version from Qualcomm's software center
3. **Extracts and installs**: Unzips the archive and copies files to `/opt/qairt/${QAIRT_VER}`
4. **Creates symlinks**: Sets up `/opt/qairt/current` to point to the installed version, and creates convenience symlinks for `bin`, `include`, and `lib` directories

The symlink approach allows easy version switching by updating the `/opt/qairt/current` link.

### 3) Build QAIRT Flat Lib Bundle (Required by Compose)

```bash
QAIRT_FLAT_DIR="${QAIRT_FLAT_LIB_DIR:-/opt/qairt/current/qairt_245_flat_libs}"
rm -rf "${QAIRT_FLAT_DIR}"
mkdir -p "${QAIRT_FLAT_DIR}"
cp -a /opt/qairt/current/lib/aarch64-oe-linux-gcc11.2/*.so* "${QAIRT_FLAT_DIR}/"
cp -a /opt/qairt/current/lib/hexagon-v73/unsigned/libQnnHtpV73*.so "${QAIRT_FLAT_DIR}/"
cp -a /opt/qairt/current/lib/hexagon-v73/unsigned/libqnnhtpv73.cat "${QAIRT_FLAT_DIR}/"
cp -a /opt/qairt/current/lib/hexagon-v73/unsigned/libsnpehtpv73.cat "${QAIRT_FLAT_DIR}/"
```

Do not bulk-copy `hexagon-v73/unsigned/*.so*` into flat dir (can cause ASR ELF class failures).

### 4) Verify Host RPC Path 

```bash
# Detect FASTRPC shell location
unset FAST_RPC_SHELL_PATH

for d in /usr/lib/dsp /usr/lib/dsp/cdsp; do
  if [ -f "$d/fastrpc_shell_unsigned_3" ]; then
    export FAST_RPC_SHELL_PATH="$d/fastrpc_shell_unsigned_3"
    break
  fi
done

[ -n "$FAST_RPC_SHELL_PATH" ] || {
  echo "ERROR: fastrpc_shell_unsigned_3 not found"
  exit 1
}

echo "FAST_RPC_SHELL_PATH=$FAST_RPC_SHELL_PATH" > .env

cat .env

ls -ld /usr/lib/dsp /usr/lib/dsp/cdsp 2>/dev/null || true
ls -l /usr/lib/dsp/cdsp/fastrpc_shell_unsigned_3 2>/dev/null || true
```

## 5) Final Readiness

```bash
docker --version
docker compose version
docker buildx version
ls -ld /opt/genai-studio-models /opt/qairt /opt/qairt/current /opt/qairt/current/qairt_245_flat_libs
echo "${FAST_RPC_SHELL_PATH:-  FAST_RPC_SHELL_PATH not set in this shell}"
```

## QLI 2.0

### 0) Already-Provisioned Quick Verify

Run this first to verify your target device is already provisioned. If all checks pass, you may skip the remaining setup steps and proceed directly to [README.md](../../README.md) for the bring-up flow.

```bash
docker --version
docker compose version
docker buildx version
ls /etc/cdi/
ls -l /dev/fastrpc-cdsp
ls -ld /opt/genai-studio-models /opt/qairt /opt/qairt/current
```

**Note:** If you encounter the error `ERROR: BuildKit is enabled but the buildx component is missing or broken. Install the buildx component to build images with BuildKit: https://docs.docker.com/go/buildx/` manually install docker buildx with:

```bash
mkdir -p ~/.docker/cli-plugins

cd ~/.docker/cli-plugins

curl -L https://github.com/docker/buildx/releases/download/v0.14.1/buildx-v0.14.1.linux-arm64 \
  -o docker-buildx
chmod +x docker-buildx
docker buildx version

cd -
```

### 1) Enable Docker CDI + Install Qualcomm CDI Spec

Container Device Interface (CDI) allows Docker containers to access hardware accelerators. This section enables CDI and installs the Qualcomm hardware acceleration specification.

```bash
install -d /etc/cdi
cp "/opt/sample-apps-for-qualcomm-linux/GenAI-Solutions/GenAI-Studio/cdi/2.x/docker-run-cdi-hw-acc.json" /etc/cdi/docker-run-cdi-hw-acc.json # Copy the corresponding cdi file from the repository
```
**Finalize Setup**
```bash
chmod 644 /etc/cdi/docker-run-cdi-hw-acc.json

systemctl restart docker
grep -n 'qualcomm.com/device=cdi-hw-acc' /etc/cdi/docker-run-cdi-hw-acc.json
```

### 2) Validate DSP Runtime

```bash
qnn-platform-validator --backend dsp --testBackend || true
```

### 3) Create Standard Runtime Layout

```bash
mkdir -p /opt/genai-studio-models/{text-to-text,speech-to-text,text-to-speech,image-to-text,text-to-image}
mkdir -p /opt/genai-studio-cache/huggingface
mkdir -p /opt/qairt
chown -R "$USER":"$USER" /opt/genai-studio-models /opt/genai-studio-cache /opt/qairt
```

### 4) Install QAIRT (Skip if already present)

QAIRT (Qualcomm AI Runtime) is the core runtime for executing AI models on Qualcomm hardware. This step checks if it's already installed and installs it if needed.

### 4a) Check if QAIRT is already installed

```bash
if [ -d /opt/qairt/current/include/Genie ] && [ -d /opt/qairt/current/lib ]; then
  echo "QAIRT present at /opt/qairt/current (skip install)."
else
  echo "QAIRT missing. Run install block below."
fi
```

Install block (only if missing):

### 4b) Download and install QAIRT (only if missing)

```bash
[ -f ./versions.env ] && set -a && source ./versions.env && set +a
QAIRT_VER="${QAIRT_VERSION:-2.45.0.260326}"
QAIRT_ZIP=/tmp/v${QAIRT_VER}.zip
QAIRT_URL="https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/${QAIRT_VER}/v${QAIRT_VER}.zip"

curl -fL "$QAIRT_URL" -o "$QAIRT_ZIP"
TMP_UNZIP=$(mktemp -d)
unzip -q "$QAIRT_ZIP" -d "$TMP_UNZIP"
mkdir -p "/opt/qairt/${QAIRT_VER}"
if [ -d "$TMP_UNZIP/qairt/${QAIRT_VER}" ]; then
  rsync -a "$TMP_UNZIP/qairt/${QAIRT_VER}/" "/opt/qairt/${QAIRT_VER}/"
else
  rsync -a "$TMP_UNZIP/" "/opt/qairt/${QAIRT_VER}/"
fi
rm -rf "$TMP_UNZIP"
ln -sfn "/opt/qairt/${QAIRT_VER}" /opt/qairt/current
ln -sfn /opt/qairt/current/bin /opt/qairt/bin
ln -sfn /opt/qairt/current/include /opt/qairt/include
ln -sfn /opt/qairt/current/lib /opt/qairt/lib
```

This script:
1. **Loads version from environment**: Checks for `versions.env` to allow version override
2. **Downloads QAIRT**: Fetches the specified version from Qualcomm's software center
3. **Extracts and installs**: Unzips the archive and copies files to `/opt/qairt/${QAIRT_VER}`
4. **Creates symlinks**: Sets up `/opt/qairt/current` to point to the installed version, and creates convenience symlinks for `bin`, `include`, and `lib` directories

The symlink approach allows easy version switching by updating the `/opt/qairt/current` link.

### 5) Build QAIRT Flat Lib Bundle (Required by Compose)

```bash
QAIRT_FLAT_DIR="${QAIRT_FLAT_LIB_DIR:-/opt/qairt/current/qairt_245_flat_libs}"
rm -rf "${QAIRT_FLAT_DIR}"
mkdir -p "${QAIRT_FLAT_DIR}"
cp -a /opt/qairt/current/lib/aarch64-oe-linux-gcc11.2/*.so* "${QAIRT_FLAT_DIR}/"
cp -a /opt/qairt/current/lib/hexagon-v73/unsigned/libQnnHtpV73*.so "${QAIRT_FLAT_DIR}/"
cp -a /opt/qairt/current/lib/hexagon-v73/unsigned/libqnnhtpv73.cat "${QAIRT_FLAT_DIR}/"
cp -a /opt/qairt/current/lib/hexagon-v73/unsigned/libsnpehtpv73.cat "${QAIRT_FLAT_DIR}/"
```

Do not bulk-copy `hexagon-v73/unsigned/*.so*` into flat dir (can cause ASR ELF class failures).

### 6) Final Readiness

```bash
docker --version
docker compose version
docker buildx version
ls -ld /opt/genai-studio-models /opt/qairt /opt/qairt/current /opt/qairt/current/qairt_245_flat_libs
```

Next:

1. Root onboarding: [../../README.md](../../README.md)
2. Troubleshooting: [../TROUBLESHOOTING_GUIDE.md](../TROUBLESHOOTING_GUIDE.md)
