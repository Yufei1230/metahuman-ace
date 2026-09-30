#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "ConversationBridgeComponent.h"
#include "ConversationAudioTestActor.h"
#include "ACEAudioPlaybackComponent.h"
#include "Engine/World.h"
#include "Sound/SoundWaveProcedural.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/Script.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEConversationProtocolTest, "ACEConversation.Protocol",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FACEConversationProtocolTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UConversationBridgeComponent> Bridge(NewObject<UConversationBridgeComponent>());
    Bridge->HandleTextMessage(TEXT("{\"type\":\"state\",\"turn_id\":null,\"payload\":{\"state\":\"LISTENING\",\"reason\":\"ready\"}}"));
    TestTrue(TEXT("Backend ready control parsed"), Bridge->bSessionReady);
    TArray<uint8> Pcm;
    Pcm.Init(0x12, 640);
    TArray<uint8> Frame = Bridge->BuildMicAudioFrame(Pcm);
    const uint8 ExpectedPrefix[] = {0x41,0x43,0x45,0x31,1,1,1,1,0,0,0x3e,0x80,0,0,2,0x80};
    TestEqual(TEXT("32 byte header plus 640 PCM bytes"), Frame.Num(), 672);
    TestTrue(TEXT("Matches Python !4sBBBBII16s wire format"), FMemory::Memcmp(Frame.GetData(), ExpectedPrefix, 16) == 0);
    for (int32 I = 16; I < 32; ++I) { TestEqual(TEXT("Mic UUID is zero"), Frame[I], uint8(0)); }
    Frame[5] = 2; // TTS
    Frame[8] = 0; Frame[9] = 0; Frame[10] = 0xac; Frame[11] = 0x44; // 44100
    int32 Rate = 0;
    TArray<uint8> Parsed;
    TestTrue(TEXT("Parses backend TTS header"), Bridge->ParseTtsAudioFrame(Frame.GetData(), Frame.Num(), Rate, Parsed));
    TestEqual(TEXT("Output rate from header"), Rate, 44100);
    TestTrue(TEXT("PCM unchanged"), Parsed == Pcm);
    Bridge->HandleBinaryMessage(Frame.GetData(), 7, false);
    TestEqual(TEXT("Fragment retained"), Bridge->BinaryMessage.Num(), 7);
    Bridge->HandleBinaryMessage(Frame.GetData() + 7, Frame.Num() - 7, true);
    TestEqual(TEXT("Complete binary message consumed"), Bridge->BinaryMessage.Num(), 0);
    Frame.Add(0);
    TestFalse(TEXT("Rejects trailing bytes"), Bridge->ParseTtsAudioFrame(Frame.GetData(), Frame.Num(), Rate, Parsed));
    Frame.RemoveAt(Frame.Num() - 1);
    Frame[6] = 2;
    TestFalse(TEXT("Rejects unsupported codec"), Bridge->ParseTtsAudioFrame(Frame.GetData(), Frame.Num(), Rate, Parsed));
    TestFalse(TEXT("Rejects truncated header"), Bridge->ParseTtsAudioFrame(Frame.GetData(), 10, Rate, Parsed));
    return true;
}

