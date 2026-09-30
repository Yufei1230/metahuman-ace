#include "ConversationAudioTestActor.h"
#include "ConversationBridgeComponent.h"
#include "ACEAudioPlaybackComponent.h"
#include "Components/SceneComponent.h"
#include "ACEConversationLog.h"
#include "Engine/World.h"
#include "ConversationMicrophoneComponent.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/PlayerController.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"

AConversationAudioTestActor::AConversationAudioTestActor()
{
#if WITH_EDITORONLY_DATA
    // This is a session/service actor, not spatial content. Keep it loaded even
    // when the player starts in a different World Partition streaming cell.
    bIsSpatiallyLoaded = false;
#endif
    InitialLifeSpan = 0.0f;
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    Bridge = CreateDefaultSubobject<UConversationBridgeComponent>(TEXT("Bridge"));
    Playback = CreateDefaultSubobject<UACEAudioPlaybackComponent>(TEXT("Playback"));
    Microphone = CreateDefaultSubobject<UConversationMicrophoneComponent>(TEXT("Microphone"));
    Microphone->Bridge = Bridge;
}

void AConversationAudioTestActor::BeginPlay()
{
    Super::BeginPlay();
    if (bShowMicrophoneControls) { ShowControls(); }
    UE_LOG(LogACEConversation, Display, TEXT("[BeginPlay] Actor=%s World=%s WorldType=%d AutoStart=%s WAV=\"%s\""),
        *GetPathName(), *GetNameSafe(GetWorld()), GetWorld() ? int32(GetWorld()->WorldType) : -1,
        bAutoStart ? TEXT("true") : TEXT("false"), *InputWavPath);
    if (bAutoStart)
    {
        UE_LOG(LogACEConversation, Display, TEXT("[AutoStart] Triggered; PIE will remain running after the response"));
        if (bMicrophoneMode) { Connect(); }
        else { StartTest(); }
    }
    else
    {
        UE_LOG(LogACEConversation, Warning, TEXT("[AutoStart] Disabled on %s; enable Auto Start on the actor or call StartTest"), *GetName());
    }
}

void AConversationAudioTestActor::StartTest()
{
    ConnectSession(true);
}

void AConversationAudioTestActor::Connect() { ConnectSession(false); }
void AConversationAudioTestActor::Disconnect()
{
    Microphone->CancelCapture();
    Bridge->Disconnect();
    Playback->StopPlayback();
    UE_LOG(LogACEConversation, Display, TEXT("[Disconnected] Microphone canceled and WebSocket closed; PIE remains running"));
}
bool AConversationAudioTestActor::StartListening()
{
    if (Microphone->IsListening()) { return true; }
    if (!Bridge->bSessionReady) { HandleError(TEXT("mic_not_ready"), TEXT("Connect and wait for backend LISTENING")); return false; }
    Playback->StopPlayback(); // Half-duplex: never record the returned sound.
    Transcript.Reset(); AssistantText.Reset(); LastError.Reset();
    ReceivedAudioBytes = 0; bResponseComplete = false;
    return Microphone->StartListening();
}
bool AConversationAudioTestActor::StopListening() { return Microphone->StopListening(); }
bool AConversationAudioTestActor::IsListening() const { return Microphone->IsListening(); }

