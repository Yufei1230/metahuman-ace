#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Tests/AutomationEditorCommon.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Components/AudioComponent.h"
#include "ConversationAudioTestActor.h"
#include "ACEAudioPlaybackComponent.h"
#include "ConversationBridgeComponent.h"

// Explicit automation only. This test owns its temporary map and stops its PIE
// session after observing playback and continued lifetime. Runtime code never stops PIE.
class FObserveConversationPIE : public IAutomationLatentCommand
{
public:
    explicit FObserveConversationPIE(FAutomationTestBase* InTest) : Test(InTest), Started(FPlatformTime::Seconds()) {}
    virtual bool Update() override
    {
        UWorld* World = GEditor->PlayWorld;
        if (!World)
        {
            if (!bSawWorld && FPlatformTime::Seconds() - Started < 30) { return false; }
            Test->AddError(TEXT("PIE failed to start or stopped before observation completed"));
            return true;
        }
        bSawWorld = true;
        AConversationAudioTestActor* Actor = nullptr;
        for (TActorIterator<AConversationAudioTestActor> It(World); It; ++It) { Actor = *It; break; }
        if (!Actor)
        {
            Test->AddError(TEXT("Conversation actor absent from PIE world"));
            return true;
        }
        if (Actor->bMicrophoneMode && !bCheckedControls)
        {
            if (!Actor->Bridge->bSessionReady && FPlatformTime::Seconds() - Started < 30) { return false; }
            Test->TestTrue(TEXT("Microphone mode connects on BeginPlay"), Actor->Bridge->bSessionReady);
            Test->TestTrue(TEXT("On-screen controls installed"), Actor->ControlsWidget.IsValid());
            Test->TestFalse(TEXT("Microphone does not record automatically"), Actor->IsListening());
            Test->TestTrue(TEXT("No WAV automatically sent in microphone mode"), Actor->Transcript.IsEmpty() && Actor->ReceivedAudioBytes == 0);
            bCheckedControls = true;
            Actor->StartTest(); // Same operation as the on-screen WAV fallback button.
            Test->AddInfo(TEXT("Microphone UI/connect verified; invoking WAV fallback. This is NOT a spoken microphone test."));
            return false;
        }
        if (UAudioComponent* Audio = Actor->FindComponentByClass<UAudioComponent>())
        {
            bSawPlaying |= Audio->IsPlaying();
        }
        if (!Actor->LastError.IsEmpty()) { Test->AddError(Actor->LastError); return true; }
        if (Actor->bResponseComplete)
        {
            if (CompletedAt == 0)
            {
                CompletedAt = FPlatformTime::Seconds();
                // Hosted TTS rate is 44100; allow even an 8 kHz response to finish.
                ObserveSeconds = FMath::Max(10.0, double(Actor->ReceivedAudioBytes) / 16000.0 + 2.0);
                Test->AddInfo(FString::Printf(TEXT("AutoStart completed: transcript=%s; PCM=%d. Observing PIE for %.1fs"), *Actor->Transcript, Actor->ReceivedAudioBytes, ObserveSeconds));
            }
            if (FPlatformTime::Seconds() - CompletedAt < ObserveSeconds) { return false; }
            Test->TestTrue(TEXT("AutoStart produced transcript"), !Actor->Transcript.IsEmpty());
            Test->TestTrue(TEXT("AutoStart produced LLM response"), !Actor->AssistantText.IsEmpty());
            Test->TestTrue(TEXT("TTS returned PCM"), Actor->ReceivedAudioBytes > 0);
            Test->TestEqual(TEXT("All PCM queued"), Actor->Playback->LastPlaybackBytes, Actor->ReceivedAudioBytes);
            Test->TestTrue(TEXT("Audio component entered playing state with real PIE audio device"), bSawPlaying);
            Test->TestTrue(TEXT("Actor remains alive after playback"), Actor->HasActorBegunPlay() && !Actor->IsActorBeingDestroyed());
            Test->AddInfo(TEXT("PIE stayed alive after AutoStart/playback. Test harness will now explicitly end its own PIE session."));
            return true;
        }
        if (FPlatformTime::Seconds() - Started > 180) { Test->AddError(TEXT("AutoStart round trip timed out")); return true; }
        return false;
    }
private:
    FAutomationTestBase* Test;
    double Started;
    double CompletedAt = 0;
    double ObserveSeconds = 0;
    bool bSawWorld = false;
    bool bSawPlaying = false;
    bool bCheckedControls = false;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEConversationPIETest, "ACEConversation.PIEAutoStart",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FACEConversationPIETest::RunTest(const FString& Parameters)
{
    FString Wav;
    if (!FParse::Value(FCommandLine::Get(), TEXT("ConversationWav="), Wav))
    {
        AddError(TEXT("Requires -ConversationWav=<16kHz mono PCM16 WAV>. Run in isolated test editor; creates a temporary map."));
        return false;
    }
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    AConversationAudioTestActor* Actor = World->SpawnActor<AConversationAudioTestActor>();
    Actor->InputWavPath = Wav;
    Actor->bAutoStart = true;
    TestFalse(TEXT("Service actor defaults to always loaded"), Actor->GetIsSpatiallyLoaded());
    TestEqual(TEXT("No automatic actor destruction"), Actor->InitialLifeSpan, 0.0f);
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FObserveConversationPIE(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEConversationMicrophonePIETest, "ACEConversation.PIEMicrophoneControls",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FACEConversationMicrophonePIETest::RunTest(const FString& Parameters)
{
    FString Wav;
    if (!FParse::Value(FCommandLine::Get(), TEXT("ConversationWav="), Wav)) { AddError(TEXT("Requires -ConversationWav")); return false; }
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    AConversationAudioTestActor* Actor = World->SpawnActor<AConversationAudioTestActor>();
    Actor->InputWavPath = Wav;
    Actor->bAutoStart = true;
    Actor->bMicrophoneMode = true;
    Actor->bShowMicrophoneControls = true;
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FObserveConversationPIE(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}
#endif
