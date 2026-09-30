# MetaHuman + ACE Conversation Architecture

The primary workflow uses Unreal Engine 5.6 for MetaHuman rendering, microphone
capture, response playback, and NVIDIA ACE Audio2Face integration. A FastAPI
backend coordinates hosted NVIDIA ASR, LLM, and TTS providers. A Mac microphone
client can publish audio to the backend and mirror responses to an Unreal
observer; see the [live microphone guide](../../services/orchestrator/tools/LIVE_MIC_CLIENT.md).

## Runtime and state

The half-duplex turn cycle is `LISTENING -> THINKING -> SPEAKING -> LISTENING`.
The orchestrator manages sessions, utterance completion, provider calls,
sentence-based synthesis, response audio, and per-turn logs. Hosted ASR buffers
an utterance before offline recognition; LLM responses stream and completed
sentences are synthesized into PCM chunks.

The Unreal bridge passes response PCM to the playback component and provides
hook points for ACE Audio2Face. Installing the ACE plugin and wiring those hooks
and the MetaHuman animation blueprint are separate engine-side steps.

## Audio contracts

Microphone input is mono 16 kHz PCM16LE in 20 ms frames (640 payload bytes).
Response audio is mono PCM16LE; use the actual sample rate carried by the audio
frame instead of assuming a fixed output rate.

## WebSocket protocol

One connection carries JSON control messages and binary audio frames.

```json
{
  "type": "session.start",
  "session_id": null,
  "turn_id": null,
  "timestamp": "2026-04-21T07:00:00Z",
  "payload": {"locale": "en-US"}
}
```

Events include `session.start`, `mic.end`, `asr.partial`, `asr.final`,
`llm.delta`, `tts.start`, `tts.end`, `state`, and `error`. Availability of partial
transcripts depends on the ASR adapter. Microphone and TTS audio are binary.

The binary header is 32 bytes:

| Bytes | Field |
| --- | --- |
| 0–3 | Magic `ACE1` |
| 4 | Version `1` |
| 5 | Kind: `1` microphone, `2` TTS |
| 6 | Codec: `1` PCM_S16LE |
| 7 | Channel count |
| 8–11 | Sample rate, big-endian uint32 |
| 12–15 | Payload size, big-endian uint32 |
| 16–31 | Turn UUID bytes |
| 32 onward | PCM payload |

## Operations

The backend loads its service-local `.env`. Store credentials there and keep
runtime logs and audio outside tracked files. `GET /healthz` checks liveness;
`GET /status` reports provider configuration and status. Run a real audio turn
to verify ASR connectivity and validate Unreal playback/animation separately.

The WebSocket endpoint has no authentication. Use loopback for same-host access
or a private network/tunnel with access restricted to the intended clients.
Response mirroring is process-local and requires one backend worker.

The optional [Linux compose templates](../../infra/compose/README.md) provide
local speech services. Their GPU and storage paths must be adapted to the host.
