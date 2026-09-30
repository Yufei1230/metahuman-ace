# Live Mac microphone client

## Architecture

```text
Mac default microphone (CoreAudio through sounddevice/PortAudio)
  -> native-rate float32 capture, bounded callback queue
  -> mono downmix, streaming SoXR HQ resampling, PCM16LE
  -> 20 ms / 640-byte ACE1 MIC frames
  -> /ws/session?publish=mac-mic
  -> existing ConversationSession -> ASR -> LLM -> TTS
       -> originating Mac socket -> event logs + local TTS WAV
       -> /ws/session?observe=mac-mic -> existing Unreal ACEConversation playback
```

The pre-existing server creates one independent conversation per socket. Merely
using the same `session_id` on two sockets does not share a session. The new
opt-in response mirror solves this routing gap without modifying any ACE1 fields,
control event schemas, or the existing conversation pipeline. Clients without
query parameters retain their original behavior. No provider credentials go to
the Mac. The current Windows microphone path remains intact.

`app.protocol` is imported directly from the source checkout. The client reuses
`pack_audio_frame`, `unpack_audio_frame`, and `serialize_control_message`;
it follows the demo client's 640-byte framing, final-frame padding, `session.start`,
`mic.end`, `LISTENING/ready`, and `LISTENING/turn_complete` behavior. The server
has no `llm.final` event: the CLI prints the accumulated deltas as the final response
at turn completion. The WAV is written on `tts.end` and preserves the ACE1 TTS
sample rate. The client validates mono PCM16, rejects format/turn changes, and
limits a response to 64 MiB. The selected output filename is overwritten on success.

## Mac setup and use

Copy/clone this source checkout to the Mac, including `services/orchestrator/app/protocol.py`.
Use Python 3.11 or newer. From the repository root:

```bash
cd services/orchestrator
python3 -m venv .venv-mic
source .venv-mic/bin/activate
python3 -m pip install --upgrade pip
python3 -m pip install -r tools/live_mic_requirements.txt
python3 tools/live_mic_client.py --list-devices
```

