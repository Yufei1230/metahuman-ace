# Windows backend verification — 2026-09-27

The backend is installed in `services/orchestrator/.venv` using Python 3.11.
English is the default; Japanese remains configurable. No Unreal files, assets,
plugins, Blueprints, or local model runtimes were changed.

## Architecture found and retained

FastAPI exposes `/healthz`, `/status`, and `/ws/session` on port 8080.
The half-duplex session consumes `ACE1` binary PCM16 frames (16 kHz mono),
uses WebRTC VAD or `mic.end` to finish an utterance, then calls hosted Riva ASR
over TLS/gRPC. Despite the streaming interface name, this adapter buffers
audio and uses offline recognition; only final transcripts are produced.
The NVIDIA OpenAI-compatible LLM returns SSE deltas, which are split into
sentences. Hosted Magpie TTS synthesizes each sentence to WAV; the adapter
extracts PCM and sends audio chunks over the existing WebSocket protocol.
The demo client saves returned PCM as a WAV with its actual sample rate.
Each turn is independent; the current LLM adapter does not retain chat history.

`tts` is a separate Japanese TTS research package, not a dependency of this
hosted path. `infra` also contains Linux Docker/Kubernetes Tokkio, local LLM,
RAG, and Irodori deployment helpers. Those separate Japanese research
deployments were preserved. The compose speech smoke-check defaults now
use English and a Windows-compatible temporary path.

## Verification

- Editable dependency install completed; `pip check` passed.
- All 12 orchestrator unit tests passed, including Japanese configuration,
  English sentence/decimal boundaries, relative storage paths, and recovery
  after an ASR failure. The simulated-failure test intentionally logs a traceback.
- `/healthz` returned HTTP 200 and `/status` returned `ok`, with all mocks false.
- The real hosted TTS `/v1/audio/list_voices` returned
  `Magpie-Multilingual.EN-US.Aria`; this exact voice was selected.
- Generated 16 kHz spoken input was sent through `/ws/session` and recognized as:
  “Hello. Please introduce yourself in English in one short sentence.”
- The LLM answered:
  “Hello! I'm Kagawa, your conversational virtual assistant. I'm here to help
  with questions, tasks, or just chat whenever you need.”
- Returned TTS WAV: 44,100 Hz, mono, PCM16, 688,128 PCM bytes, 7.80 seconds.
  RMS amplitude was 1,684. Hosted ASR recognized the saved output as the same
  English response, confirming audible speech content rather than silence.
- Evidence is in ignored `outputs/tts-voices.json`, `outputs/status.json`,
  `outputs/demo-input.wav`, `outputs/english-e2e.wav`,
  `outputs/english-e2e.json`, and `outputs/output-transcript.json`.
- After the final background restart, one hosted LLM call took about 74 seconds
  to produce its first content and returned malformed text (`Iells`). All
  transport stages completed, but that was not a meaningful English answer.
  A repeat using the same input/configuration completed in about 8 seconds
  with a normal English introduction and 766,392 bytes of TTS PCM. Evidence:
  `outputs/english-e2e-final.json` (anomaly) and
  `outputs/english-e2e-repeat.json` / `.wav` (successful final repeat).

## Files changed

| File | Change |
| --- | --- |
| `.gitignore` | Ignore orchestrator secrets and runtime data |
| `README.md` | Windows entry point and current hosted LLM default |
| `infra/compose/check_nim_stack.py` | English defaults and portable temporary output |
| `services/orchestrator/.env` | Preserved credentials; English language/voice/prompt defaults and Windows storage; removed from Git index, kept locally |
| `services/orchestrator/.env.example` | Secret-free hosted configuration with English defaults |
| `services/orchestrator/README.md` | Windows setup, start/test commands, language options and limitations |
| `services/orchestrator/app/settings.py` | Module-relative `.env` and storage; configurable languages; English/Japanese prompts/voices; ASR key fallback; current LLM default |
| `services/orchestrator/app/adapters/asr.py` | English mock text, bounded gRPC future wait, accurate configuration health wording |
| `services/orchestrator/app/adapters/llm.py` | English mock response |
| `services/orchestrator/app/adapters/tts.py` | Hosted voice discovery and voice validation |
| `services/orchestrator/app/main.py` | Language fields in `/status` |
| `services/orchestrator/app/session.py` | English/configured locale and recoverable ASR failures |
| `services/orchestrator/app/text.py` | English sentence boundary handling |
| `services/orchestrator/tests/test_settings.py` | English, Japanese, aliases and portable storage verification |
| `services/orchestrator/tests/test_text.py` | English streamed sentences and decimals |
| `services/orchestrator/tests/test_session_flow.py` | ASR failure recovery verification |
| `services/orchestrator/tools/demo_client.py` | Generated speech test input, real-service requirement, timeout/error handling, actual output rate, JSON evidence |
| `services/orchestrator/tools/init_storage.py` | Portable service-local runtime root |
| `services/orchestrator/WINDOWS_SETUP_REPORT.md` | This report |
| `services/orchestrator/COMMAND_LOG.md` | Execution inventory |

