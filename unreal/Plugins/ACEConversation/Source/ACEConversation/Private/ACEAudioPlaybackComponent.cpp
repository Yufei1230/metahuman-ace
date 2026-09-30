#include "ACEAudioPlaybackComponent.h"
#include "ACEConversationLog.h"
#include "AudioDevice.h"

#include "Components/AudioComponent.h"
#include "GameFramework/Actor.h"
#include "Sound/SoundWaveProcedural.h"
#include "Engine/World.h"
#include "TimerManager.h"

UACEAudioPlaybackComponent::UACEAudioPlaybackComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
}

void UACEAudioPlaybackComponent::StartPlayback(int32 SampleRateHz)
{
    StopPlayback();
    LastPlaybackBytes = 0;
    EnsurePlaybackObjects(SampleRateHz);
}

void UACEAudioPlaybackComponent::PushPcm16(const TArray<uint8>& Pcm16Mono, int32 SampleRateHz)
{
    if (Pcm16Mono.IsEmpty() || Pcm16Mono.Num() % 2 || SampleRateHz <= 0)
    {
        UE_LOG(LogACEConversation, Error, TEXT("[PCMError] Invalid PCM bytes=%d rate=%d"), Pcm16Mono.Num(), SampleRateHz);
        return;
    }

    EnsurePlaybackObjects(SampleRateHz);
    PendingAudio.Append(Pcm16Mono);
}

void UACEAudioPlaybackComponent::EndPlayback()
{
    // tts.end means transmission ended, not that the sound finished playing.
    // Buffer one response for this minimal test, then play it without underruns.
    if (AudioComponent != nullptr && ProceduralWave != nullptr && !PendingAudio.IsEmpty())
    {
        LastPlaybackBytes = PendingAudio.Num();
        ProceduralWave->QueueAudio(PendingAudio.GetData(), PendingAudio.Num());
        UE_LOG(LogACEConversation, Display, TEXT("[PCMQueued] Bytes=%d Rate=%d Hz Duration=%.2f seconds"), LastPlaybackBytes, CurrentSampleRateHz, float(LastPlaybackBytes) / (2 * CurrentSampleRateHz));
        PendingAudio.Reset();
        AudioComponent->Play();
        UE_LOG(LogACEConversation, Display, TEXT("[PlaybackStarted] IsPlaying=%d AudioDevice=%d Volume=%.2f Spatialized=%d"), AudioComponent->IsPlaying(), GetWorld() && GetWorld()->GetAudioDevice().IsValid(), AudioComponent->VolumeMultiplier, AudioComponent->bAllowSpatialization);
        if (UWorld* World = GetWorld())
        {
            const float Duration = float(LastPlaybackBytes) / (2 * CurrentSampleRateHz);
            World->GetTimerManager().SetTimer(StopTimer, this, &UACEAudioPlaybackComponent::StopPlayback, Duration + 0.5f, false);
        }
    }
    else
    {
        UE_LOG(LogACEConversation, Warning, TEXT("[PlaybackEmpty] tts.end received without buffered playable PCM"));
    }
}

void UACEAudioPlaybackComponent::StopPlayback()
{
    if (UWorld* World = GetWorld()) { World->GetTimerManager().ClearTimer(StopTimer); }
    if (AudioComponent != nullptr)
    {
        if (AudioComponent->IsPlaying()) { UE_LOG(LogACEConversation, Display, TEXT("[PlaybackStopped] Audio only; PIE/session remain running")); }
        AudioComponent->Stop();
    }
    if (ProceduralWave != nullptr) { ProceduralWave->ResetAudio(); }
    PendingAudio.Reset();
}

void UACEAudioPlaybackComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    StopPlayback();
    if (AudioComponent != nullptr) { AudioComponent->DestroyComponent(); }
    Super::EndPlay(EndPlayReason);
}

void UACEAudioPlaybackComponent::EnsurePlaybackObjects(int32 SampleRateHz)
{
    if (AudioComponent != nullptr && ProceduralWave != nullptr && CurrentSampleRateHz == SampleRateHz)
    {
        return;
    }

    CurrentSampleRateHz = SampleRateHz;

    if (AudioComponent == nullptr)
    {
        AActor* Owner = GetOwner();
        if (Owner == nullptr)
        {
            return;
        }
        AudioComponent = NewObject<UAudioComponent>(Owner);
        AudioComponent->bAutoActivate = false;
        AudioComponent->bIsUISound = true;
        AudioComponent->bAllowSpatialization = false;
        AudioComponent->RegisterComponent();
        Owner->AddInstanceComponent(AudioComponent);
    }

    ProceduralWave = NewObject<USoundWaveProcedural>(this);
    ProceduralWave->SetSampleRate(SampleRateHz);
    ProceduralWave->NumChannels = 1;
    ProceduralWave->Duration = INDEFINITELY_LOOPING_DURATION;
    ProceduralWave->bLooping = false;
    AudioComponent->SetSound(ProceduralWave);
}
