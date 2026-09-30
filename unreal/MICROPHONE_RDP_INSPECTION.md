# RDP microphone inspection (2026-09-28)

## Capture path and Windows API

`AConversationAudioTestActor::StartListening` stops local playback and calls
`UConversationMicrophoneComponent::StartListening`. The component requires a
ready bridge, loads `AudioCapture`, then uses `Audio::FAudioCapture`:
`GetCaptureDevicesAvailable`, `GetCaptureDeviceInfo`, `OpenCaptureStream`,
and `StartStream`. Device index -1 selects the default recording input.

The installed UE **5.6.1-44394996** selects **AudioCaptureWasapi**, verified by
the effective runtime configuration, loaded module, and one registered capture
factory in the probe. Evidence in the installed engine under
`C:/Program Files/Epic Games/UE_5.6/Engine`:

- `Config/Windows/BaseWindowsEngine.ini`: `[Audio] AudioCaptureModuleName=AudioCaptureWasapi`.
- `Source/Runtime/AudioCaptureCore/Private/AudioCaptureInternal.h`:
  `FAudioCapture::CreateImpl` uses the first registered factory when
  `FApp::CanEverRenderAudio()` is true; otherwise it creates a null implementation.
- `Source/Runtime/AudioCaptureImplementations/Windows/AudioCaputureWasapi/Private/WasapiDeviceEnumeration.h`:
  active capture-device enumeration uses Windows **MMDevice API**,
  `IMMDeviceEnumerator` / `IMMDevice`, with separate render and capture endpoints.
- `.../Private/WasapiInputStream.h`: capture uses **WASAPI**, specifically
  `IAudioClient3` and `IAudioCaptureClient`.

