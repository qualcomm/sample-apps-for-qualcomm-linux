
# Speech-to-Text 
## Call Flow
1. When the user provides audio input, the Web‑UI, Sends the audio payload to the ASR container via REST API.
2. The ASR service performs automatic speech recognition and returns transcribed text.
3. Once complete, the Web‑UI could Display the text directly (or) Forward it to the Text‑to‑Text backend for further generation.

![image_generation](readme_assets/asr_unified.png)

.