void AConversationAudioTestActor::ConnectSession(bool bSendWav)
{
    bSendWavOnReady = bSendWav;
    Microphone->CancelCapture();
    UE_LOG(LogACEConversation, Display, TEXT("[StartTest] Actor=%s Endpoint=%s"), *GetName(), *Bridge->ServerUrl);
    Bridge->Disconnect();
    Playback->StopPlayback();
    Transcript.Reset(); AssistantText.Reset(); LastError.Reset();
    ReceivedAudioBytes = 0; bResponseComplete = false;
    Bridge->OnSessionReady.AddUniqueDynamic(this, &AConversationAudioTestActor::HandleReady);
    Bridge->OnAsrFinal.AddUniqueDynamic(this, &AConversationAudioTestActor::HandleTranscript);
    Bridge->OnLlmDelta.AddUniqueDynamic(this, &AConversationAudioTestActor::HandleLlm);
    Bridge->OnTtsStarted.AddUniqueDynamic(this, &AConversationAudioTestActor::HandleTtsStart);
    Bridge->OnTtsAudioChunk.AddUniqueDynamic(this, &AConversationAudioTestActor::HandleAudio);
    Bridge->OnTtsEnded.AddUniqueDynamic(this, &AConversationAudioTestActor::HandleTtsEnd);
    Bridge->OnError.AddUniqueDynamic(this, &AConversationAudioTestActor::HandleError);
    Bridge->Connect();
}

void AConversationAudioTestActor::HandleReady()
{
    if (!bSendWavOnReady)
    {
        UE_LOG(LogACEConversation, Display, TEXT("[SessionReady] Press Start Listening, speak, then Stop Listening / Send Turn"));
        return;
    }
    bSendWavOnReady = false;
    UE_LOG(LogACEConversation, Display, TEXT("[SessionReady] Sending WAV=\"%s\""), *InputWavPath);
    Bridge->SendWavFile(InputWavPath);
}
void AConversationAudioTestActor::HandleTranscript(const FString& Text)
{
    Transcript = Text;
    UE_LOG(LogACEConversation, Display, TEXT("[Transcript] %s"), *Text);
}
void AConversationAudioTestActor::HandleLlm(const FString& Text)
{
    if (AssistantText.IsEmpty()) { UE_LOG(LogACEConversation, Display, TEXT("[LLM] First response text received")); }
    AssistantText += Text;
    UE_LOG(LogACEConversation, Verbose, TEXT("[LLMDelta] %s"), *Text);
}
void AConversationAudioTestActor::HandleTtsStart(int32 SampleRateHz) { Playback->StartPlayback(SampleRateHz); }
void AConversationAudioTestActor::HandleAudio(const TArray<uint8>& Pcm, int32 Rate, const FGuid& Turn)
{
    ReceivedAudioBytes += Pcm.Num();
    UE_LOG(LogACEConversation, Display, TEXT("[TTSAudio] Chunk=%d bytes Total=%d Rate=%d Hz Turn=%s"),
        Pcm.Num(), ReceivedAudioBytes, Rate, *Turn.ToString(EGuidFormats::DigitsWithHyphensLower));
    Playback->PushPcm16(Pcm, Rate);
}
void AConversationAudioTestActor::HandleTtsEnd()
{
    Playback->EndPlayback();
    bResponseComplete = true;
    UE_LOG(LogACEConversation, Display, TEXT("[LLMResponse] %s"), *AssistantText);
    UE_LOG(LogACEConversation, Display, TEXT("[ResponseComplete] Received=%d bytes Queued=%d bytes; actor/session remain alive"),
        ReceivedAudioBytes, Playback->LastPlaybackBytes);
}
void AConversationAudioTestActor::HandleError(const FString& Code, const FString& Message)
{
    LastError = Code + TEXT(": ") + Message;
    UE_LOG(LogACEConversation, Error, TEXT("[Error] Actor=%s %s"), *GetName(), *LastError);
}
void AConversationAudioTestActor::EndPlay(const EEndPlayReason::Type Reason)
{
    const TCHAR* ReasonText = TEXT("Unknown");
    switch (Reason)
    {
    case EEndPlayReason::Destroyed: ReasonText = TEXT("Destroyed"); break;
    case EEndPlayReason::LevelTransition: ReasonText = TEXT("LevelTransition"); break;
    case EEndPlayReason::EndPlayInEditor: ReasonText = TEXT("EndPlayInEditor"); break;
    case EEndPlayReason::RemovedFromWorld: ReasonText = TEXT("RemovedFromWorld (streamed out)"); break;
    case EEndPlayReason::Quit: ReasonText = TEXT("Quit"); break;
    }
    UE_LOG(LogACEConversation, Display, TEXT("[EndPlay] Actor=%s Reason=%s ResponseComplete=%s Received=%d; cleaning up after engine end-play notification"),
        *GetName(), ReasonText, bResponseComplete ? TEXT("true") : TEXT("false"), ReceivedAudioBytes);
    Bridge->Disconnect();
    Microphone->CancelCapture();
    Playback->StopPlayback();
    if (ControlsWidget && GetWorld() && GetWorld()->GetGameViewport())
    {
        GetWorld()->GetGameViewport()->RemoveViewportWidgetContent(ControlsWidget.ToSharedRef());
        ControlsWidget.Reset();
    }
    Super::EndPlay(Reason);
}