The installed engine contains these headers but not the backend `.cpp` files;
the exact internal COM call arguments/HRESULTs are not exposed by FAudioCapture.
The plugin logs the failing Unreal operation, not an invented Windows error.
Microsoft documents the endpoint/client relationship in
[About WASAPI](https://learn.microsoft.com/en-us/windows/win32/coreaudio/wasapi).
The plugin does not directly use PnP enumeration, WinMM `waveIn`, DirectSound,
NVIDIA ACE microphone capture, or the RDP protocol.

Capture callbacks supply interleaved float32 PCM with actual rate/channels.
Stop Listening averages channels, resamples to 16 kHz with BestSinc when needed,
and converts to PCM16LE. It calls the existing `Bridge->SendPcm16Audio`.
No bridge, WebSocket framing, `/ws/session`, ACE1, or `mic.end` code was changed.

## Can this run without a recording endpoint?

The real microphone path cannot. The probe can run and report unavailability;
conversion, framing, playback-buffer tests and the prerecorded WAV conversation
path do not require an input endpoint. The WAV integration tests require the
local backend and its configured providers. A warning-only hardware probe is
not proof of successful microphone capture or physical speech recognition.

Current runtime probe: `ConfiguredModule=AudioCaptureWasapi Loaded=1
CanEverRenderAudio=1`, one registered factory, **zero capture devices**.
`[CaptureUnavailable]` confirms open/start were not attempted. This rules out
the null backend and distinguishes this failure from device-open failure.

The reported running audio services and `fDisableAudioCapture=0` do not provide
an input endpoint themselves. The name `Remote Audio` alone does not establish
whether an endpoint is capture or playback. In this process, WASAPI exposes no
usable recording inputs. RDP input redirection must actually present a recording
endpoint before real microphone capture can work. No Windows/RDP settings were
changed during this inspection.

## Logging added

| Marker | Meaning |
| --- | --- |
| `MicrophoneBackend` | Configured capture module, factory/audio availability; probe also logs whether the module is loaded |
| `MicrophoneDevices` | Component enumeration count and requested index |
| `CaptureUnavailable` | Zero capture devices; open/start not attempted |
| `CaptureDeviceSelectionFailed` | Inputs exist, but selected/default input is unusable |
| `CaptureOpenFailed` | `OpenCaptureStream` returned false |
| `CaptureStartFailed` | Open succeeded, `StartStream` returned false |
| `CaptureStarted` | Start succeeded; PCM delivery still unverified |
| `CaptureFramesReceived` | First game-thread observation of valid PCM, including counts/format; silence also counts as frames |
| `CaptureSummary` | Component close/cancel totals, including zero frames and capture failure text |
| `CaptureNoFrames` | Probe started but received no valid PCM in two seconds |

The component retains its five-second no-samples timeout and existing error
codes. No per-buffer logging was added on the audio callback thread. The probe
also closes an opened stream if starting it fails.

## Commands and validation

Run from `C:/Users/Administrator/Documents/tokkio` in PowerShell. Build and editor
execution needed access outside the filesystem sandbox for Unreal's AppData
logs/cache. The first sandbox build failed with `UnauthorizedAccessException`;
the first sandbox editor launch failed during Zen cache initialization.

```powershell
Copy-Item unreal/Plugins/ACEConversation/Source/ACEConversation/* unreal/outputs/MicrophoneDiagnostics/Plugins/ACEConversation/Source/ACEConversation -Recurse -Force
& 'C:/Program Files/Epic Games/UE_5.6/Engine/Build/BatchFiles/Build.bat' UnrealEditor Win64 Development '-Project=C:/Users/Administrator/Documents/tokkio/unreal/outputs/MicrophoneDiagnostics/HostProject.uproject' '-plugin=C:/Users/Administrator/Documents/tokkio/unreal/outputs/MicrophoneDiagnostics/Plugins/ACEConversation/ACEConversation.uplugin' -NoHotReload -NoUBTMakefiles -NoUBA 2>&1 | Tee-Object unreal/outputs/microphone-rdp-build.log
& 'C:/Program Files/Epic Games/UE_5.6/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' 'C:/Users/Administrator/Documents/tokkio/unreal/outputs/MicrophoneDiagnostics/HostProject.uproject' '-ExecCmds=Automation RunTests ACEConversation' '-ConversationWav=C:/Users/Administrator/Documents/tokkio/services/orchestrator/outputs/demo-input.wav' -ConversationProbeMicrophone '-ReportExportPath=C:/Users/Administrator/Documents/tokkio/unreal/outputs/microphone-rdp-test-results' '-TestExit=Automation Test Queue Empty' -unattended -nop4 -NullRHI -stdout -FullStdOutLogOutput 2>&1 | Tee-Object unreal/outputs/microphone-rdp-test-console.log
```

The backend was initially stopped (connection refused). Started it from
`C:/Users/Administrator/Documents/tokkio/services/orchestrator` with:

```powershell
.\.venv\Scripts\python.exe -m uvicorn app.main:app --host 127.0.0.1 --port 8080
```

Exact rerun command after starting the backend:

```powershell
& 'C:/Program Files/Epic Games/UE_5.6/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' 'C:/Users/Administrator/Documents/tokkio/unreal/outputs/MicrophoneDiagnostics/HostProject.uproject' '-ExecCmds=Automation RunTests ACEConversation' '-ConversationWav=C:/Users/Administrator/Documents/tokkio/services/orchestrator/outputs/demo-input.wav' -ConversationProbeMicrophone '-ReportExportPath=C:/Users/Administrator/Documents/tokkio/unreal/outputs/microphone-rdp-retest-results' '-TestExit=Automation Test Queue Empty' -unattended -nop4 -NullRHI -stdout -FullStdOutLogOutput 2>&1 | Tee-Object unreal/outputs/microphone-rdp-retest-console.log
```

Build: **Succeeded**, compiling both changed C++ files and linking the plugin.
The first unrestricted suite run had 3 successes, 1 success with a hardware
warning, and 3 failures because the backend was stopped (LiveRoundTrip,
PIEAutoStart, PIEMicrophoneControls). Unreal exited with status 0 despite test
failures, so inspect `index.json`, not just the process exit status.

Final rerun: **6 successes, 1 success with the expected no-device warning,
0 failures**, verified in `outputs/microphone-rdp-retest-results/index.json`.
Passed: LiveRoundTrip, MicrophoneConversion, PIEAutoStart,
PIEMicrophoneControls, PlaybackBuffer, Protocol. MicrophoneDeviceProbe reports
zero inputs, not successful recording. The PIE tests queued 581,632 and 561,152
TTS PCM bytes respectively and verified continued PIE lifetime after playback.
The temporary backend process was stopped after testing (`Stop-Process -Id
10452`), restoring its initial stopped state.

Successful device open/start and incoming microphone PCM could not be exercised
with the current endpoint configuration. Those log branches compiled but remain
unverified against actual microphone hardware. No simulated microphone success
was substituted.

## Files changed by this inspection

- `Plugins/ACEConversation/Source/ACEConversation/Private/ConversationMicrophoneComponent.cpp`
- `Plugins/ACEConversation/Source/ACEConversation/Private/Tests/ConversationMicrophoneTests.cpp`
- `MICROPHONE_RDP_INSPECTION.md` (this report)

Generated/copied source, build intermediates, binaries and editor output are in
ignored `outputs/MicrophoneDiagnostics`; task logs/reports use
`outputs/microphone-rdp-*`. Pre-existing workspace changes were preserved.
