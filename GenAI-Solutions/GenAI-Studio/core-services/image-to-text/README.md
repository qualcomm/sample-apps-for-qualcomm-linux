# Image-To-Text Service

Image-To-Text (I2T) runs on port `8080`.
Direct inference endpoint is **only** `POST /v1/responses`.

Canonical build/run/rebuild commands live in repo-root `README.md`:

1. `One Bring-Up Path`
2. `Strict clean rebuild cycle`
3. `Six-service functional tests`

## Service Prerequisites (Runtime)

- model bundle:
  `/opt/genai-studio-models/image-to-text/qwen2_5_vl_7b_instruct-genie-w4a16-qualcomm_qcs9075`
- QAIRT flat libs mount:
  `/opt/qairt/current/qairt_245_flat_libs`
- pinned image tag (compose/build):
  `image-to-text:responses-v1`

## Build and Start Service

Build Image

```bash
DOCKER_BUILDKIT=1 docker build --progress=plain -t image-to-text:responses-v1 core-services/image-to-text/
```

Export required environment variables:

```bash
export I2T_QAIRT_FLAT_LIB_DIR=/opt/qairt/current/qairt_245_flat_libs
export I2T_MODEL_HOST_DIR=/opt/genai-studio-models/image-to-text/qwen2_5_vl_7b_instruct-genie-w4a16-qualcomm_qcs9075
export IMAGE_TO_TEXT_IMAGE=image-to-text:responses-v1
```

Start the service:

```bash
docker compose up -d image-to-text
```
For full stack bring-up, refer to `README.md` [section 7) Start services with docker compose](#7-start-services-with-docker-compose). (Recommended)

## Quick Validation

Health:

```bash
curl -sS http://localhost:8080/health
test "$(curl -s -o /dev/null -w '%{http_code}' -X POST http://localhost:8080/v1/responses -H 'Content-Type: application/json' -d '{}')" = "400" && echo "/v1/responses route OK"
```

Expected response shape:

```json
{"status":"ok"}
```

## API Contract

- Direct I2T endpoint: `POST /v1/responses`.
- Request contract: OpenAI Responses `input[]`.
- Image input: `input[].content[].input_image.image_url`.
- Supported image sources: `https://` and `data:` only.
- Supported decoded formats: `jpeg/jpg`, `png`, `webp`, `gif` (first frame).
- `pixel_values_path` is rejected (`400`).
- Legacy direct endpoints are removed (`404`).

Orchestrator route for I2T inference:

- `POST /v1/responses` on `:8090` (same OpenAI Responses contract)

### Validation Examples

Direct `/v1/responses` text-only:

```bash
curl -sS -X POST http://localhost:8080/v1/responses \
  -H 'Content-Type: application/json' \
  -H 'X-Session-Id: i2t-demo' \
  -d '{
    "model":"qwen2.5-vl-7b-instruct",
    "input":[{"role":"user","content":[{"type":"input_text","text":"Say hello."}]}],
    "stream":false,
    "max_output_tokens":64
  }'
```

Expected response shape:

```json
{
  "id": "resp-...",
  "object": "response",
  "model": "qwen2.5-vl-7b-instruct",
  "output_text": "..."
}
```

Direct `/v1/responses` image+text (`data:` URL):

```bash
curl -sS -X POST http://localhost:8080/v1/responses \
  -H 'Content-Type: application/json' \
  -H 'X-Session-Id: i2t-demo' \
  -d '{
    "model":"qwen2.5-vl-7b-instruct",
    "input":[{
      "role":"user",
      "content":[
        {"type":"input_text","text":"Describe this image briefly."},
        {"type":"input_image","image_url":"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+tmn8AAAAASUVORK5CYII="}
      ]
    }],
    "stream":false,
    "max_output_tokens":96
  }'
```

Expected response shape:

```json
{
  "id": "resp-...",
  "object": "response",
  "output": [{"type":"message"}],
  "output_text": "..."
}
```

Orchestrator `/v1/responses` (same payload contract):

```bash
curl -sS -X POST http://localhost:8090/v1/responses \
  -H 'Content-Type: application/json' \
  -d '{
    "model":"qwen2.5-vl-7b-instruct",
    "input":[{"role":"user","content":[{"type":"input_text","text":"Describe this image."},{"type":"input_image","image_url":"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+tmn8AAAAASUVORK5CYII="}]}],
    "stream":false
  }'
```

Expected response shape:

```json
{
  "id": "resp-...",
  "model": "qwen2.5-vl-7b-instruct",
  "output_text": "..."
}
```

OpenAI Python package smoke (direct I2T endpoint):

```python
import base64
import io
from PIL import Image, ImageDraw
from openai import OpenAI

# Synthetic image: left half blue, right half yellow
img = Image.new("RGB", (220, 140), (0, 0, 255))
ImageDraw.Draw(img).rectangle((110, 0, 219, 139), fill=(255, 255, 0))
buf = io.BytesIO()
img.save(buf, format="PNG")
img_url = "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode("ascii")

client = OpenAI(base_url="http://127.0.0.1:8080/v1", api_key="dummy")
resp = client.responses.create(
    model="qwen2.5-vl-7b-instruct",
    input=[{
        "role": "user",
        "content": [
            {"type": "input_text", "text": "Does this image contain both blue and yellow regions? Answer only yes or no."},
            {"type": "input_image", "image_url": img_url},
        ],
    }],
)
print(resp.output_text)
```

## Common Errors

- `"'input' array is required"`
  - Payload is not OpenAI Responses format.
- `"'pixel_values_path' is not supported..."`
  - Old preprocess contract is no longer accepted.
- `"Only https:// and data: image_url sources are supported"`
  - `http://` or local path was sent in `image_url`.
- `"Image-To-Text is exposed only via POST /v1/responses..."`
  - Image/session payload was sent to `/v1/chat/completions` instead of `/v1/responses`.
- `"unsupported image format ..."`
  - Data URL content is not jpeg/png/webp/gif.
- `"image_url preprocessing failed: failed to launch python preprocessing command"`
  - `data:` URL is too large for command-line handoff to in-container preprocessor.
  - Prefer smaller images for `data:` URLs or use `https://` image URLs.

Expected error response shape (top failure):

```json
{
  "error": {
    "type": "session_conflict",
    "code": "session_conflict",
    "message": "Active session is different..."
  }
}
```

## Notes

- In-container Python preprocessing happens at `core-services/image-to-text/src/preprocess.py`.
- Runtime limits are configurable:
  - `I2T_MAX_IMAGE_BYTES` (default `20971520`)
  - `I2T_MAX_IMAGE_PIXELS` (default `40000000`)
- Session is controlled by `X-Session-Id` header; backend defaults to `__default__` when omitted.
- Cooperative release endpoint:
  - `POST /v1/session/release` (soft reset)
  - `POST /v1/session/release?unload=1` (unload worker process state for T2I arbitration)
  - unload mode returns `503 upstream_busy` while `/v1/responses` requests are in-flight.ou# Image-To-Text Service