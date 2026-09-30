# UE 5.6 Windows WebSocket audio test

For the latest Development Editor rebuild, preconfigured test level, runtime
logs, and real PIE playback/lifetime verification, see [PIE diagnostics](PIE_DIAGNOSTICS.md).
The three-test results below describe the earlier transport-only verification.

Verified on this Windows machine on 2026-09-27 with UE **5.6.1**:
Editor, Win64 Development, and Win64 Shipping builds succeeded. The final
exported native test report records **3 succeeded, 0 failed**:
`ACEConversation.Protocol`, `ACEConversation.PlaybackBuffer`, and
`ACEConversation.LiveRoundTrip`.

The live test returned the transcript “Hello. Please introduce yourself in
English in one short sentence.” and the response “I'm Kagawa, your
conversational virtual assistant. How can I help you today?” All **356,792**
returned PCM bytes were queued into Unreal playback. The procedural audio
test generated 2,048 bytes and verified they matched the supplied PCM.
Evidence: `outputs/test-results/index.json` and `outputs/native-test-console.log`.
Physical speaker output was not enabled in these automated checks.

This reuses the existing `ACEConversation` plugin. No MetaHuman, NVIDIA ACE,
Audio2Face, microphone capture, API keys, or Blueprint asset edits are required.
The backend remains `ws://127.0.0.1:8080/ws/session`.

## Quick test in Unreal

1. Copy `unreal/Plugins/ACEConversation` into your UE 5.6 project's
   `Plugins/ACEConversation`, enable the plugin, and build the project.
   Alternatively, use the isolated host project created by the build command
   below: `unreal/outputs/ACEConversation/HostProject/HostProject.uproject`.
2. Open a blank level. Place **Conversation Audio Test Actor** from the
   **Place Actors** panel (or create a Blueprint subclass of that C++ actor).
3. Set **Input Wav Path** to the absolute path of
   `services/orchestrator/outputs/demo-input.wav`, or another uncompressed
   16 kHz mono PCM16 WAV. This reads a file from disk, not an imported SoundWave
   asset. Relative paths are resolved against your Unreal project directory.
4. Enable **Auto Start** and press Play. The actor connects, waits for the
   backend's `LISTENING/ready` event, sends the WAV, logs the transcript and
   LLM response, buffers the returned PCM, and plays it after `tts.end`.
5. Filter Output Log for `LogACEConversation`, particularly `[BeginPlay]`,
   `[Transcript]`, `[LLMResponse]`, `[PCMQueued]`, and `[PlaybackStarted]`.
   Ensure Windows has a working audio output device.
   The actor's Blueprint-readable fields expose transcript, assistant text,
   error, completion, and received byte count.

The generated host project is disposable test infrastructure; no changes to
the existing avatar project are needed. It opens without MetaHuman or ACE.
The plugin uses built-in Engine, Json, Projects, and WebSockets modules, with
UnrealEd included only in editor builds for the PIE regression test.

## Reusable Blueprint component interface

Add `ConversationBridgeComponent` and `ACEAudioPlaybackComponent` to a plain
Actor. Bind events before calling `Connect`:

- `OnSessionReady` → `SendWavFile(FilePath)`.
- `OnAsrFinal` / `OnAsrPartial` → transcript display.
- `OnLlmDelta` → append text to a display (deltas are not complete responses).
- `OnTtsStarted(SampleRateHz)` → playback `StartPlayback`.
- `OnTtsAudioChunk(Pcm16Mono, SampleRateHz, TurnId)` → playback `PushPcm16`.
- `OnTtsEnded` → playback `EndPlayback`.
- `OnError(Code, Message)` → error display.

`SendPcm16Audio` accepts a complete raw PCM utterance and sends 640-byte
20 ms frames, zero-padding the last frame, followed by `mic.end`.
`PushMicChunk` and `SendMicEnd` remain available for existing clients.
Only send when `bSessionReady` is true. This is a half-duplex backend.
After `OnTtsEnded` and `LISTENING/turn_complete`, another file can be sent. To reconnect,
call `Disconnect` then `Connect`. Ending Play disconnects and stops audio.

`Locale` defaults to `en-US` and is session metadata. The Python `.env`
controls actual model languages. Unreal receives no NVIDIA credentials.

Playback deliberately buffers one returned utterance before playing. This
minimal path avoids underruns and the previous premature fade at `tts.end`.
It stops the procedural sound after the PCM duration plus a 0.5 second tail.
A new response interrupts old playback. Streaming playback and precise
audio-device completion notifications can be added after this test stage.

## Exact backend protocol

Text messages are UTF-8 JSON with `type`, `session_id` (UUID string),
`timestamp` (UTC ISO8601), and `payload`; server frames also include `turn_id`.

```json
{"type":"session.start","session_id":"11111111-2222-3333-4444-555555555555","timestamp":"2026-09-27T00:00:00Z","payload":{"locale":"en-US"}}
```

After sending audio:

```json
{"type":"mic.end","session_id":"11111111-2222-3333-4444-555555555555","timestamp":"2026-09-27T00:00:01Z","payload":{}}
```

The binary header exactly matches Python `struct.Struct("!4sBBBBII16s")`:

| Offset | Size | Meaning |
| --- | --- | --- |
| 0 | 4 | ASCII `ACE1` |
| 4 | 1 | Version `1` |
| 5 | 1 | Kind `1` input / `2` TTS |
| 6 | 1 | Codec `1` = signed little-endian PCM16 |
| 7 | 1 | Channels `1` |
| 8 | 4 | Sample rate, unsigned **big-endian** |
| 12 | 4 | PCM payload byte count, unsigned **big-endian** |
| 16 | 16 | Turn UUID in network byte order; zeros for input |
| 32 | payload count | Raw **little-endian** PCM16 audio |

