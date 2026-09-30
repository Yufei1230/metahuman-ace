# MetaHuman + NVIDIA ACE

An Unreal Engine digital human project built around **MetaHuman + NVIDIA ACE**.
The primary workflow connects microphone input to a conversational backend and
returns synthesized speech to Unreal for playback and ACE Audio2Face facial
animation integration.

The current focus is the **Unreal / MetaHuman experience**, with a Windows
hosted NVIDIA speech and language backend and an optional Mac microphone client.
The original Tokkio 5.0 / Kubernetes deployment tools remain available as a
separate, optional workflow.

## Architecture

```text
Microphone (Unreal or Mac client)
    -> WebSocket conversation orchestrator (FastAPI + asyncio)
    -> NVIDIA ASR -> NVIDIA LLM -> NVIDIA TTS
    -> PCM audio returned to Unreal
         -> Audio playback
         -> NVIDIA ACE / Audio2Face -> MetaHuman facial animation
```

The repository contains the conversation backend, WebSocket/audio bridge,
playback components, and an Unreal project skeleton. MetaHuman assets and the
NVIDIA ACE Unreal Plugin must be installed in the Unreal environment separately.
The skeleton documents the Audio2Face hook points; its commented ACE calls need
to be wired up when integrating the plugin. Backend or standalone playback tests
alone do not verify MetaHuman facial animation.

## Start here

