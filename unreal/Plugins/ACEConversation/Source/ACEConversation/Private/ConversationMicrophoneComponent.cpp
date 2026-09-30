#include "ConversationMicrophoneComponent.h"
#include "ConversationBridgeComponent.h"
#include "ACEConversationLog.h"
#include "AudioCaptureCore.h"
#include "AudioResampler.h"
#include "GameFramework/Actor.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "Features/IModularFeatures.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"

struct FConversationCaptureState
{
    FCriticalSection Mutex;
    TArray<float> Samples;
    int32 Rate = 0;
    int32 Channels = 0;
    bool bAccept = true;
    FString Failure;
};

UConversationMicrophoneComponent::UConversationMicrophoneComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = false;
}
UConversationMicrophoneComponent::~UConversationMicrophoneComponent() = default;

bool UConversationMicrophoneComponent::Error(const FString& Code, const FString& Message)
{
    UE_LOG(LogACEConversation, Warning, TEXT("[MicrophoneError] %s: %s"), *Code, *Message);
    if (Bridge) { Bridge->OnError.Broadcast(Code, Message); }
    return false;
}

bool UConversationMicrophoneComponent::StartListening()
{
    if (bListening) { return true; }
    if (!Bridge && GetOwner()) { Bridge = GetOwner()->FindComponentByClass<UConversationBridgeComponent>(); }
    if (!Bridge || !Bridge->bSessionReady) { return Error(TEXT("mic_not_ready"), TEXT("Connect and wait for LISTENING before recording")); }
    CapturedBytes = ConvertedBytes = CaptureSampleRate = CaptureChannels = 0;
    DeviceName.Reset();
    FModuleManager::LoadModuleChecked<IModuleInterface>(TEXT("AudioCapture"));
    FString CaptureModule;
    GConfig->GetString(TEXT("Audio"), TEXT("AudioCaptureModuleName"), CaptureModule, GEngineIni);
    UE_LOG(LogACEConversation, Display, TEXT("[MicrophoneBackend] ConfiguredModule=%s Factories=%d CanEverRenderAudio=%d"), *CaptureModule,
        IModularFeatures::Get().GetModularFeatureImplementationCount(Audio::IAudioCaptureFactory::GetModularFeatureName()), FApp::CanEverRenderAudio());
    if (!FApp::CanEverRenderAudio())
    {
        return Error(TEXT("mic_backend_unavailable"), TEXT("Audio disabled: Unreal would use null capture. Remove -nosound/use an audio-capable editor process."));
    }
    if (!IModularFeatures::Get().IsModularFeatureAvailable(Audio::IAudioCaptureFactory::GetModularFeatureName()))
    {
        return Error(TEXT("mic_backend_unavailable"), TEXT("UE has no registered audio capture implementation; check that the built-in AudioCapture plugin is enabled"));
    }
    Capture = MakeUnique<Audio::FAudioCapture>();
    TArray<Audio::FCaptureDeviceInfo> Devices;
    Capture->GetCaptureDevicesAvailable(Devices);
    UE_LOG(LogACEConversation, Display, TEXT("[MicrophoneDevices] Count=%d RequestedIndex=%d (-1=default input)"), Devices.Num(), DeviceIndex);
    for (int32 I = 0; I < Devices.Num(); ++I)
    {
        UE_LOG(LogACEConversation, Display, TEXT("[MicrophoneDevice] Index=%d Name=%s PreferredRate=%d Channels=%d"), I, *Devices[I].DeviceName, Devices[I].PreferredSampleRate, Devices[I].InputChannels);
    }
    Audio::FCaptureDeviceInfo Info;
    if (Devices.IsEmpty())
    {
        UE_LOG(LogACEConversation, Warning, TEXT("[CaptureUnavailable] No capture device available; open/start not attempted. A render-only Remote Audio endpoint is insufficient."));
        Capture.Reset();
        return Error(TEXT("mic_no_device"), TEXT("No recording endpoint visible to Unreal. Check RDP microphone redirection and Windows Sound Input."));
    }
    if (!Capture->GetCaptureDeviceInfo(Info, DeviceIndex) || Info.InputChannels < 1)
    {
        UE_LOG(LogACEConversation, Warning, TEXT("[CaptureDeviceSelectionFailed] Count=%d RequestedIndex=%d; no usable selected/default input; open/start not attempted"), Devices.Num(), DeviceIndex);
        Capture.Reset();
        return Error(TEXT("mic_no_device"), TEXT("Inputs exist but selected/default input is unavailable; check DeviceIndex and Windows default recording device."));
    }
    DeviceName = Info.DeviceName;
    UE_LOG(LogACEConversation, Display, TEXT("[MicrophoneSelected] Index=%d Name=%s PreferredRate=%d Channels=%d"), DeviceIndex, *DeviceName, Info.PreferredSampleRate, Info.InputChannels);
    State = MakeShared<FConversationCaptureState, ESPMode::ThreadSafe>();
    const auto Buffer = State.ToSharedRef(); // Callback owns only thread-safe data, never a UObject.
    CaptureLimitSeconds = FMath::Clamp(MaxCaptureSeconds, 1.0f, 60.0f);
    const float Limit = CaptureLimitSeconds;
    Audio::FAudioCaptureDeviceParams Params;
    Params.DeviceIndex = DeviceIndex;
    Params.PCMAudioEncoding = Audio::EPCMAudioEncoding::FLOATING_POINT_32;
    // UE 5.6.1's new OpenAudioCaptureStream lacks DLL export in the installed
    // header. Its exported compatibility wrapper guarantees float32 callbacks.
    PRAGMA_DISABLE_DEPRECATION_WARNINGS
    const bool bOpened = Capture->OpenCaptureStream(Params,
        [Buffer, Limit](const float* Data, int32 Frames, int32 Channels, int32 Rate, double, bool bOverflow)
        {
            FScopeLock Lock(&Buffer->Mutex);
            if (!Buffer->bAccept) { return; }
            if (!Data || Frames <= 0 || Channels < 1 || Channels > 8 || Rate < 8000 || Rate > 192000 || bOverflow ||
                (Buffer->Rate && (Buffer->Rate != Rate || Buffer->Channels != Channels)))
            {
                Buffer->Failure = TEXT("Invalid/changed capture format or capture overflow; recording discarded");
                Buffer->bAccept = false;
                return;
            }
            Buffer->Rate = Rate; Buffer->Channels = Channels;
            if (int64(Buffer->Samples.Num()) + int64(Frames) * Channels > int64(Limit * Rate) * Channels)
            {
                Buffer->Failure = TEXT("Recording exceeded time limit; use a shorter turn (recording discarded)");
                Buffer->bAccept = false;
                return;
            }
            Buffer->Samples.Append(Data, Frames * Channels);
        }, 1024);
    PRAGMA_ENABLE_DEPRECATION_WARNINGS
    if (!bOpened)
    {
        UE_LOG(LogACEConversation, Warning, TEXT("[CaptureOpenFailed] Device=%s Index=%d OpenCaptureStream=false Format=float32 DesiredFrames=1024"), *DeviceName, DeviceIndex);
        CancelCapture();
        return Error(TEXT("mic_open_failed"), TEXT("Cannot open recording device; check microphone permissions, RDP redirection, and exclusive device use"));
    }
    if (!Capture->StartStream())
    {
        UE_LOG(LogACEConversation, Warning, TEXT("[CaptureStartFailed] Device=%s OpenCaptureStream=true StartStream=false"), *DeviceName);
        CancelCapture();
        return Error(TEXT("mic_open_failed"), TEXT("Recording device opened but StartStream failed; see CaptureStartFailed log"));
    }
    bListening = true;
    StartedAt = FPlatformTime::Seconds();
    SetComponentTickEnabled(true);
    UE_LOG(LogACEConversation, Display, TEXT("[CaptureStarted] Device=%s StartStream=true StreamRate=%d PCM=float32; awaiting frames (not yet proof of PCM delivery); Limit=%.0fs"), *DeviceName, Capture->GetSampleRate(), CaptureLimitSeconds);
    return true;
}

