# Unreal Integration

For Windows push-to-talk capture and on-screen Start/Stop controls, see
[Microphone conversation test](MICROPHONE_TEST.md). For prerecorded audio sent
through the hosted backend to Unreal playback, see
[WebSocket audio test](WEBSOCKET_AUDIO_TEST.md). Those standalone tests do not
require MetaHuman or Audio2Face and do not verify facial animation.

## Engine setup

Use a UE 5.6 C++ project or the included
`ACEAvatarSandbox/ACEAvatarSandbox.uproject` skeleton.

1. Add a MetaHuman to your project.
2. Install the NVIDIA ACE Unreal Plugin separately.
3. Add `Apply ACE Face Animations` to `Face_AnimBP`.
4. Select the `mh_arkit_mapping_pose_A2F` pose asset.
5. Remove interference from the default `Mouth Close` curve to avoid applying
   mouth closure twice.
6. Configure `RemoteA2F` to connect to the remote Audio2Face-3D provider.

## Conversation and audio playback

The bundled `Plugins/ACEConversation` plugin contains
`UConversationBridgeComponent`, which connects to the orchestrator over
WebSocket. It sends session control events and microphone audio, and receives
transcripts, response text, state/error events, and TTS PCM chunks.

Connect microphone capture to `PushMicChunk()` and utterance completion to
`SendMicEnd()`. Wire the bridge delegates to `UACEAudioPlaybackComponent`:

- `OnTtsStarted` -> `StartPlayback`
- `OnTtsAudioChunk` -> `PushPcm16`
- `OnTtsEnded` -> `EndPlayback`

The included `AACEAvatarCharacter` has the bridge and playback components with
these delegate connections. Make it the parent of your MetaHuman character or
copy the equivalent wiring into your existing character.

## Audio2Face integration

The NVIDIA ACE plugin is not bundled. The character skeleton contains comments
at the integration points; install the plugin and implement these calls in your
project before expecting facial animation.

Add the `ACERuntime` module and include `ACERuntimeModule.h`. Route the same PCM
audio used for playback through `FACERuntimeModule::Get()`:

1. Start a new utterance when `tts.start` arrives.
2. For each TTS chunk, queue PCM for playback and call `AnimateFromAudioSamples()`.
3. On `tts.end`, call `EndAudioSamples()`.
4. On disconnect or interruption, call `CancelAnimationGeneration()`.

Match the call signatures and provider configuration to your installed plugin.
Verify playback and facial animation together in Unreal after completing the
MetaHuman animation blueprint and runtime wiring.

## Storage

Use `Config/DefaultEngine.ini.example` as a starting point for an external
Derived Data Cache. Replace the example Linux path for your own machine.
Keep MetaHuman assets, engine caches, generated audio, and build artifacts out
of Git unless you have explicitly planned their storage and distribution.
