#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "Features/IModularFeatures.h"
#include "AudioCaptureCore.h"
#include "ConversationMicrophoneComponent.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FConversationConversionTest, "ACEConversation.MicrophoneConversion",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FConversationConversionTest::RunTest(const FString&)
{
    TArray<uint8> Pcm;
    TestFalse(TEXT("Empty input rejected"), UConversationMicrophoneComponent::ConvertToPcm16({}, 48000, 2, Pcm));
    TestFalse(TEXT("Incomplete channel frame rejected"), UConversationMicrophoneComponent::ConvertToPcm16({1}, 48000, 2, Pcm));
    TestTrue(TEXT("PCM16 conversion"), UConversationMicrophoneComponent::ConvertToPcm16({-1, 0, 1}, 16000, 1, Pcm));
    const TArray<uint8> Expected{0,128,0,0,255,127};
    TestTrue(TEXT("Signed PCM16 little-endian extrema"), Pcm == Expected);
    for (const int32 Rate : {16000, 44100, 48000})
    {
        TArray<float> Stereo;
        Stereo.SetNumZeroed(Rate * 2);
        for (int32 I = 0; I < Rate; ++I)
        {
            // Opposite stereo channels should downmix to silence.
            Stereo[2 * I] = 0.5f * FMath::Sin(2 * PI * 1000 * I / Rate);
            Stereo[2 * I + 1] = -Stereo[2 * I];
        }
        TestTrue(TEXT("Stereo downmix/resample succeeded"), UConversationMicrophoneComponent::ConvertToPcm16(Stereo, Rate, 2, Pcm));
        TestTrue(TEXT("One second produces 16000 PCM samples"), FMath::Abs(Pcm.Num() - 32000) <= 2);
        bool bSilent = true;
        for (uint8 B : Pcm) { bSilent &= B == 0; }
        TestTrue(TEXT("Opposite channels cancel"), bSilent);
    }
    // Ensure downsampling filters above-Nyquist energy, not just dropping samples.
    double Rms[2] = {};
    for (int32 Tone = 0; Tone < 2; ++Tone)
    {
        TArray<float> Input;
        Input.SetNumUninitialized(48000);
        for (int32 I = 0; I < Input.Num(); ++I) { Input[I] = 0.5f * FMath::Sin(2 * PI * (Tone ? 12000 : 1000) * I / 48000); }
        TestTrue(TEXT("Tone resampled"), UConversationMicrophoneComponent::ConvertToPcm16(Input, 48000, 1, Pcm));
        const int32 N = Pcm.Num() / 2;
        for (int32 I = 500; I < N - 500; ++I)
        {
            const double V = int16(uint16(Pcm[2 * I]) | (uint16(Pcm[2 * I + 1]) << 8)) / 32768.0;
            Rms[Tone] += V * V;
        }
        Rms[Tone] = FMath::Sqrt(Rms[Tone] / FMath::Max(1, N - 1000));
    }
    TestTrue(TEXT("Speech-band energy preserved"), Rms[0] > 0.3 && Rms[0] < 0.4);
    TestTrue(TEXT("12 kHz rejected before conversion to 16 kHz"), Rms[1] < 0.02);
    AddInfo(TEXT("Synthetic conversion tests only; not a physical microphone test."));
    return true;
}