Dependencies: `websockets>=12,<16`, `numpy>=1.26,<3`,
`sounddevice>=0.5,<0.6`, `soxr>=0.5,<2` (sounddevice also installs CFFI).
No full orchestrator installation is required on the Mac.
The pip sounddevice installation normally includes PortAudio on macOS;
see [upstream installation instructions](https://github.com/spatialaudio/python-sounddevice/blob/master/doc/installation.rst).
If PortAudio fails to load, with Homebrew available:

```bash
brew install portaudio
python3 -m pip install --force-reinstall sounddevice
```

Allow microphone access for the launching terminal/application in macOS
System Settings > Privacy & Security > Microphone. Select the input in macOS
Sound settings or use `--device <index-or-name>`. Native-rate mono capture is
requested by default. `--channels 2` supports inputs that require stereo.
The resampler carries filter state across callback boundaries and flushes once,
as required by [SoXR's streaming API](https://python-soxr.readthedocs.io/en/stable/soxr.html).

First start the backend, then connect Unreal as described below. On the Mac:

```bash
python3 tools/live_mic_client.py \
  --url ws://BACKEND_HOST:8080/ws/session \
  --unreal-channel mac-mic \
  --output live-mic-response.wav
```

Replace `BACKEND_HOST` with the EC2/private/tunnel host accessible from the Mac.
Enter starts capture; Enter stops capture, flushes the resampler, pads the final
20 ms frame and sends `mic.end`. The default maximum is 30 seconds, configurable
up to 60 with `--max-seconds`. Response timeout defaults to 180 seconds.
This first version handles one spoken turn per invocation. Use headphones or
avoid recording while Unreal is speaking; this is half-duplex with no new AEC.

The backend's existing VAD remains active. Speak a short continuous sentence;
a pause may trigger server endpointing before Enter. The client stops capture
when the server enters THINKING/SPEAKING and discards remaining buffered capture.
Pure silence may never start a backend turn and will time out without a WAV.
There is no new client VAD or local transcription. Callback overflow or a stalled
capture fails explicitly; it does not silently drop PCM or claim success.
Ctrl+C, disconnects, backend errors and timeouts close capture and the socket.

To test the Mac socket/WAV path alone, omit `--unreal-channel`:

```bash
python3 tools/live_mic_client.py --url ws://BACKEND_HOST:8080/ws/session
```

To exercise exactly the same streaming/conversion/response code without hardware:

```bash
python3 tools/live_mic_client.py --url ws://BACKEND_HOST:8080/ws/session \
  --unreal-channel mac-mic --test-wav /path/to/pcm16-speech.wav \
  --output simulated-response.wav
```

`--test-wav` is explicitly labeled SIMULATED, paced in real time, and does not
open a microphone or prompt for Enter. It accepts uncompressed PCM16 WAV input
and converts the WAV's source rate/channels through the same converter.

## Backend and Unreal setup

The orchestrator's ASR input settings must remain **16000 Hz / 20 ms / mono PCM16**.
From `services/orchestrator` on Windows:

```powershell
.\.venv\Scripts\python.exe -m uvicorn app.main:app --host 0.0.0.0 --port 8080 --workers 1
```

The tests here used `--host 127.0.0.1`; direct Mac access requires a reachable bind
and TCP 8080 allowed through the host firewall and EC2 security group from your
Mac/network. Use a private network or existing tunnel where possible. No networking
rules were changed. `ws://` is plaintext and the existing endpoint has no auth;
`mac-mic` is a routing label, not an access token. Use `wss://` with an existing TLS
proxy if accessing over an untrusted network. If Windows already has SSH access
configured, an alternative is `ssh -N -L 8080:127.0.0.1:8080 USER@EC2_HOST`, leaving
the backend on loopback and using `ws://127.0.0.1:8080/ws/session` on the Mac.

In `/Game/MicrophoneConversationTest`, select the Conversation Audio Test Actor:

1. Set its Bridge's Server URL to `ws://127.0.0.1:8080/ws/session?observe=mac-mic`
   when the backend is on the same Windows host.
2. Enable Auto Start and Microphone Mode. This connects without opening the
   Windows microphone or sending the prerecorded WAV. Start PIE and wait for
   `[SessionReady]` / Ready **before launching the Mac publisher**.
3. Use only the Mac's Enter controls. Local Start Listening and WAV fallback
   send input and are not valid on a receive-only observer connection.
4. Observe `[Transcript]`, `[LLM]`, `[TTSAudio]`, `[PCMQueued]`, and
   `[PlaybackStarted]`. The actor stays alive after playback.
5. Before a new CLI invocation, use Disconnect/Connect or restart PIE to reset
   the test actor's displayed transcript/counters. The server observer itself
   can remain connected after a successfully completed publisher session.

Only one publisher is allowed per channel. Observers must connect first;
late joins are rejected to avoid playing truncated turns. No past audio is
replayed. Observer inputs other than the initial existing `session.start` are
rejected. Responses are mirrored byte-for-byte with the publisher's turn IDs.
Each observer has a bounded 256-message queue; falling behind disconnects it
instead of blocking the conversation or dropping audio unnoticed. Interrupted
publisher sessions close observers; reconnect them before retrying. There is
no automatic reconnect/replay. Mirroring is in-memory, **single-worker only**.

## Windows validation (2026-09-28)

No physical macOS microphone capture was tested on Windows.

- Python suite: **30 tests passed**. Conversion at 16/44.1/48 kHz, anti-alias rejection, chunk-boundary
  continuity, mono averaging, PCM extrema/endianness/padding, capture failures,
  exact ACE1 prefix, simulated capture, event/WAV handling, and routing.
- FastAPI integration uses the real session handler with explicitly mocked
  ASR/LLM/TTS and deterministic speech detection. Synthetic PCM is resampled and
  framed by the new client code. Publisher and observer responses are compared
  byte-for-byte, including turn IDs; channel isolation and backpressure are tested.
- A real hosted ASR/LLM/TTS round trip used the new CLI's prerecorded source.
  It saved **491,520 PCM bytes at 44,100 Hz**. The opt-in native Unreal test
  received/queued the same byte count, observed playback, confirmed the Windows
  microphone never started, and observed continued PIE lifetime. This is remote
  input delivery validation, not physical Mac microphone validation.
- The unchanged demo client also completed its prerecorded WAV round trip.
- `/status` confirmed `mock_asr=false`, `mock_llm=false`, `mock_tts=false` for
  the hosted-provider tests. The demo regression received 552,960 bytes at 44.1 kHz.
- Unreal build succeeded. `ACEConversation.RemoteInput` passed.

Python setup and test commands, from `services/orchestrator`:

```powershell
.\.venv\Scripts\python.exe -m pip install -r tools/live_mic_requirements.txt
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
.\.venv\Scripts\python.exe tools/live_mic_client.py --help
.\.venv\Scripts\python.exe -m uvicorn app.main:app --host 127.0.0.1 --port 8080
```

With the observer test ready (commands below), the exact real-backend client
and demo regression commands from `services/orchestrator` were:

```powershell
.\.venv\Scripts\python.exe tools/live_mic_client.py --url ws://127.0.0.1:8080/ws/session --unreal-channel mac-test --test-wav outputs/demo-input.wav --output outputs/live-mic-remote-response.wav
.\.venv\Scripts\python.exe tools/demo_client.py --url ws://127.0.0.1:8080/ws/session --wav outputs/demo-input.wav --output outputs/live-mic-demo-regression.wav
```

Exact native build/test commands from the repository root:

```powershell
Copy-Item unreal/Plugins/ACEConversation/Source/ACEConversation/Private/Tests/ConversationRemoteInputTest.cpp unreal/outputs/MicrophoneDiagnostics/Plugins/ACEConversation/Source/ACEConversation/Private/Tests/ConversationRemoteInputTest.cpp
& 'C:/Program Files/Epic Games/UE_5.6/Engine/Build/BatchFiles/Build.bat' UnrealEditor Win64 Development '-Project=C:/Users/Administrator/Documents/tokkio/unreal/outputs/MicrophoneDiagnostics/HostProject.uproject' '-plugin=C:/Users/Administrator/Documents/tokkio/unreal/outputs/MicrophoneDiagnostics/Plugins/ACEConversation/ACEConversation.uplugin' -NoHotReload -NoUBTMakefiles -NoUBA
& 'C:/Program Files/Epic Games/UE_5.6/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' 'C:/Users/Administrator/Documents/tokkio/unreal/outputs/MicrophoneDiagnostics/HostProject.uproject' '-ExecCmds=Automation RunTests ACEConversation.RemoteInput' '-ConversationObserveUrl=ws://127.0.0.1:8080/ws/session?observe=mac-test' '-ReportExportPath=C:/Users/Administrator/Documents/tokkio/unreal/outputs/live-mic-remote-results' '-TestExit=Automation Test Queue Empty' -unattended -nop4 -NullRHI -stdout -FullStdOutLogOutput
```

The native test requires an external publisher within 180 seconds. Without
`-ConversationObserveUrl`, it explicitly skips rather than connecting. Use an
isolated test editor because the test creates a temporary map and ends its PIE.
Generated evidence: `services/orchestrator/outputs/live-mic-*` and
`unreal/outputs/live-mic-*` (ignored output directories).

Still required on the Mac: install/permission prompts, actual default input
selection, native CoreAudio callbacks and sound levels, Enter controls, network
reachability to EC2, spoken ASR quality and audible Unreal output. Windows tests
prove software conversion/routing/playback state, not microphone hardware or
what a person hears through an RDP playback device.

## Files created/changed

- `.gitignore`: ignore the Mac-only `.venv-mic` environment.
- `services/orchestrator/README.md`: Mac dependencies, launch and routing instructions.
- `services/orchestrator/app/main.py`: opt-in mirror routing on the existing endpoint.
- `services/orchestrator/app/response_mirror.py`: bounded per-observer response forwarding.
- `services/orchestrator/tools/live_mic_client.py`: capture, conversion, streaming and response WAV.
- `services/orchestrator/tools/live_mic_requirements.txt`: lightweight client dependencies.
- `services/orchestrator/tools/LIVE_MIC_CLIENT.md`: this guide and validation record.
- `services/orchestrator/tests/test_live_mic_client.py`: conversion/capture/protocol/response tests.
- `services/orchestrator/tests/test_response_mirror.py`: server routing/session integration tests.
- `unreal/Plugins/ACEConversation/Source/ACEConversation/Private/Tests/ConversationRemoteInputTest.cpp`:
  opt-in remote-input PIE/playback test. No Unreal runtime implementation changed.

Generated/copied source, binaries, logs, reports, WAVs and backend debug artifacts
remain in ignored output/runtime directories. Existing workspace edits, demo
client, protocol module, session implementation and Unreal microphone code were
preserved. The temporary loopback backend used for validation was stopped after
testing; start it with the documented host/port appropriate to your network.