1. Set up the [Windows hosted backend](services/orchestrator/README.md#windows-hosted-backend-english-default).
2. Follow [Unreal integration](unreal/README.md) to add MetaHuman, the ACE plugin,
   conversation components, and the Audio2Face animation path.
3. Test [Unreal WebSocket audio playback](unreal/WEBSOCKET_AUDIO_TEST.md) and
   [microphone conversation](unreal/MICROPHONE_TEST.md).
4. To use a Mac microphone with the Windows/Unreal host, follow the
   [live microphone guide](services/orchestrator/tools/LIVE_MIC_CLIENT.md).

### Windows conversation backend

Use Python 3.11. From the repository root, run in PowerShell:

```powershell
py -3.11 -m venv services/orchestrator/.venv
.\services\orchestrator\.venv\Scripts\python.exe -m pip install -e .\services\orchestrator
# Fresh checkout only: do not overwrite an existing .env.
Copy-Item services/orchestrator/.env.example services/orchestrator/.env
.\services\orchestrator\.venv\Scripts\python.exe services/orchestrator/tools/init_storage.py
```

Set `ACE_NIM_API_KEY` in the ignored `services/orchestrator/.env`. The example
configuration uses hosted NVIDIA ASR, an OpenAI-compatible NVIDIA LLM endpoint,
and hosted Magpie TTS, with English defaults. This backend configuration does
not require local model downloads or Docker services. See the
[backend README](services/orchestrator/README.md) for language, voice, endpoint,
and storage settings.

```powershell
.\services\orchestrator\.venv\Scripts\python.exe -m uvicorn app.main:app --host 127.0.0.1 --port 8080
```

Connect the Unreal bridge to `ws://127.0.0.1:8080/ws/session` when running on the
same host. For remote microphone access, follow the networking instructions in
the live microphone guide. The WebSocket endpoint has no authentication; use a
private network/tunnel or restrict access to the intended client.

### MetaHuman and ACE integration

The included Unreal skeleton targets UE 5.6:
`unreal/ACEAvatarSandbox/ACEAvatarSandbox.uproject`.

- Add your MetaHuman and install the NVIDIA ACE Unreal Plugin.
- Configure `Face_AnimBP` with `Apply ACE Face Animations` and the
  `mh_arkit_mapping_pose_A2F` pose asset, following the integration guide.
- Configure the Audio2Face provider, including `RemoteA2F` for the documented
  remote Audio2Face-3D path.
- Use `ConversationBridgeComponent` for session events and PCM audio transfer.
- Connect `OnTtsStarted`, `OnTtsAudioChunk`, and `OnTtsEnded` to
  `ACEAudioPlaybackComponent` for playback, and route the same PCM audio into
  the ACE Audio2Face integration for facial animation.

See [Unreal integration](unreal/README.md) for the plugin module, runtime calls,
animation setup, and project-specific wiring.

### Mac microphone input

The Mac client sends microphone audio to the backend and can mirror responses
to an Unreal observer, without relying on RDP microphone redirection. Connect
the Unreal observer first. The documented CLI runs one response per invocation;
reconnect the observer actor before the next invocation. Response mirroring
requires a single backend worker.

Follow the [live microphone guide](services/orchestrator/tools/LIVE_MIC_CLIENT.md)
for dependencies, commands, audio format, and validation limits.

## Repository layout

| Path | Purpose |
| --- | --- |
| `services/orchestrator/` | FastAPI conversation backend, provider adapters, microphone clients, and tests |
| `unreal/ACEAvatarSandbox/` | Unreal Engine project skeleton |
| `unreal/Plugins/ACEConversation/` | Conversation bridge, audio playback, and test components |
| `unreal/README.md` | MetaHuman and NVIDIA ACE integration instructions |
| `docs/architecture/ace-sandbox.md` | Original sandbox architecture and protocol notes |
| `infra/llm/` | LLM endpoint checks and optional local inference helpers |
| `infra/compose/` | Optional Linux speech NIM deployment templates |
| `infra/tokkio/` | Optional Tokkio 5.0 deployment tools |
| `infra/rag/` | Optional retrieval tools and Tokkio RAG configuration |
| `tts/` | Alternative speech synthesis workflow documentation |
| `Irodori-TTS/` | Third-party TTS submodule, retained at its original pinned revision |

## Verification

Run backend tests from the repository root after installing its dependencies:

```powershell
.\services\orchestrator\.venv\Scripts\python.exe -m unittest discover -s services/orchestrator/tests
.\services\orchestrator\.venv\Scripts\python.exe -m pip check
```

With the backend running and provider credentials configured, test a real audio
turn:

```powershell
.\services\orchestrator\.venv\Scripts\python.exe services/orchestrator/tools/demo_client.py --generate-input --require-real --output services/orchestrator/outputs/english-e2e.wav
```

`GET /healthz` checks process liveness. `GET /status` reports provider status;
ASR connectivity requires the audio test. Verify Unreal playback and MetaHuman
facial animation separately in the engine. Historical validation reports record
the environments used for those runs, not a guarantee for a new installation.

## Optional deployments and research workflows

These components are preserved from the original repository and are separate
from the primary MetaHuman + ACE setup:

- [Tokkio 5.0 deployment](infra/tokkio/README.md) and
  [reference architecture](docs/architecture/tokkio-reference-stack.md):
  browser-first Kubernetes deployment.
- [Linux speech NIM](infra/compose/README.md): local ASR/TTS service templates.
- [LLM tooling](infra/llm/README.md): endpoint validation and local inference research.
- [RAG tooling](infra/rag/README.md): local SQLite retrieval and NVIDIA RAG
  Blueprint options for the documented Tokkio workflow.
- [Alternative TTS](tts/README.md): additional speech synthesis research.

## Configuration and storage

Keep API keys in ignored `.env` files, and keep models, caches, generated audio,
and Unreal build artifacts outside Git. The hosted backend uses service-relative
runtime directories by default. Legacy Linux guides use `/data/ACE` and
`/home2/ko66/ace-sandbox`; adapt those paths only when using those deployments.
Historical Windows reports also contain machine-specific paths that must be
adjusted for your environment.

## Origin and third-party components

This repository starts from a content snapshot of
[Yufei1230/tokkio](https://github.com/Yufei1230/tokkio), with a new Git history and
a README focused on MetaHuman + NVIDIA ACE. Existing source files and third-party
references are retained. The [Irodori-TTS](https://github.com/Aratako/Irodori-TTS)
submodule remains an external project with its own authors and license; a fresh
commit history does not change third-party authorship or licensing.