struct FProbeSamples
{
    FCriticalSection Mutex;
    int64 Samples = 0;
    int32 Rate = 0;
    int32 Channels = 0;
    float Peak = 0;
    bool bOverflow = false;
};
class FConversationDeviceProbeCommand : public IAutomationLatentCommand
{
public:
    explicit FConversationDeviceProbeCommand(FAutomationTestBase* InTest) : Test(InTest) {}
    virtual bool Update() override
    {
        if (!Capture)
        {
            FModuleManager::LoadModuleChecked<IModuleInterface>(TEXT("AudioCapture"));
            FString CaptureModule;
            GConfig->GetString(TEXT("Audio"), TEXT("AudioCaptureModuleName"), CaptureModule, GEngineIni);
            Test->AddInfo(FString::Printf(TEXT("[MicrophoneBackend] ConfiguredModule=%s Loaded=%d CanEverRenderAudio=%d"),
                *CaptureModule, FModuleManager::Get().IsModuleLoaded(FName(*CaptureModule)), FApp::CanEverRenderAudio()));
            if (!Test->TestTrue(TEXT("Audio enabled (otherwise FAudioCapture selects null backend)"), FApp::CanEverRenderAudio())) { return true; }
            const int32 Factories = IModularFeatures::Get().GetModularFeatureImplementationCount(Audio::IAudioCaptureFactory::GetModularFeatureName());
            Test->AddInfo(FString::Printf(TEXT("REAL DEVICE PROBE: registered capture implementations=%d"), Factories));
            if (!Test->TestTrue(TEXT("Capture implementation registered (distinguish missing backend from no devices)"), Factories > 0)) { return true; }
            Capture = MakeUnique<Audio::FAudioCapture>();
            TArray<Audio::FCaptureDeviceInfo> Devices;
            Capture->GetCaptureDevicesAvailable(Devices);
            Test->AddInfo(FString::Printf(TEXT("REAL DEVICE PROBE: Unreal capture devices=%d"), Devices.Num()));
            for (int32 I = 0; I < Devices.Num(); ++I)
            {
                Test->AddInfo(FString::Printf(TEXT("Device[%d]=%s preferred=%dHz channels=%d"), I, *Devices[I].DeviceName, Devices[I].PreferredSampleRate, Devices[I].InputChannels));
            }
            Audio::FCaptureDeviceInfo Default;
            if (Devices.IsEmpty())
            {
                Test->AddWarning(TEXT("[CaptureUnavailable] No capture device available; open/start not attempted. Physical speech unverified. Check RDP microphone redirection and Windows Sound Input."));
                return true;
            }
            if (!Capture->GetCaptureDeviceInfo(Default) || Default.InputChannels < 1)
            {
                Test->AddWarning(TEXT("[CaptureDeviceSelectionFailed] Inputs exist but default recording device is unavailable; open/start not attempted."));
                return true;
            }
            Test->AddInfo(TEXT("Default microphone: ") + Default.DeviceName);
            State = MakeShared<FProbeSamples, ESPMode::ThreadSafe>();
            const auto Buffer = State.ToSharedRef();
            Audio::FAudioCaptureDeviceParams Params;
            PRAGMA_DISABLE_DEPRECATION_WARNINGS
            const bool bOpened = Capture->OpenCaptureStream(Params, [Buffer](const float* Data, int32 Frames, int32 Channels, int32 Rate, double, bool Overflow)
            {
                FScopeLock Lock(&Buffer->Mutex);
                if (!Data || Frames <= 0 || Channels <= 0 || Rate <= 0) { return; }
                Buffer->Rate = Rate; Buffer->Channels = Channels; Buffer->Samples += int64(Frames) * Channels;
                Buffer->bOverflow |= Overflow;
                if (Data) { for (int32 I = 0; I < Frames * Channels; ++I) { Buffer->Peak = FMath::Max(Buffer->Peak, FMath::Abs(Data[I])); } }
            }, 1024);
            PRAGMA_ENABLE_DEPRECATION_WARNINGS
            if (!bOpened)
            {
                Test->AddWarning(TEXT("[CaptureOpenFailed] OpenCaptureStream=false; StartStream not attempted. Physical speech unverified."));
                if (Capture->IsStreamOpen()) { Capture->CloseStream(); }
                return true;
            }
            if (!Capture->StartStream())
            {
                Test->AddWarning(TEXT("[CaptureStartFailed] OpenCaptureStream=true StartStream=false. Physical speech unverified."));
                if (Capture->IsCapturing()) { Capture->StopStream(); }
                Capture->CloseStream();
                return true;
            }
            Test->AddInfo(TEXT("[CaptureStarted] StartStream=true PCM=float32; awaiting frames, delivery not yet verified."));
            Started = FPlatformTime::Seconds();
            return false;
        }
        {
            FScopeLock Lock(&State->Mutex);
            if (!bReportedFrames && State->Samples > 0)
            {
                Test->AddInfo(FString::Printf(TEXT("[CaptureFramesReceived] Frames=%lld Samples=%lld Bytes=%lld Rate=%d Channels=%d Peak=%.6f (includes silence)"),
                    State->Samples / State->Channels, State->Samples, State->Samples * 4, State->Rate, State->Channels, State->Peak));
                bReportedFrames = true;
            }
        }
        if (FPlatformTime::Seconds() - Started < 2) { return false; }
        Capture->StopStream(); Capture->CloseStream();
        FScopeLock Lock(&State->Mutex);
        Test->AddInfo(FString::Printf(TEXT("REAL CAPTURE: float32 Rate=%d Channels=%d Bytes=%lld Peak=%.6f Overflow=%d ResamplingRequired=%d"), State->Rate, State->Channels, State->Samples * 4, State->Peak, State->bOverflow, State->Rate != 16000));
        if (State->Samples == 0) { Test->AddWarning(TEXT("[CaptureNoFrames] Stream started but delivered no valid PCM frames within two seconds.")); }
        Test->AddInfo(TEXT("Probe records two seconds without saving or sending. Receiving samples does not verify physical speech or microphone ASR. Manual spoken test still required."));
        return true;
    }
private:
    FAutomationTestBase* Test;
    TUniquePtr<Audio::FAudioCapture> Capture;
    TSharedPtr<FProbeSamples, ESPMode::ThreadSafe> State;
    double Started = 0;
    bool bReportedFrames = false;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FConversationDeviceProbeTest, "ACEConversation.MicrophoneDeviceProbe",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FConversationDeviceProbeTest::RunTest(const FString&)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("ConversationProbeMicrophone")))
    {
        AddInfo(TEXT("Hardware probe not requested. Pass -ConversationProbeMicrophone to enumerate and open the default microphone for two seconds."));
        return true;
    }
    ADD_LATENT_AUTOMATION_COMMAND(FConversationDeviceProbeCommand(this));
    return true;
}
#endif