void UConversationMicrophoneComponent::CloseCapture()
{
    if (State)
    {
        FScopeLock Lock(&State->Mutex);
        State->bAccept = false;
        UE_LOG(LogACEConversation, Display, TEXT("[CaptureSummary] Frames=%d Samples=%d Bytes=%lld Rate=%d Channels=%d Failure=%s"),
            State->Channels > 0 ? State->Samples.Num() / State->Channels : 0, State->Samples.Num(),
            int64(State->Samples.Num()) * sizeof(float), State->Rate, State->Channels, *State->Failure);
    }
    // Never hold the mutex while stopping/joining the capture callback.
    if (Capture)
    {
        if (Capture->IsCapturing()) { Capture->StopStream(); }
        if (Capture->IsStreamOpen()) { Capture->CloseStream(); }
        Capture.Reset();
    }
    if (bListening) { UE_LOG(LogACEConversation, Display, TEXT("[CaptureStopped] Device=%s"), *DeviceName); }
    bListening = false;
    SetComponentTickEnabled(false);
}

void UConversationMicrophoneComponent::CancelCapture()
{
    CloseCapture();
    State.Reset();
}

bool UConversationMicrophoneComponent::StopListening()
{
    if (!bListening) { return Error(TEXT("mic_not_listening"), TEXT("Start Listening before sending a turn")); }
    CloseCapture();
    TArray<float> Samples;
    FString Failure;
    {
        FScopeLock Lock(&State->Mutex);
        Samples = MoveTemp(State->Samples);
        CaptureSampleRate = State->Rate; CaptureChannels = State->Channels;
        Failure = State->Failure;
    }
    State.Reset();
    CapturedBytes = Samples.Num() * sizeof(float);
    float Peak = 0;
    for (float V : Samples) { if (FMath::IsFinite(V)) { Peak = FMath::Max(Peak, FMath::Abs(V)); } }
    UE_LOG(LogACEConversation, Display, TEXT("[CapturedAudio] Bytes=%d Format=float32 Rate=%d Channels=%d Peak=%.6f"), CapturedBytes, CaptureSampleRate, CaptureChannels, Peak);
    if (!Failure.IsEmpty()) { return Error(TEXT("mic_capture_failed"), Failure); }
    if (!Samples.IsEmpty() && Peak == 0) { return Error(TEXT("mic_silent"), TEXT("Microphone returned only zeros; check mute/input level and RDP microphone redirection. Nothing sent.")); }
    TArray<uint8> Pcm;
    if (!ConvertToPcm16(Samples, CaptureSampleRate, CaptureChannels, Pcm)) { return Error(TEXT("mic_empty_or_invalid"), TEXT("No usable microphone samples or resampling failed")); }
    ConvertedBytes = Pcm.Num();
    UE_LOG(LogACEConversation, Display, TEXT("[MicrophoneConverted] InputBytes=%d OutputBytes=%d PCM16LE mono 16000 Hz Resampled=%d"), CapturedBytes, ConvertedBytes, CaptureSampleRate != 16000);
    if (!Bridge) { return Error(TEXT("mic_no_bridge"), TEXT("No WebSocket bridge assigned")); }
    UE_LOG(LogACEConversation, Display, TEXT("[MicrophoneSend] Sending %d PCM bytes through existing ACE1 frames and mic.end"), ConvertedBytes);
    return Bridge->SendPcm16Audio(Pcm);
}