void AConversationAudioTestActor::ShowControls()
{
    UGameViewportClient* Viewport = GetWorld()->GetGameViewport();
    if (!Viewport) { return; }
    const TWeakObjectPtr<AConversationAudioTestActor> Self(this);
    ControlsWidget = SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(20)
        [ SNew(SBorder).Padding(16)
          [ SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(FText::FromString(TEXT("Microphone conversation - Start, speak English, Stop / Send (max 30s)")))]
            + SVerticalBox::Slot().AutoHeight()[SNew(SButton).Text(FText::FromString(TEXT("Connect"))).OnClicked_Lambda([Self]() { if (Self.IsValid()) { Self->Connect(); } return FReply::Handled(); })]
            + SVerticalBox::Slot().AutoHeight()[SNew(SButton).Text(FText::FromString(TEXT("Start Listening")))
                .IsEnabled_Lambda([Self]() { return Self.IsValid() && Self->Bridge->bSessionReady && !Self->IsListening(); })
                .OnClicked_Lambda([Self]() { if (Self.IsValid()) { Self->StartListening(); } return FReply::Handled(); })]
            + SVerticalBox::Slot().AutoHeight()[SNew(SButton).Text(FText::FromString(TEXT("Stop Listening / Send Turn")))
                .IsEnabled_Lambda([Self]() { return Self.IsValid() && Self->IsListening(); })
                .OnClicked_Lambda([Self]() { if (Self.IsValid()) { Self->StopListening(); } return FReply::Handled(); })]
            + SVerticalBox::Slot().AutoHeight()[SNew(SButton).Text(FText::FromString(TEXT("WAV fallback test")))
                .OnClicked_Lambda([Self]() { if (Self.IsValid()) { Self->StartTest(); } return FReply::Handled(); })]
            + SVerticalBox::Slot().AutoHeight()[SNew(SButton).Text(FText::FromString(TEXT("Disconnect"))).OnClicked_Lambda([Self]() { if (Self.IsValid()) { Self->Disconnect(); } return FReply::Handled(); })]
            + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).AutoWrapText(true).Text_Lambda([Self]()
                {
                    if (!Self.IsValid()) { return FText::GetEmpty(); }
                    return FText::FromString(FString::Printf(TEXT("%s\nDevice: %s\nTranscript: %s\nAssistant: %s\nError: %s"),
                        Self->IsListening() ? TEXT("RECORDING - press Stop Listening to send") : (Self->Bridge->bSessionReady ? TEXT("Ready") : TEXT("Disconnected / waiting for backend")),
                        *Self->Microphone->DeviceName, *Self->Transcript, *Self->AssistantText, *Self->LastError));
                })]
          ] ];
    Viewport->AddViewportWidgetContent(ControlsWidget.ToSharedRef(), 100);
    if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
    {
        PC->bShowMouseCursor = true;
        FInputModeGameAndUI Mode;
        Mode.SetHideCursorDuringCapture(false);
        Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
        PC->SetInputMode(Mode);
    }
}