Input is 16 kHz mono; returned sample rate is read from each header (currently
44.1 kHz). UE `OnBinaryMessage` fragments are assembled before parsing;
message size, codec, channel count, payload length, and sample rate are checked.
`TurnId` is taken from the binary header, not inferred from a preceding event.
UE can dispatch text and binary through separate callback queues. Therefore
the component emits `OnTtsStarted` just before the first complete audio frame
and defers `OnTtsEnded` by a game frame so queued binary data is consumed first.

Server events are `state`, `asr.partial`, `asr.final`, `llm.delta`,
`tts.start`, binary TTS audio, `tts.end`, and `error`. State progresses through
`LISTENING`, `THINKING`, and `SPEAKING`, returning to `LISTENING/turn_complete`.
The hosted ASR adapter currently emits a final transcript after buffering the
utterance; partial-event support remains available in the Unreal component.

## Native build and automated tests

From repository root in PowerShell:

```powershell
& 'C:/Program Files/Epic Games/UE_5.6/Engine/Build/BatchFiles/RunUAT.bat' BuildPlugin '-Plugin=C:/Users/Administrator/Documents/tokkio/unreal/Plugins/ACEConversation/ACEConversation.uplugin' '-Package=C:/Users/Administrator/Documents/tokkio/unreal/outputs/ACEConversation' -TargetPlatforms=Win64 -NoDeleteHostProject
```

`BuildPlugin` replaces the generated package directory when rebuilt. Use a
different `-Package` path if you want to retain a prior generated package.

With the backend running, run the compiled native checks and real audio turn:

```powershell
& 'C:/Program Files/Epic Games/UE_5.6/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' 'C:/Users/Administrator/Documents/tokkio/unreal/outputs/ACEConversation/HostProject/HostProject.uproject' '-ExecCmds=Automation RunTests ACEConversation' '-ConversationWav=C:/Users/Administrator/Documents/tokkio/services/orchestrator/outputs/demo-input.wav' '-ReportExportPath=C:/Users/Administrator/Documents/tokkio/unreal/outputs/test-results' -TestExit='Automation Test Queue Empty' -unattended -nop4 -NullRHI -stdout -FullStdOutLogOutput
```

The protocol test compares the actual C++ header to Python's wire format,
checks fragmented delivery, and rejects invalid frames. The playback test
drives Unreal's procedural PCM generator and compares output bytes to the
received samples. The live test uses the actual Unreal WebSocket code, sends
the WAV to the existing backend, and requires ASR, LLM text, `tts.end`, and
all returned PCM queued to Unreal playback within 180 seconds.

The new `PIEAutoStart` test also runs a real PIE session with audio enabled,
then verifies playback state and continued actor/session lifetime. Run it in
an isolated editor because it creates a temporary map and ends its own PIE.
The earlier three-test run used `-nosound`; the current four-test suite must
omit that flag. Hosted response quality/latency remains provider-dependent.
Silence-only input produces no VAD turn, and long internal pauses can split
files into multiple turns; use a short continuous spoken WAV for this first test.
This initial client assumes one returned utterance at a time and has no
automatic reconnect/retry policy. The live automation test has a 180 second
deadline; in Editor Play, use `Disconnect` if a provider stalls.

Check `outputs/test-results/index.json` for individual test results: UE may
exit zero when its test queue empties even if an individual assertion failed.

## Files changed in this stage

All paths below are under `unreal`; the backend was not edited in this stage.

| Path | Change |
| --- | --- |
| `Plugins/ACEConversation/Source/ACEConversation/Public/ConversationBridgeComponent.h` | File/PCM sending, readiness, locale, fragment/completion state |
| `Plugins/ACEConversation/Source/ACEConversation/Private/ConversationBridgeComponent.cpp` | Existing protocol reused; WAV parsing, padding, lifecycle, binary UUIDs and callback ordering |
| `Plugins/ACEConversation/Source/ACEConversation/Public/ACEAudioPlaybackComponent.h` | Response buffer and playback byte count |
| `Plugins/ACEConversation/Source/ACEConversation/Private/ACEAudioPlaybackComponent.cpp` | Complete-response playback; no premature fade; stop/cleanup |
| `Plugins/ACEConversation/Source/ACEConversation/Public/ConversationAudioTestActor.h` | New standalone test actor and Blueprint-readable result fields |
| `Plugins/ACEConversation/Source/ACEConversation/Private/ConversationAudioTestActor.cpp` | New automatic connection, file send, events and playback wiring |
| `Plugins/ACEConversation/Source/ACEConversation/Private/Tests/ConversationTests.cpp` | New native wire-format, procedural PCM, and live backend tests |
| `Plugins/ACEConversation/Config/FilterPlugin.ini` | Default empty packaging filter created by UE's BuildPlugin tool |
| `README.md` | Link to this independent test stage |
| `WEBSOCKET_AUDIO_TEST.md` | Setup, protocol, test commands and file report |

Generated build files, compiled plugin, isolated host project, logs, and test
reports are in ignored `unreal/outputs`. The source plugin's descriptor was
retained; UnrealEd is now an editor-only test dependency. No avatar project, asset, Blueprint, or NVIDIA
ACE plugin file was changed.