bool UConversationMicrophoneComponent::ConvertToPcm16(const TArray<float>& Input, int32 Rate, int32 Channels, TArray<uint8>& OutPcm)
{
    OutPcm.Reset();
    if (Input.IsEmpty() || Channels < 1 || Channels > 8 || Input.Num() % Channels || Rate < 8000 || Rate > 192000) { return false; }
    Audio::VectorOps::FAlignedFloatBuffer Mono;
    Mono.SetNumUninitialized(Input.Num() / Channels);
    for (int32 I = 0; I < Mono.Num(); ++I)
    {
        double Sum = 0;
        for (int32 C = 0; C < Channels; ++C)
        {
            const float V = Input[I * Channels + C];
            Sum += FMath::IsFinite(V) ? FMath::Clamp(V, -1.0f, 1.0f) : 0.0f;
        }
        Mono[I] = float(Sum / Channels);
    }
    Audio::VectorOps::FAlignedFloatBuffer Resampled;
    const float* Output = Mono.GetData();
    int32 Count = Mono.Num();
    if (Rate != 16000)
    {
        // Sinc resampling includes anti-alias filtering when downsampling.
        Audio::FResamplingParameters Params{Audio::EResamplingMethod::BestSinc, 1, float(Rate), 16000.0f, Mono};
        Resampled.SetNumUninitialized(Audio::GetOutputBufferSize(Params));
        Audio::FResamplerResults Result;
        Result.OutBuffer = &Resampled;
        if (!Audio::Resample(Params, Result) || Result.InputFramesUsed != Mono.Num()) { return false; }
        Output = Resampled.GetData(); Count = Result.OutputFramesGenerated;
    }
    if (Count <= 0) { return false; }
    OutPcm.SetNumUninitialized(Count * 2);
    for (int32 I = 0; I < Count; ++I)
    {
        const float V = FMath::Clamp(Output[I], -1.0f, 1.0f);
        const int16 Sample = int16(FMath::RoundToInt(V * (V < 0 ? 32768.0f : 32767.0f)));
        OutPcm[2 * I] = uint8(Sample & 255);
        OutPcm[2 * I + 1] = uint8((uint16(Sample) >> 8) & 255);
    }
    return true;
}

void UConversationMicrophoneComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction)
{
    Super::TickComponent(DeltaTime, TickType, TickFunction);
    if (!bListening || !State) { return; }
    FString Failure;
    {
        FScopeLock Lock(&State->Mutex);
        Failure = State->Failure;
        if (CaptureSampleRate == 0 && !State->Samples.IsEmpty())
        {
            CaptureSampleRate = State->Rate; CaptureChannels = State->Channels;
            UE_LOG(LogACEConversation, Display, TEXT("[CaptureFormat] float32 Rate=%d Channels=%d"), CaptureSampleRate, CaptureChannels);
            UE_LOG(LogACEConversation, Display, TEXT("[CaptureFramesReceived] First observed PCM: Frames=%d Samples=%d Bytes=%lld Rate=%d Channels=%d (includes silence)"),
                State->Samples.Num() / CaptureChannels, State->Samples.Num(), int64(State->Samples.Num()) * sizeof(float), CaptureSampleRate, CaptureChannels);
        }
        if (State->Samples.IsEmpty() && FPlatformTime::Seconds() - StartedAt > 5) { Failure = TEXT("Capture device opened but delivered no samples for five seconds"); }
    }
    if (!Bridge || !Bridge->bSessionReady) { Failure = TEXT("WebSocket session no longer ready; recording discarded"); }
    if (FPlatformTime::Seconds() - StartedAt > CaptureLimitSeconds + 1) { Failure = TEXT("Recording time limit reached; recording discarded"); }
    if (!Failure.IsEmpty()) { CancelCapture(); Error(TEXT("mic_capture_failed"), Failure); }
}

void UConversationMicrophoneComponent::EndPlay(const EEndPlayReason::Type Reason) { CancelCapture(); Super::EndPlay(Reason); }
void UConversationMicrophoneComponent::BeginDestroy() { CancelCapture(); Super::BeginDestroy(); }
