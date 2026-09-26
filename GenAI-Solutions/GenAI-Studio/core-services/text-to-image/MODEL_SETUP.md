# Image-Generation Model Setup (Stable Diffusion 2.1)

This guide prepares runtime assets for `text-to-image:latest`.

Download and tokenizer preparation can run on a host or target machine.
The resulting model directory must end up on the target before build or run.

## 0) Scripted setup (recommended)

Run on target device from repo root:

```bash
bash scripts/phases/model_gen.sh --service t2i
```

Force refresh:

```bash
bash scripts/phases/model_gen.sh --service t2i --force-download
```

## 1) Download & Push artifacts

Download SD2.1 QNN context bundle for your target:

```bash
wget https://qaihub-public-assets.s3.us-west-2.amazonaws.com/qai-hub-models/models/stable_diffusion_v2_1/releases/v0.50.2/stable_diffusion_v2_1-qnn_context_binary-w8a16-qualcomm_qcs9075.zip
```
### 1.1 Choose model directory

Recommended model dir:

- `/opt/genai-studio-models/text-to-image/`

```bash
unzip  stable_diffusion_v2_1-qnn_context_binary-w8a16-qualcomm_qcs9075.zip -d /opt/genai-studio-models/text-to-image/

pushd /opt/genai-studio-models/text-to-image/stable_diffusion_v2_1-qnn_context_binary-w8a16-qualcomm_qcs9075


mkdir tokenizer
cd tokenizer/

wget https://huggingface.co/sd-research/stable-diffusion-2-1-base/resolve/main/tokenizer/merges.txt
wget https://huggingface.co/sd-research/stable-diffusion-2-1-base/resolve/main/tokenizer/vocab.json
cd ..

ls -la

export IMAGEGEN_MODEL_DIR=/opt/genai-studio-models/text-to-image/stable_diffusion_v2_1-qnn_context_binary-w8a16-qualcomm_qcs9075/

popd

```

If you downloaded and unpacked on another machine, copy the prepared directory
to the target before continuing:

```bash
rsync -av /path/to/stable_diffusion_v2_1-qnn_context_binary-w8a16-qualcomm_qcs9075/ \
  ubuntu@<target-host>:/opt/genai-studio-models/text-to-image/stable_diffusion_v2_1-qnn_context_binary-w8a16-qualcomm_qcs9075/
```

### 1.2: Required files in `IMAGEGEN_MODEL_DIR`

- `text_encoder.bin`
- `unet.bin`
- `vae.bin`
- `tokenizer/vocab.json`
- `tokenizer/merges.txt`

## 4) QAIRT flat runtime libraries

Prepare flat library folder:
- You can check this from [Device_Setup Section 6](../../docs/setup/DEVICE_SETUP.md#6-build-qairt-flat-lib-bundle-required-by-compose)

- `/opt/qairt/current/qairt_245_flat_libs`

Required minimum:

- `libQnnHtp.so`
- `libQnnSystem.so`
- `libQnnHtpV73Skel.so`
- `libqnnhtpv73.cat`

## 5) Validate before build/run

```bash
export IMAGEGEN_MODEL_DIR=/opt/genai-studio-models/text-to-image/stable_diffusion_v2_1-qnn_context_binary-w8a16-qualcomm_qcs9075/
export QAIRT_FLAT=/opt/qairt/current/qairt_245_flat_libs

ls -lah "${IMAGEGEN_MODEL_DIR}"
for f in text_encoder.bin unet.bin vae.bin; do
  test -f "${IMAGEGEN_MODEL_DIR}/${f}" && echo "${f} OK"
done
test -f "${IMAGEGEN_MODEL_DIR}/tokenizer/vocab.json" && echo "tokenizer vocab OK"
test -f "${IMAGEGEN_MODEL_DIR}/tokenizer/merges.txt" && echo "tokenizer merges OK"

for f in libQnnHtp.so libQnnSystem.so libQnnHtpV73Skel.so libqnnhtpv73.cat; do
  test -f "${QAIRT_FLAT}/${f}" && echo "${f} OK"
done
```

## 6) Next step

Continue with `core-services/text-to-image/README.md` for build/run/test commands.