static UWorld* MakeTestWorld()
{
    const UWorld::InitializationValues Values = UWorld::InitializationValues().AllowAudioPlayback(false)
        .CreatePhysicsScene(false).CreateNavigation(false).CreateAISystem(false);
    // CreateWorld already calls InitializeNewWorld; initializing twice duplicates WorldSettings.
    return UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEConversationPlaybackTest, "ACEConversation.PlaybackBuffer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FACEConversationPlaybackTest::RunTest(const FString& Parameters)
{
    UWorld* World = MakeTestWorld();
    AConversationAudioTestActor* Actor = World->SpawnActor<AConversationAudioTestActor>();
    UACEAudioPlaybackComponent* Playback = Actor->Playback;
    TArray<uint8> Pcm;
    Pcm.Init(0x24, 8820); // 100 ms of PCM16 at 44.1 kHz
    Playback->StartPlayback(44100);
    Playback->PushPcm16(Pcm, 44100);
    TestEqual(TEXT("Response buffered before tts.end"), Playback->PendingAudio.Num(), Pcm.Num());
    Playback->EndPlayback();
    TestEqual(TEXT("Complete response queued, not faded out"), Playback->LastPlaybackBytes, Pcm.Num());
    TArray<uint8> Rendered;
    Rendered.SetNumZeroed(4096);
    const int32 GeneratedBytes = Playback->ProceduralWave->GeneratePCMData(Rendered.GetData(), 2048);
    TestTrue(TEXT("Procedural generator returned PCM bytes"), GeneratedBytes > 0 && GeneratedBytes <= Rendered.Num());
    if (GeneratedBytes > 0 && GeneratedBytes <= Rendered.Num())
    {
        // GeneratePCMData may generate fewer samples than requested (see SoundWaveProcedural.h).
        TestTrue(TEXT("Unreal procedural generator produces received PCM"), FMemory::Memcmp(Rendered.GetData(), Pcm.GetData(), GeneratedBytes) == 0);
    }
    AddInfo(FString::Printf(TEXT("Procedural generator returned %d PCM bytes"), GeneratedBytes));
    Playback->StopPlayback();
    World->DestroyWorld(false);
    return true;
}

class FConversationRoundTripCommand : public IAutomationLatentCommand
{
public:
    explicit FConversationRoundTripCommand(FAutomationTestBase* InTest) : Test(InTest) {}
    virtual bool Update() override
    {
        if (!World)
        {
            // This automation world is outside PIE; allow dynamic UFUNCTION delegates.
            ScriptGuard = MakeUnique<FEditorScriptExecutionGuard>();
            FString Wav;
            if (!FParse::Value(FCommandLine::Get(), TEXT("ConversationWav="), Wav))
            {
                Test->AddError(TEXT("LiveRoundTrip requires -ConversationWav=<absolute 16kHz mono PCM16 WAV path>"));
                return true;
            }
            World = MakeTestWorld();
            Actor = World->SpawnActor<AConversationAudioTestActor>();
            Actor->InputWavPath = Wav;
            Actor->StartTest();
            Started = FPlatformTime::Seconds();
        }
        if (Actor->LastError.IsEmpty() && !Actor->bResponseComplete && FPlatformTime::Seconds() - Started < 180)
        {
            return false;
        }
        Test->TestTrue(TEXT("No backend/client errors"), Actor->LastError.IsEmpty());
        Test->TestTrue(TEXT("Received tts.end within deadline"), Actor->bResponseComplete);
        Test->TestTrue(TEXT("Received ASR transcript"), !Actor->Transcript.IsEmpty());
        Test->TestTrue(TEXT("Received LLM text"), !Actor->AssistantText.IsEmpty());
        Test->TestTrue(TEXT("Received TTS PCM"), Actor->ReceivedAudioBytes > 0);
        Test->TestEqual(TEXT("All TTS PCM queued for Unreal playback"), Actor->Playback->LastPlaybackBytes, Actor->ReceivedAudioBytes);
        Test->AddInfo(FString::Printf(TEXT("ASR=%s; LLM=%s; PCM=%d"), *Actor->Transcript, *Actor->AssistantText, Actor->ReceivedAudioBytes));
        Actor->Bridge->Disconnect();
        Actor->Playback->StopPlayback();
        World->DestroyWorld(false);
        ScriptGuard.Reset();
        return true;
    }
private:
    FAutomationTestBase* Test;
    UWorld* World = nullptr;
    AConversationAudioTestActor* Actor = nullptr;
    double Started = 0;
    TUniquePtr<FEditorScriptExecutionGuard> ScriptGuard;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEConversationLiveTest, "ACEConversation.LiveRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FACEConversationLiveTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FConversationRoundTripCommand(this));
    return true;
}

#endif
