# PIE audio diagnostics — UE 5.6.1

## Findings (2026-09-27)

The original HostProject log records PIE sessions lasting about **26, 32, and
89 seconds**, followed by normal world teardown. The previous plugin contains
no runtime PIE-stop, exit, level-travel, or actor-destruction call. Its EndPlay
disconnects only after Unreal ends the actor. These log excerpts do not
identify who requested Stop.

None of those sessions logged a conversation WebSocket connection, and the
backend received no corresponding session. The observed failure was before
the conversation began, rather than evidence of a TTS decoder failure.
The precise original startup cause is **unconfirmed**: the level is unsaved,
and read-only inspection of its autosave found no conversation actor.

The original level uses the Open World / World Partition template. Previously
the actor inherited `Is Spatially Loaded=true`, which can exclude it from play
outside loaded cells. New instances now default to **false** and have zero
lifespan. This removes that startup risk; it does not prove streaming caused
the original incident. Existing instances can retain serialized overrides.
See [Epic's World Partition documentation](https://dev.epicgames.com/documentation/unreal-engine/world-partition-in-unreal-engine).

## Build and verification

The original HostProject editor was still open with an unsaved level. Its
loaded DLL was not replaced and the editor was not closed automatically.
The new sources were built successfully as **UnrealEditor Win64 Development**
with installed **UE 5.6.1**, in a separate host:

`C:\Users\Administrator\Documents\tokkio\unreal\outputs\PIEDiagnostics\HostProject.uproject`

Report: `outputs/pie-test-results/index.json`: **4 succeeded, 0 failed**.
Log: `outputs/pie-test-console.log`.

- `Protocol`: existing ACE1 framing and control messages.
- `PlaybackBuffer`: procedural generator returns the supplied PCM bytes.
- `LiveRoundTrip`: real hosted ASR → LLM → TTS over the existing WebSocket.
- `PIEAutoStart`: real PIE BeginPlay / Auto Start, transcript, LLM response,
  **557,056 PCM bytes** queued at 44.1 kHz (6.32 seconds),
  `IsPlaying=1 AudioDevice=1`, with XAudio2 using **Remote Audio**.
  PIE and the actor remained alive for **36.8 seconds after response completion**.
  Only the explicit test harness then stopped its own PIE session.

This verifies UE playback state and PCM generation. It does not establish
what was audible at your remote workstation's speakers.

## Exact manual test

1. Keep the backend running at `ws://127.0.0.1:8080/ws/session`.
2. Save your current HostProject level before closing that editor. You may
   leave MetaHumanTest open. Open the **PIEDiagnostics** project above; it has
   the rebuilt DLL, unlike the already-open original HostProject.
3. Open `/Game/ConversationAudioTest` if it is not already open. This saved
   empty level contains one **Conversation Audio Round Trip** actor with
   **Auto Start=true**, **Is Spatially Loaded=false**, and Input Wav Path:
   `C:\Users\Administrator\Documents\tokkio\services\orchestrator\outputs\demo-input.wav`.
4. Open **Window → Developer Tools → Output Log** (or the Output Log panel)
   and filter for **LogACEConversation**. Enable Display, Warning, and Error
   messages. Use Play with **Net Mode: Play Standalone**, **Number of Players: 1**.
5. Press **Play**, leaving the session running. Expect these markers:
   `[BeginPlay]`, `[AutoStart]`, `[WebSocketConnecting]`,
   `[WebSocketConnected]`, `[SessionReady]`, `[WavOpened]`,
   `[AudioInputSent]`, `[Transcript]`, `[LLM]`, `[TTSAudio]`,
   `[PCMQueued]`, `[PlaybackStarted]`, `[LLMResponse]`, `[ResponseComplete]`.
   Audio begins after the complete response arrives. The verified run took
   about 12 seconds before playback; hosted latency can vary.
6. Confirm `[PlaybackStarted] IsPlaying=1 AudioDevice=1`. The sound stops
   after its duration; **PIE stays active** until you press Stop/Escape.
   EndPlay then logs `Reason=EndPlayInEditor`.
7. If these playback markers appear but you hear nothing, check Windows
   output/mixer and RDP **Local Resources → Remote audio → Play on this computer**.
   If `[BeginPlay]` is absent, the actor is absent from the runtime world or
   you opened the old DLL/project. If Auto Start is disabled, enable it on the
   placed actor. `[Error]` reports file, connection, or backend failures.

For your original Open World level after installing the updated plugin,
explicitly uncheck the existing actor's **World Partition → Is Spatially Loaded**,
set **Initial Life Span=0**, enable Auto Start, save, and repeat. Do not rebuild
using `BuildPlugin -Package` over a host containing a level you want to keep:
that packaging operation replaces its output directory.

## Reproduce the build and tests

From repository root in PowerShell, with the diagnostic host editor closed:

```powershell
Copy-Item unreal/Plugins/ACEConversation/Source/ACEConversation/* unreal/outputs/PIEDiagnostics/Plugins/ACEConversation/Source/ACEConversation -Recurse -Force
& 'C:/Program Files/Epic Games/UE_5.6/Engine/Build/BatchFiles/Build.bat' UnrealEditor Win64 Development '-Project=C:/Users/Administrator/Documents/tokkio/unreal/outputs/PIEDiagnostics/HostProject.uproject' '-plugin=C:/Users/Administrator/Documents/tokkio/unreal/outputs/PIEDiagnostics/Plugins/ACEConversation/ACEConversation.uplugin' -NoHotReload -NoUBTMakefiles -NoUBA
& 'C:/Program Files/Epic Games/UE_5.6/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' 'C:/Users/Administrator/Documents/tokkio/unreal/outputs/PIEDiagnostics/HostProject.uproject' '-ExecCmds=Automation RunTests ACEConversation' '-ConversationWav=C:/Users/Administrator/Documents/tokkio/services/orchestrator/outputs/demo-input.wav' '-ReportExportPath=C:/Users/Administrator/Documents/tokkio/unreal/outputs/pie-test-results' '-TestExit=Automation Test Queue Empty' -unattended -nop4 -NullRHI -stdout -FullStdOutLogOutput
```

Run automation only in an isolated test editor: the PIE test creates a
temporary map and explicitly ends its own test session. Do **not** pass
`-nosound` to the PIE test. Check the JSON report's failed count, not just the
process exit code. The `UnrealEd` dependency is editor-only.

## Source changes in this fix

Under `Plugins/ACEConversation/Source/ACEConversation`:

- `Private/ACEConversationLog.h` (new): dedicated log category declaration.
- `Private/ACEConversationModule.cpp`: log category definition.
- `Private/ConversationAudioTestActor.cpp`: BeginPlay, Auto Start, transcript,
  LLM, TTS, errors, and EndPlay reason logs; non-spatial/zero-lifespan defaults.
- `Private/ConversationBridgeComponent.cpp`: connection, WAV, input, state,
  session, and connection error logs. No wire protocol change.
- `Private/ACEAudioPlaybackComponent.cpp`: PCM queue, playback/device state,
  invalid PCM and empty-response diagnostics; reset previous byte count.
- `Private/Tests/ConversationPIETest.cpp` (new): real PIE lifecycle regression.
- `ACEConversation.Build.cs`: editor-only test dependency on UnrealEd.

Documentation: `WEBSOCKET_AUDIO_TEST.md` and this file. Generated plugin copies,
builds, saved test level, inspection/preparation scripts and reports are under
ignored `outputs/`. No backend, secrets, MetaHuman, ACE, or existing Blueprint
assets were changed in this fix.
