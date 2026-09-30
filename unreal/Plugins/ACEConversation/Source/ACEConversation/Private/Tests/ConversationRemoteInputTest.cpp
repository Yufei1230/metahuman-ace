#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Tests/AutomationEditorCommon.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "ConversationAudioTestActor.h"
#include "ConversationBridgeComponent.h"
#include "ACEAudioPlaybackComponent.h"
#include "Components/AudioComponent.h"

// Opt-in: an external client publishes into the channel while this PIE listens.
class FObserveRemoteConversation : public IAutomationLatentCommand
{
public:
    explicit FObserveRemoteConversation(FAutomationTestBase* InTest) : Test(InTest), Started(FPlatformTime::Seconds()) {}
    bool Update() override
    {
        UWorld* World = GEditor->PlayWorld;
        if (World)
        {
            for (TActorIterator<AConversationAudioTestActor> It(World); It; ++It)
            {
                AConversationAudioTestActor* Actor = *It;
                if (!Actor->LastError.IsEmpty()) { Test->AddError(Actor->LastError); return true; }
                if (Actor->Bridge->bSessionReady && !bReady)
                {
                    bReady = true;
                    Test->AddInfo(TEXT("REMOTE INPUT READY: publish from the external client now"));
                }
                if (UAudioComponent* Audio = Actor->FindComponentByClass<UAudioComponent>()) { bPlayed |= Audio->IsPlaying(); }
                if (Actor->bResponseComplete)
                {
                    if (Completed == 0) { Completed = FPlatformTime::Seconds(); }
                    if (FPlatformTime::Seconds() - Completed < 10) { return false; }
                    Test->TestFalse(TEXT("Windows microphone never started"), Actor->IsListening());
                    Test->TestTrue(TEXT("Remote ASR transcript delivered"), !Actor->Transcript.IsEmpty());
                    Test->TestTrue(TEXT("Remote LLM text delivered"), !Actor->AssistantText.IsEmpty());
                    Test->TestTrue(TEXT("Remote TTS PCM received"), Actor->ReceivedAudioBytes > 0);
                    Test->TestEqual(TEXT("All remote PCM queued"), Actor->Playback->LastPlaybackBytes, Actor->ReceivedAudioBytes);
                    Test->TestTrue(TEXT("Unreal playback entered playing state"), bPlayed);
                    Test->AddInfo(FString::Printf(TEXT("Remote input PCM=%d; PIE remains alive; no Windows recording endpoint used"), Actor->ReceivedAudioBytes));
                    return true;
                }
            }
        }
        if (FPlatformTime::Seconds() - Started > 180) { Test->AddError(TEXT("Remote publisher/response timed out")); return true; }
        return false;
    }
private:
    FAutomationTestBase* Test;
    double Started;
    double Completed = 0;
    bool bReady = false;
    bool bPlayed = false;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FConversationRemoteInputTest, "ACEConversation.RemoteInput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FConversationRemoteInputTest::RunTest(const FString&)
{
    FString Url;
    if (!FParse::Value(FCommandLine::Get(), TEXT("ConversationObserveUrl="), Url))
    {
        AddInfo(TEXT("Remote input probe skipped; pass -ConversationObserveUrl=ws://host:8080/ws/session?observe=channel and publish externally."));
        return true;
    }
    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    AConversationAudioTestActor* Actor = World->SpawnActor<AConversationAudioTestActor>();
    Actor->Bridge->ServerUrl = Url;
    Actor->bAutoStart = true;
    Actor->bMicrophoneMode = true; // Connect only; no automatic WAV or microphone.
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FObserveRemoteConversation(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}
#endif
