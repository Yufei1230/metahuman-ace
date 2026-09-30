# Microphone conversation test (Windows, UE 5.6.1)

The ACEConversation plugin now has push-to-talk microphone capture and an
isolated diagnostic level with clickable controls. The existing WAV path,
`/ws/session`, ACE1 binary framing, and Python hosted providers are unchanged.

## Device status on this AWS/RDP session

On 2026-09-27 the native Unreal device probe returned **1 registered capture
implementation and 0 capture devices**. The capture backend is installed;
this is an input-device availability problem.
Windows listed a Remote Audio endpoint, but Unreal found no default recording
device. **A physical microphone round trip has not been verified.** No capture
format could be measured, and whether this session's real microphone requires
resampling is therefore unknown. The plugin uses the actual callback format
and converts to PCM16 little-endian, mono, 16 kHz when a device is available.

In Windows App on the Mac, edit the remote PC and use **Devices & Audio** to
enable the microphone and select playback **On this computer**, then reconnect.
Allow the RDP app microphone access in macOS privacy settings. Microsoft
documents [Mac device and audio redirection](https://learn.microsoft.com/en-us/windows-app/device-audio-folder-redirection-teams).
Check Windows **Sound → Input** (or `mmsys.cpl` → **Recording**) for an actual
recording device and confirm its input meter responds to speech. If it is still
absent, check the host's **Allow audio recording redirection** policy under
Remote Desktop Session Host → Device and Resource Redirection; see
[Microsoft's recording redirection guidance](https://learn.microsoft.com/en-us/azure/virtual-desktop/redirection-configure-audio-video).
No RDP, microphone privacy, or system policy setting was changed by this task.

## Project and manual test

Open this newly built isolated project (the older PIEDiagnostics editor was
left open and unchanged):

`C:\Users\Administrator\Documents\tokkio\unreal\outputs\MicrophoneDiagnostics\HostProject.uproject`

1. Keep the real backend running at `ws://127.0.0.1:8080/ws/session`.
2. Open `/Game/MicrophoneConversationTest`. Its placed Conversation Audio Test
   Actor has Auto Start, Microphone Mode, and Show Microphone Controls enabled;
   Is Spatially Loaded is false. Auto Start **connects only** in this mode.
3. Use Play Standalone, one player, and press **Play**. The on-screen controls
   show **Ready** once the backend acknowledges the session. Filter Output Log
   for `LogACEConversation`. If the mouse is captured, use **Shift+F1**.
4. Press **Start Listening**. Expect `[MicrophoneSelected]`, `[CaptureStarted]`,
   and `[CaptureFormat]`. Only then speak English, e.g. “Hello, please introduce
   yourself in one short sentence.” Use a short continuous sentence.
5. Press **Stop Listening / Send Turn** promptly. Expect `[CaptureStopped]`,
   `[CapturedAudio]`, `[MicrophoneConverted]`, `[MicrophoneSend]`, and
   `[AudioInputSent]`. The last log confirms existing framing plus `mic.end`.
6. Expect `[Transcript]`, `[LLM]`, `[TTSAudio]`, `[PCMQueued]`,
   `[PlaybackStarted]`, `[LLMResponse]`, and eventually `[PlaybackStopped]`.
   Transcript, assistant text and errors also appear in the panel. PIE stays
   active; when Ready, repeat Start/Stop for another turn.
7. If no input device is available, Start Listening displays `mic_no_device`
   and sends nothing. Enable RDP input redirection and retry. No simulated
   audio is substituted. The **WAV fallback test** button runs the known audio
   fixture through the same backend and playback without using a microphone.
8. **Disconnect** cancels any recording, closes the WebSocket, and stops audio.
   It does not end PIE. **Connect** starts a fresh backend session. End PIE
   normally using the editor Stop button; capture is closed without sending.

The separate `/Game/WavConversationTest` level retains the previous automatic
WAV test. Both use `services/orchestrator/outputs/demo-input.wav` as fallback.

## Blueprint interface and behavior

The test actor exposes `Connect`, `Disconnect`, `StartListening`,
`StopListening` (displayed as **Stop Listening / Send Turn**), `IsListening`,
and `StartTest` (WAV fallback). Existing transcript, LLM delta, TTS audio, and
error delegates remain on its `Bridge` component.

For other actors, add `ConversationBridgeComponent`,
`ConversationMicrophoneComponent`, and the existing playback component.
Assign the microphone's **Bridge** reference (or leave it unset to find the
bridge on its owner), connect, wait for OnSessionReady, then call Start/Stop
Listening. Bind the existing bridge events to playback as documented in
[the WAV guide](WEBSOCKET_AUDIO_TEST.md). On disconnect, call CancelCapture
before Bridge.Disconnect, as the test actor does. Microphone errors broadcast
through the same Bridge.OnError delegate.

Capture uses UE AudioCapture with Windows backend implementations. DeviceIndex
defaults to -1 (Windows default input); available indices/names are logged.
The capture stream requests float32 samples at the device's native/default
rate and channel count. Callback metadata is authoritative and is logged.
Stop Listening averages channels to mono, uses UE's BestSinc anti-aliasing
resampler if the rate differs from 16 kHz, then clamps/quantizes PCM16LE.
The existing SendPcm16Audio sends 640-byte frames and pads the final frame.

The capture callback holds only shared, mutex-protected data, not an actor
pointer. It never calls WebSocket or Blueprint functions from the audio
thread. Recording is memory-only, bounded by MaxCaptureSeconds (30 by default,
1–60 configurable). Overflow, format change, missing callbacks, lost backend
readiness, or the time limit cancel/discard the recording with an error.
All-zero input is rejected with `mic_silent` without sending.

The actor stops local TTS before starting capture. This is half-duplex,
button-controlled recording; there is no microphone monitoring, echo
cancellation guarantee, automatic speech endpointing on the client, or barge-in.
The backend still applies its existing VAD: long pauses can split an input
into multiple turns, and silence/non-speech may produce no response. For this
stage use short, continuous utterances. If a turn produces no response, use
Connect to reset before retrying. API keys remain exclusively in Python .env.

## Exact build and level preparation commands

From `C:\Users\Administrator\Documents\tokkio` in PowerShell:

```powershell
New-Item -ItemType Directory -Force unreal/outputs/MicrophoneDiagnostics/Plugins | Out-Null
Copy-Item unreal/Plugins/ACEConversation unreal/outputs/MicrophoneDiagnostics/Plugins/ACEConversation -Recurse -Force
Copy-Item unreal/outputs/PIEDiagnostics/HostProject.uproject unreal/outputs/MicrophoneDiagnostics/HostProject.uproject
& 'C:/Program Files/Epic Games/UE_5.6/Engine/Build/BatchFiles/Build.bat' UnrealEditor Win64 Development '-Project=C:/Users/Administrator/Documents/tokkio/unreal/outputs/MicrophoneDiagnostics/HostProject.uproject' '-plugin=C:/Users/Administrator/Documents/tokkio/unreal/outputs/MicrophoneDiagnostics/Plugins/ACEConversation/ACEConversation.uplugin' -NoHotReload -NoUBTMakefiles -NoUBA
& 'C:/Program Files/Epic Games/UE_5.6/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' 'C:/Users/Administrator/Documents/tokkio/unreal/outputs/MicrophoneDiagnostics/HostProject.uproject' -run=pythonscript '-script=C:/Users/Administrator/Documents/tokkio/unreal/tools/prepare_microphone_host.py' -unattended -nop4 -NullRHI -nosound -stdout -FullStdOutLogOutput
```

The preparation script creates/replaces only the two named diagnostic maps in
MicrophoneDiagnostics. Save any edits you wish to keep elsewhere before rerunning
it. Close this diagnostic editor before rebuilding its loaded DLL. Do not run
BuildPlugin packaging over an output directory containing levels you want to keep.

For subsequent source updates, the exact incremental build used was:

```powershell
Copy-Item unreal/Plugins/ACEConversation/Source/ACEConversation/* unreal/outputs/MicrophoneDiagnostics/Plugins/ACEConversation/Source/ACEConversation -Recurse -Force
& 'C:/Program Files/Epic Games/UE_5.6/Engine/Build/BatchFiles/Build.bat' UnrealEditor Win64 Development '-Project=C:/Users/Administrator/Documents/tokkio/unreal/outputs/MicrophoneDiagnostics/HostProject.uproject' '-plugin=C:/Users/Administrator/Documents/tokkio/unreal/outputs/MicrophoneDiagnostics/Plugins/ACEConversation/ACEConversation.uplugin' -NoHotReload -NoUBTMakefiles -NoUBA
```

## Native tests and evidence

```powershell
& 'C:/Program Files/Epic Games/UE_5.6/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' 'C:/Users/Administrator/Documents/tokkio/unreal/outputs/MicrophoneDiagnostics/HostProject.uproject' '-ExecCmds=Automation RunTests ACEConversation' '-ConversationWav=C:/Users/Administrator/Documents/tokkio/services/orchestrator/outputs/demo-input.wav' -ConversationProbeMicrophone '-ReportExportPath=C:/Users/Administrator/Documents/tokkio/unreal/outputs/microphone-test-results' '-TestExit=Automation Test Queue Empty' -unattended -nop4 -NullRHI -stdout -FullStdOutLogOutput
```

Run automation only in an isolated test editor; PIE tests create temporary
maps and explicitly stop their own PIE after observing continued lifetime.
`-ConversationProbeMicrophone` allows the device probe to enumerate and, if
available, open the real default input for two seconds. Probe samples are not
saved or sent to ASR. An absent microphone is an explicit **warning**, not
proof of successful capture. Omit that flag to skip hardware access.

The first run produced **5 successes, 1 success with warning, 0 failures** in
`outputs/microphone-test-results/index.json`:

- Protocol and PlaybackBuffer: original binary framing and procedural PCM tests.
- LiveRoundTrip and PIEAutoStart: real hosted ASR/LLM/TTS using prerecorded audio,
  with playback and PIE lifetime verified (540,672 PCM bytes in the PIE run).
- MicrophoneConversion: **synthetic** float input; PCM16LE endpoints, stereo
  averaging, 16/44.1/48 kHz duration, and anti-alias rejection of a 12 kHz tone.
- MicrophoneDeviceProbe: **real enumeration**, 0 inputs, explicit unavailable
  warning. No actual recording format or physical speech was available.

`PIEMicrophoneControls` additionally checks that microphone mode creates the
panel, connects without auto-recording/sending WAV, and runs the WAV fallback
while keeping PIE alive. It does not synthesize a successful microphone test.
This additional check **passed**, received/played **483,328 PCM bytes**, and
observed PIE alive for **32.2 seconds after response completion**. Its report is
`outputs/microphone-ui-test-results/index.json` and its log is
`outputs/microphone-ui-test-console.log`.

The final focused conversion/device rerun passed conversion and again reported
one registered capture implementation with zero inputs (explicit warning):
`outputs/microphone-device-test-results/index.json` and
`outputs/microphone-device-test-console.log`. Across the seven distinct cases,
there were **six successes, one device-unavailable warning, and zero failures**.
The final successful build log is `outputs/microphone-build.log`.

The exact focused test commands used were:

```powershell
& 'C:/Program Files/Epic Games/UE_5.6/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' 'C:/Users/Administrator/Documents/tokkio/unreal/outputs/MicrophoneDiagnostics/HostProject.uproject' '-ExecCmds=Automation RunTests ACEConversation.PIEMicrophoneControls' '-ConversationWav=C:/Users/Administrator/Documents/tokkio/services/orchestrator/outputs/demo-input.wav' '-ReportExportPath=C:/Users/Administrator/Documents/tokkio/unreal/outputs/microphone-ui-test-results' '-TestExit=Automation Test Queue Empty' -unattended -nop4 -NullRHI -stdout -FullStdOutLogOutput
& 'C:/Program Files/Epic Games/UE_5.6/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' 'C:/Users/Administrator/Documents/tokkio/unreal/outputs/MicrophoneDiagnostics/HostProject.uproject' '-ExecCmds=Automation RunTests ACEConversation.Microphone' -ConversationProbeMicrophone '-ReportExportPath=C:/Users/Administrator/Documents/tokkio/unreal/outputs/microphone-device-test-results' '-TestExit=Automation Test Queue Empty' -unattended -nop4 -NullRHI -stdout -FullStdOutLogOutput
```

## Files changed in this stage

Under `unreal/`:

| File | Change |
| --- | --- |
| `Plugins/ACEConversation/ACEConversation.uplugin` | Enable built-in AudioCapture dependency |
| `Plugins/ACEConversation/Source/ACEConversation/ACEConversation.Build.cs` | Capture, resampler, Slate/input module dependencies |
| `Plugins/ACEConversation/Source/ACEConversation/Public/ConversationMicrophoneComponent.h` | New reusable microphone controls and format/count properties |
| `Plugins/ACEConversation/Source/ACEConversation/Private/ConversationMicrophoneComponent.cpp` | New bounded capture, conversion, cleanup, errors and existing-bridge send |
| `Plugins/ACEConversation/Source/ACEConversation/Public/ConversationAudioTestActor.h` | Microphone component, mode, controls and Blueprint functions |
| `Plugins/ACEConversation/Source/ACEConversation/Private/ConversationAudioTestActor.cpp` | Connect/listen/send UI and wiring; retain WAV fallback |
| `Plugins/ACEConversation/Source/ACEConversation/Private/Tests/ConversationMicrophoneTests.cpp` | New synthetic conversion tests and real-device probe |
| `Plugins/ACEConversation/Source/ACEConversation/Private/Tests/ConversationPIETest.cpp` | Microphone UI/connect/WAV fallback PIE regression |
| `tools/prepare_microphone_host.py` | New isolated microphone and WAV diagnostic map preparation |
| `MICROPHONE_TEST.md` | This report and exact commands |
| `README.md` | Link to microphone stage |

Generated project/configuration, copied plugin sources/binaries, the two .umap
files, logs and reports are under ignored `unreal/outputs/MicrophoneDiagnostics`
and `unreal/outputs/microphone-*`. No MetaHuman, NVIDIA ACE, Audio2Face,
Face_AnimBP, backend code/protocol, or secrets were edited.