No changes were committed. `.env` was already tracked before this work;
its staged deletion only removes Git tracking and does not delete the local
file. Previous Git history may still contain credentials; history was not rewritten.

## Environment and exact commands

Use the existing ignored `services/orchestrator/.env`. Required credential:
`ACE_NIM_API_KEY`. A separate `ACE_ASR_API_KEY` is optional and otherwise falls
back to the shared key. The existing separate ASR credential was preserved.

Required hosted routing is supplied by `.env.example`:
`ACE_ASR_USE_HOSTED_API=true`, `ACE_ASR_USE_SSL=true`,
`ACE_ASR_SERVER=grpc.nvcf.nvidia.com:443`,
`ACE_ASR_FUNCTION_ID=b0e8b4a5-217c-40b7-9b96-17d84e666317`,
`ACE_ASR_MODEL=` (server default), `ACE_TTS_USE_HOSTED_API=true`,
`ACE_TTS_API_URL=https://877104f7-e885-42b9-8de8-f6e4c6303969.invocation.api.nvcf.nvidia.com/v1/audio/synthesize`,
`ACE_NIM_LLM_BASE_URL=https://integrate.api.nvidia.com/v1`,
`ACE_NIM_LLM_MODEL=nvidia/nemotron-3.5-lightning-30b-a3b`.
The model already configured locally was retained and validated against `/models`.

English settings: `ASSISTANT_LANGUAGE=en-US`, `ASR_LANGUAGE=en-US`,
`TTS_LANGUAGE=en-US`, `ACE_TTS_VOICE=Magpie-Multilingual.EN-US.Aria`,
`ACE_SYSTEM_PROMPT=`. The existing legacy language aliases were also updated
in the local `.env`; new language aliases take precedence.

Japanese option: change all three language variables to `ja-JP`, use
`ACE_TTS_VOICE=Magpie-Multilingual.JA-JP.Louise`, and leave the prompt blank.
Restart after changing configuration. Both voices came from the live server list.

Start, from repository root in PowerShell:

```powershell
.\services\orchestrator\.venv\Scripts\python.exe -m uvicorn app.main:app --host 127.0.0.1 --port 8080
```

The service was also launched as a hidden background process, with its launcher
PID recorded in `runtime/server.pid`; stdout/stderr are in `runtime/logs/server.*.log`.
Do not start a second instance while port 8080 is occupied.

End-to-end test, from repository root in another terminal:

```powershell
.\services\orchestrator\.venv\Scripts\python.exe services/orchestrator/tools/demo_client.py --generate-input --require-real --output services/orchestrator/outputs/english-e2e.wav
```

For recorded audio, replace `--generate-input` with `--wav path/to/input.wav`.
Input must be 16 kHz mono PCM16; the client pads the final 20 ms frame.

## Remaining limits and assumptions

Physical microphone capture and speaker playback on the AWS host were not
tested. The input test uses genuine synthesized speech, sent over the same
PCM/WebSocket path as a microphone client. Output is saved for playback.
Unreal/MetaHuman/ACE animation integration remains untouched and untested.
The server binds loopback; remote Unreal access requires an intentional host
binding and AWS/network configuration, which were not changed.
The background process is not installed as an auto-starting Windows service.
Hosted latency is variable. ASR health is configuration-only; real connectivity
is established by the audio test. Japanese configuration was unit-tested, but
the end-to-end speech test was English only.
The retained hosted LLM had one observed response-quality anomaly; the demo
checks transport completion, not semantic correctness. This remains a provider
reliability limitation, and no alternate model or silent retry policy was introduced.

NVIDIA's hosted voice-discovery API is documented at
https://build.nvidia.com/nvidia/magpie-tts-multilingual/api .
