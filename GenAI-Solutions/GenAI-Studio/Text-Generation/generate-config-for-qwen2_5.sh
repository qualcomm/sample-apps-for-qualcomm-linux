#!/bin/sh
# =============================================================================
# QGenie Setup: Qwen2.5-7B-Instruct (On-Device)
# =============================================================================

set -e

# --- Hardcoded Model Specs ---
MODEL_BASE="qwen2_5_7b_instruct"
HF_REPO="Qwen/Qwen2.5-7B-Instruct"
VOCAB_SIZE=152064
BOS_TOKEN=151643
EOS_TOKEN=151645
ROPE_THETA=1000000.0

# --- User Input ---
NUM_PARTS=${1:-6}

die() { echo "Error: $1" >&2; exit 1; }

# =============================================================================
# 1. Hardware Detection (Machine String + SOC ID)
# =============================================================================
echo "Detecting Hardware..."

soc_name=$(cat /sys/devices/soc0/machine)

case "$soc_name" in
    # --- IoT / Auto ---
    QCS6490|QCM6490|SC7280|QCS5430)
        HTP_SOC_MODEL=93;  DSP_ARCH="v68"; DEV="$soc_name (qcm6490 family)" ;;
    SA8295P)
        HTP_SOC_MODEL=39;  DSP_ARCH="v68"; DEV="SA8295P ADP" ;;
    SA8775P|QCS9075|QCS9100)
        HTP_SOC_MODEL=52;  DSP_ARCH="v73"; DEV="$soc_name (qcs9100 family)" ;;
    QCS8300|QCS8275|SA7255P)
        HTP_SOC_MODEL=67;  DSP_ARCH="v75"; DEV="$soc_name (qcs8300 family)" ;;
    QCS8550)
        HTP_SOC_MODEL=43;  DSP_ARCH="v73"; DEV="QCS8550" ;;
    QCS8450)
        HTP_SOC_MODEL=36;  DSP_ARCH="v69"; DEV="QCS8450 / XR2 Gen 2" ;;

    *)
        die "Unsupported machine: '$soc_name'. Please add mapping to script." ;;
esac

echo "Detected : $DEV"
echo "   -> Model : $HTP_SOC_MODEL"
echo "   -> DSP Arch       : $DSP_ARCH"

# =============================================================================
# 2. Generate htp_backend_ext_config.json
# =============================================================================
echo " Writing htp_backend_ext_config.json..."

cat > "htp_backend_ext_config.json" <<EOF
{
    "devices": [
        {
            "device_id": 0,
            "soc_id": $HTP_SOC_MODEL,
            "dsp_arch": "$DSP_ARCH",
            "cores": [{ "core_id": 0, "perf_profile": "burst", "rpc_control_latency": 100 }]
        }
    ],
    "memory": { "mem_type": "shared_buffer" }
}
EOF

# =============================================================================
# 3. Generate genie_config.json
# =============================================================================
echo "Writing genie_config.json..."

# Build part list for multi-part binaries
BIN_LIST=""
i=1
while [ $i -le "$NUM_PARTS" ]; do
    BIN_LIST="${BIN_LIST}                \"${MODEL_BASE}_part_${i}_of_${NUM_PARTS}.bin\""
    [ $i -lt "$NUM_PARTS" ] && BIN_LIST="${BIN_LIST},\n"
    i=$((i + 1))
done

cat > "genie_config.json" <<EOF
{
    "dialog": {
        "version": 1,
        "type": "basic",
        "context": {
            "version": 1,
            "size": 4096,
            "n-vocab": $VOCAB_SIZE,
            "bos-token": $BOS_TOKEN,
            "eos-token": $EOS_TOKEN
        },
        "sampler": {
            "version": 1,
            "seed": 42,
            "temp": 0.8,
            "top-k": 40,
            "top-p": 0.95
        },
        "tokenizer": {
            "version": 1,
            "path": "tokenizer.json"
        },
        "engine": {
            "version": 1,
            "n-threads": 3,
            "backend": {
                "version": 1,
                "type": "QnnHtp",
                "QnnHtp": {
                    "version": 1,
                    "use-mmap": true,
                    "spill-fill-bufsize": 0,
                    "mmap-budget": 0,
                    "poll": true,
                    "cpu-mask": "0xe0",
                    "kv-dim": 128,
                    "pos-id-dim": 64,
                    "rope-theta": 1000000,
                    "allow-async-init": true
                },
                "extensions": "htp_backend_ext_config.json"
            },
            "model": {
                "version": 1,
                "type": "binary",
                "binary": {
                    "version": 1,
                    "ctx-bins": [
$(printf "$BIN_LIST")
                    ]
                }
            }
        }
    }
}
EOF

# =============================================================================
# 4. Download Tokenizer
# =============================================================================
echo "Downloading tokenizer.json from HuggingFace..."
URL="https://huggingface.co/$HF_REPO/raw/main/tokenizer.json"

if command -v curl > /dev/null; then
    curl -L "$URL" -o "tokenizer.json"
elif command -v wget > /dev/null; then
    wget "$URL" -O "tokenizer.json"
else
    echo "⚠️  Warning: Neither curl nor wget found. Please manually download tokenizer.json"
fi

echo "Done! Configs generated"
