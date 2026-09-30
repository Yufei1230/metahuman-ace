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

For Japanese, set all three language variables to `ja-JP`, set
`ACE_TTS_VOICE=Magpie-Multilingual.JA-JP.Louise`, and leave `ACE_SYSTEM_PROMPT`
blank. Remove old explicit language/prompt overrides when switching languages.
Restart the service after configuration changes. Both listed voices were returned
by the hosted server's `/v1/audio/list_voices` during Windows verification.
Other languages require a voice from the server's actual list.

Run tests from the repository root:

```powershell
.\services\orchestrator\.venv\Scripts\python.exe -m unittest discover -s services/orchestrator/tests
.\services\orchestrator\.venv\Scripts\python.exe -m pip check
```

The Linux instructions below describe the original deployment option.

FastAPI ベースの WebSocket オーケストレータです。役割は以下です。

- Unreal から `16kHz mono PCM16` を受ける
- VAD で EOS を検出する
- ASR をストリーミングし `partial/final transcript` を返す
- NVIDIA NIM API へ会話プロンプトを送り、streaming delta を返す
- 文単位に区切って TTS を streaming し、受信 PCM を Unreal へ返す

## Run

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -e .
cp .env.example .env
python3 tools/init_storage.py
uvicorn app.main:app --host 0.0.0.0 --port 8080
```

`.env.example` uses hosted NVIDIA NIM at `https://integrate.api.nvidia.com/v1`
with `nvidia/nemotron-3.5-lightning-30b-a3b`. Set `ACE_NIM_API_KEY` in `.env`.

## Control Frames

JSON text frame の envelope は以下です。

```json
{
  "type": "state",
  "session_id": "a79e3ab3-4fbf-4ffc-b0fa-ad16bb6f8139",
  "turn_id": "eb799a38-19f1-4eb5-ad54-96bdf9518efe",
  "timestamp": "2026-04-21T07:00:00Z",
  "payload": {
    "state": "SPEAKING"
  }
}
```

### Required Client Flow

1. `session.start` を送る
2. `kind=mic` の binary frame を 20ms ごとに送る
3. クライアント側で明示的に終端が分かる場合は `mic.end` を送る
4. `asr.partial` / `asr.final` / `llm.delta` / `tts.start` / binary `tts` / `tts.end` を受ける

## Binary Audio Frames

- magic: `ACE1`
- version: `1`
- kind: `1=mic`, `2=tts`
- codec: `1=PCM_S16LE`
- channels: `1`
- sample rate: big-endian uint32
- payload size: big-endian uint32
- turn id bytes: 16 bytes
- payload: raw PCM16

## Notes

- `ACE_MOCK_ASR=true`
- `ACE_MOCK_LLM=true`
- `ACE_MOCK_TTS=true`

を指定すると、外部サービスなしで WebSocket の疎通確認だけ先に進められます。

## Utilities

- `tools/init_storage.py`: initializes persistent directories under the service-local `runtime/` directory by default
- `tools/demo_client.py`: WAV もしくは疑似マイク音声で orchestrator と会話フローを検証する

## HTTP Endpoints

- `GET /healthz`: プロセス生存確認
- `GET /status`: ASR / TTS / LLM の接続状態とモック設定を返す
