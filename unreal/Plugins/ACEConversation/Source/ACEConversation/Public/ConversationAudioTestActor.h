#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ConversationAudioTestActor.generated.h"

class UConversationBridgeComponent;
class UACEAudioPlaybackComponent;
class UConversationMicrophoneComponent;
class SWidget;

// Standalone audio test actor: no MetaHuman or NVIDIA ACE dependency.
UCLASS()
class ACECONVERSATION_API AConversationAudioTestActor : public AActor
{
    GENERATED_BODY()
public:
    AConversationAudioTestActor();

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Conversation Test")
    TObjectPtr<UConversationBridgeComponent> Bridge;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Conversation Test")
    TObjectPtr<UACEAudioPlaybackComponent> Playback;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Conversation Test")
    TObjectPtr<UConversationMicrophoneComponent> Microphone;
    // Auto Start connects only; recording requires an explicit button press.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Conversation Test")
    bool bMicrophoneMode = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Conversation Test")
    bool bShowMicrophoneControls = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Conversation Test")
    FString InputWavPath;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Conversation Test")
    bool bAutoStart = false;

    UPROPERTY(BlueprintReadOnly, Category="Conversation Test")
    FString Transcript;
    UPROPERTY(BlueprintReadOnly, Category="Conversation Test")
    FString AssistantText;
    UPROPERTY(BlueprintReadOnly, Category="Conversation Test")
    FString LastError;
    UPROPERTY(BlueprintReadOnly, Category="Conversation Test")
    bool bResponseComplete = false;
    UPROPERTY(BlueprintReadOnly, Category="Conversation Test")
    int32 ReceivedAudioBytes = 0;

    UFUNCTION(BlueprintCallable, Category="Conversation Test")
    void StartTest();
    UFUNCTION(BlueprintCallable, Category="Conversation Test")
    void Connect();
    UFUNCTION(BlueprintCallable, Category="Conversation Test")
    void Disconnect();
    UFUNCTION(BlueprintCallable, Category="Conversation Test")
    bool StartListening();
    UFUNCTION(BlueprintCallable, Category="Conversation Test", meta=(DisplayName="Stop Listening / Send Turn"))
    bool StopListening();
    UFUNCTION(BlueprintPure, Category="Conversation Test")
    bool IsListening() const;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
    friend class FObserveConversationPIE;
    bool bSendWavOnReady = false;
    TSharedPtr<SWidget> ControlsWidget;
    void ConnectSession(bool bSendWav);
    void ShowControls();
    UFUNCTION() void HandleReady();
    UFUNCTION() void HandleTranscript(const FString& Text);
    UFUNCTION() void HandleLlm(const FString& Text);
    UFUNCTION() void HandleTtsStart(int32 SampleRateHz);
    UFUNCTION() void HandleAudio(const TArray<uint8>& Pcm, int32 Rate, const FGuid& Turn);
    UFUNCTION() void HandleTtsEnd();
    UFUNCTION() void HandleError(const FString& Code, const FString& Message);
};
