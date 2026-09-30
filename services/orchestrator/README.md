# Orchestrator

## Live Mac microphone to Unreal (no RDP recording device required)

See [the live microphone guide](tools/LIVE_MIC_CLIENT.md) for architecture,
Unreal observer setup, networking assumptions, exact tests and validation limits.
The source checkout's Mac client needs only NumPy, sounddevice/PortAudio, SoXR,
and websockets; it does not need provider API keys or the server packages.

On macOS, from this `services/orchestrator` directory (Python 3.11+ recommended):

```bash
python3 -m venv .venv-mic
source .venv-mic/bin/activate
python3 -m pip install --upgrade pip
python3 -m pip install -r tools/live_mic_requirements.txt
python3 tools/live_mic_client.py --list-devices
python3 tools/live_mic_client.py --url ws://BACKEND_HOST:8080/ws/session --unreal-channel mac-mic --output live-mic-response.wav
```

Replace `BACKEND_HOST` with an address reachable from the Mac. Connect Unreal's
existing bridge to `ws://127.0.0.1:8080/ws/session?observe=mac-mic` **first** if
the backend runs on the same Windows machine. Use the microphone test actor in
Microphone Mode with Auto Start enabled (connect only); do not press its local
microphone or WAV buttons on this observer connection. Enter starts/stops the
Mac recording. The client exits after one response; reconnect the actor before
each new CLI invocation to reset its displayed transcript/response counters.
Without `--unreal-channel`, responses go only to the Mac socket and WAV file.

`pip install sounddevice` supplies PortAudio on macOS. If PortAudio cannot load:

```bash
brew install portaudio
python3 -m pip install --force-reinstall sounddevice
```

Allow microphone access for Terminal/iTerm/your Python launcher in macOS
**System Settings > Privacy & Security > Microphone**. Capture uses the default
input and its native sample rate, continuously resamples to mono 16 kHz PCM16LE,
and sends the existing 640-byte ACE1 payloads. `--device` selects another input;
`--channels 2` captures stereo and averages it to mono.

The backend must be reachable on TCP 8080; a loopback bind is only reachable
locally or through a tunnel. For direct access, run on Windows from this directory:

```powershell
.\.venv\Scripts\python.exe -m uvicorn app.main:app --host 0.0.0.0 --port 8080 --workers 1
```

The mirror is process-local and requires **one worker**. Use your private
network/tunnel or restrict firewall/security-group access to the Mac; the existing
endpoint has no authentication and channel names are routing labels, not secrets.
No firewall, EC2 security-group, RDP, or OS settings are changed by this feature.
The ACE1 header and JSON events remain unchanged; only optional `publish` and
`observe` URL parameters enable response mirroring. Normal sessions and the
existing Unreal local microphone path are unchanged.

## Windows hosted backend (English default)

Use Python 3.11. Run these PowerShell commands from the repository root:

```powershell
py -3.11 -m venv services/orchestrator/.venv
.\services\orchestrator\.venv\Scripts\python.exe -m pip install -e .\services\orchestrator
# Only on a fresh checkout, when .env does not already exist:
Copy-Item services/orchestrator/.env.example services/orchestrator/.env
.\services\orchestrator\.venv\Scripts\python.exe services/orchestrator/tools/init_storage.py
```

Set `ACE_NIM_API_KEY` in the ignored `services/orchestrator/.env`. The example
uses hosted NVIDIA ASR over TLS/gRPC, hosted NVIDIA LLM over HTTP/SSE, and
hosted Magpie TTS over HTTP. `ACE_ASR_API_KEY` can override the shared key.
No local models or Docker services are needed for this configuration.

Start from the repository root (the app loads its own `.env` regardless of cwd):

```powershell
.\services\orchestrator\.venv\Scripts\python.exe -m uvicorn app.main:app --host 127.0.0.1 --port 8080
```

In another PowerShell terminal, run a real audio turn using the existing client:

```powershell
.\services\orchestrator\.venv\Scripts\python.exe services/orchestrator/tools/demo_client.py --generate-input --require-real --output services/orchestrator/outputs/english-e2e.wav
```

This synthesizes an English **input** WAV, sends it as microphone PCM frames
to `ws://127.0.0.1:8080/ws/session`, and saves the **response** WAV plus a JSON
summary. It fails on server errors, timeout, missing stages, or mock adapters.
To use recorded speech instead, replace `--generate-input` with
`--wav path/to/16khz-mono-pcm16.wav`. This does not capture or play a physical
microphone/speaker on the AWS host.

`GET /healthz` checks process liveness. `GET /status` verifies the LLM model and
TTS voice; ASR status checks configuration, while the audio test verifies ASR
connectivity. Hosted ASR currently buffers an utterance and calls offline
recognition after VAD or `mic.end`; it does not produce live partial transcripts.
LLM output streams, and each completed sentence is synthesized and chunked
into PCM frames. Session locale is metadata, not a per-session model override.

Language configuration:

| Variable | Default / purpose |
| --- | --- |
| `ASSISTANT_LANGUAGE` | `en-US`; selects prompt and speech-language defaults |
| `ASR_LANGUAGE` | Overrides ASR language; legacy `ACE_ASR_LANGUAGE_CODE` also works |
| `TTS_LANGUAGE` | Overrides TTS language; legacy `ACE_TTS_LANGUAGE_CODE` also works |
| `ACE_TTS_VOICE` | English: `Magpie-Multilingual.EN-US.Aria` |
| `ACE_SYSTEM_PROMPT` | Blank uses the selected language's default prompt |
| `ACE_NIM_API_KEY` | Required NVIDIA credential; stored only in `.env` |
| `ACE_ASR_API_KEY` | Optional separate ASR credential |
| `ACE_ASR_SERVER`, `ACE_ASR_FUNCTION_ID`, `ACE_ASR_USE_SSL`, `ACE_ASR_USE_HOSTED_API` | Hosted ASR routing, as in `.env.example` |
| `ACE_NIM_LLM_BASE_URL`, `ACE_NIM_LLM_MODEL` | Hosted LLM endpoint/model |
| `ACE_TTS_API_URL`, `ACE_TTS_USE_HOSTED_API` | Hosted TTS routing |
| `ACE_TTS_SAMPLE_RATE_HZ` | `44100`; client reads actual frame rate |
| `ACE_LOG_DIR`, `ACE_AUDIO_DIR` | Optional storage overrides; defaults are module-relative `runtime` directories |

Other languages require an explicit voice from the provider's voice list and an
appropriate system prompt. Restart the service after configuration changes.

## Verification

From the repository root, with dependencies installed:

```bash
python3 -m pip install -r services/orchestrator/tools/live_mic_requirements.txt
cd services/orchestrator
python3 -m unittest discover -s tests
cd ../..
python3 -m pip check
```

## WebSocket protocol

1. Send `session.start`.
2. Send mono 16 kHz PCM16 microphone frames every 20 ms.
3. Send `mic.end` when the utterance is complete.
4. Receive transcript, LLM, state, and TTS events plus binary response audio.

Binary audio uses a 32-byte `ACE1` header. See
[architecture and protocol](../../docs/architecture/ace-sandbox.md) for details.

Set `ACE_MOCK_ASR=true`, `ACE_MOCK_LLM=true`, and `ACE_MOCK_TTS=true` to test
session connectivity without external providers. Mock tests do not validate
provider connectivity or MetaHuman animation.
