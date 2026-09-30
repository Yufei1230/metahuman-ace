#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "AudioCaptureCore.h"
#include "ConversationMicrophoneComponent.generated.h"

class UConversationBridgeComponent;
struct FConversationCaptureState;

// Bounded push-to-talk capture. No audio is transmitted until StopListening.
UCLASS(ClassGroup=(ACE), meta=(BlueprintSpawnableComponent))
class ACECONVERSATION_API UConversationMicrophoneComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UConversationMicrophoneComponent();
    virtual ~UConversationMicrophoneComponent() override;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ACE|Microphone")
    TObjectPtr<UConversationBridgeComponent> Bridge;
    // -1 selects the Windows default recording device. Indices are logged on start.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ACE|Microphone", meta=(ClampMin="-1"))
    int32 DeviceIndex = -1;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ACE|Microphone", meta=(ClampMin="1", ClampMax="60"))
    float MaxCaptureSeconds = 30.0f;
    UPROPERTY(BlueprintReadOnly, Category="ACE|Microphone")
    FString DeviceName;
    UPROPERTY(BlueprintReadOnly, Category="ACE|Microphone")
    int32 CaptureSampleRate = 0;
    UPROPERTY(BlueprintReadOnly, Category="ACE|Microphone")
    int32 CaptureChannels = 0;
    UPROPERTY(BlueprintReadOnly, Category="ACE|Microphone")
    int32 CapturedBytes = 0;
    UPROPERTY(BlueprintReadOnly, Category="ACE|Microphone")
    int32 ConvertedBytes = 0;

    UFUNCTION(BlueprintCallable, Category="ACE|Microphone")
    bool StartListening();
    UFUNCTION(BlueprintCallable, Category="ACE|Microphone", meta=(DisplayName="Stop Listening / Send Turn"))
    bool StopListening();
    UFUNCTION(BlueprintPure, Category="ACE|Microphone")
    bool IsListening() const { return bListening; }
    // Discard without sending on disconnect, EndPlay or error.
    UFUNCTION(BlueprintCallable, Category="ACE|Microphone")
    void CancelCapture();

    static bool ConvertToPcm16(const TArray<float>& Interleaved, int32 Rate, int32 Channels, TArray<uint8>& OutPcm);
protected:
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void BeginDestroy() override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction) override;
private:
    TUniquePtr<Audio::FAudioCapture> Capture;
    TSharedPtr<FConversationCaptureState, ESPMode::ThreadSafe> State;
    bool bListening = false;
    double StartedAt = 0;
    float CaptureLimitSeconds = 30;
    void CloseCapture();
    bool Error(const FString& Code, const FString& Message);
};
