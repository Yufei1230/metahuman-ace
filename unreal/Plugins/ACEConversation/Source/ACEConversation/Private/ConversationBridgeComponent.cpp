#include "ConversationBridgeComponent.h"
#include "ACEConversationLog.h"

#include "Dom/JsonObject.h"
#include "IWebSocket.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "WebSocketsModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
static constexpr ANSICHAR AceMagic[4] = {'A', 'C', 'E', '1'};
static constexpr uint8 FrameVersion = 1;
static constexpr uint8 MicKind = 1;
static constexpr uint8 TtsKind = 2;
static constexpr uint8 PcmS16Le = 1;
static constexpr int32 HeaderSize = 32;
}

UConversationBridgeComponent::UConversationBridgeComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
}

void UConversationBridgeComponent::Connect()
{
    if (Socket.IsValid())
    {
        return;
    }

    FWebSocketsModule& WebSocketsModule = FModuleManager::LoadModuleChecked<FWebSocketsModule>(TEXT("WebSockets"));
    Socket = WebSocketsModule.CreateWebSocket(ServerUrl);

    Socket->OnConnected().AddUObject(this, &UConversationBridgeComponent::HandleConnected);
    Socket->OnConnectionError().AddUObject(this, &UConversationBridgeComponent::HandleConnectionError);
    Socket->OnClosed().AddUObject(this, &UConversationBridgeComponent::HandleClosed);
    Socket->OnMessage().AddUObject(this, &UConversationBridgeComponent::HandleTextMessage);
    Socket->OnBinaryMessage().AddUObject(this, &UConversationBridgeComponent::HandleBinaryMessage);
    UE_LOG(LogACEConversation, Display, TEXT("[WebSocketConnecting] %s"), *ServerUrl);
    Socket->Connect();
}

void UConversationBridgeComponent::Disconnect()
{
    bSessionStarted = false;
    bSessionReady = false;
    BinaryMessage.Reset();
    bTtsAudioStarted = false;
    FTSTicker::GetCoreTicker().RemoveTicker(EndTicker);
    EndTicker.Reset();
    ActiveTurnId.Invalidate();
    if (Socket.IsValid())
    {
        Socket->OnConnected().RemoveAll(this);
        Socket->OnConnectionError().RemoveAll(this);
        Socket->OnClosed().RemoveAll(this);
        Socket->OnMessage().RemoveAll(this);
        Socket->OnBinaryMessage().RemoveAll(this);
        Socket->Close();
        Socket.Reset();
    }
}

void UConversationBridgeComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Disconnect();
    Super::EndPlay(EndPlayReason);
}

bool UConversationBridgeComponent::SendWavFile(const FString& FilePath)
{
    const FString Path = FPaths::IsRelative(FilePath) ? FPaths::Combine(FPaths::ProjectDir(), FilePath) : FilePath;
    TArray<uint8> Wav;
    if (!FFileHelper::LoadFileToArray(Wav, *Path) || Wav.Num() < 12 ||
        FMemory::Memcmp(Wav.GetData(), "RIFF", 4) || FMemory::Memcmp(Wav.GetData() + 8, "WAVE", 4))
    {
        OnError.Broadcast(TEXT("invalid_wav"), TEXT("Cannot read a RIFF/WAVE file"));
        return false;
    }
    const auto Read16 = [](const uint8* P) { return uint16(P[0]) | (uint16(P[1]) << 8); };
    UE_LOG(LogACEConversation, Display, TEXT("[WavOpened] Path=\"%s\" FileBytes=%d"), *Path, Wav.Num());
    const auto Read32 = [](const uint8* P) { return uint32(P[0]) | (uint32(P[1]) << 8) | (uint32(P[2]) << 16) | (uint32(P[3]) << 24); };
    bool bValidFormat = false;
    TArray<uint8> Pcm;
    const uint64 RiffEnd = uint64(Read32(Wav.GetData() + 4)) + 8;
    if (RiffEnd > uint64(Wav.Num()))
    {
        OnError.Broadcast(TEXT("invalid_wav"), TEXT("Truncated RIFF file"));
        return false;
    }
    for (uint64 Offset = 12; Offset + 8 <= RiffEnd;)
    {
        const uint8* Chunk = Wav.GetData() + Offset;
        const uint32 Length = Read32(Chunk + 4);
        if (Offset + 8 + Length > RiffEnd)
        {
            OnError.Broadcast(TEXT("invalid_wav"), TEXT("Truncated WAV chunk"));
            return false;
        }
        const uint8* Data = Chunk + 8;
        if (!FMemory::Memcmp(Chunk, "fmt ", 4) && Length >= 16)
        {
            bValidFormat = Read16(Data) == 1 && Read16(Data + 2) == 1 &&
                Read32(Data + 4) == 16000 && Read16(Data + 12) == 2 && Read16(Data + 14) == 16;
        }
        if (!FMemory::Memcmp(Chunk, "data", 4))
        {
            Pcm.Append(Data, int32(Length));
        }
        Offset += 8 + uint64(Length) + (Length & 1);
    }
    if (!bValidFormat)
    {
        OnError.Broadcast(TEXT("invalid_wav"), TEXT("Expected uncompressed 16000 Hz mono PCM16 WAV"));
        return false;
    }
    return SendPcm16Audio(Pcm);
}

bool UConversationBridgeComponent::SendPcm16Audio(const TArray<uint8>& Pcm16Mono)
{
    if (!bSessionReady || !Socket.IsValid() || !Socket->IsConnected())
    {
        OnError.Broadcast(TEXT("not_ready"), TEXT("Wait for OnSessionReady or LISTENING before sending audio"));
        return false;
    }
    if (Pcm16Mono.IsEmpty() || Pcm16Mono.Num() % 2)
    {
        OnError.Broadcast(TEXT("invalid_pcm"), TEXT("Expected nonempty 16000 Hz mono PCM16 bytes"));
        return false;
    }
    for (int64 Offset = 0; Offset < Pcm16Mono.Num(); Offset += 640)
    {
        TArray<uint8> Chunk;
        Chunk.Append(Pcm16Mono.GetData() + Offset, int32(FMath::Min<int64>(640, Pcm16Mono.Num() - Offset)));
        Chunk.SetNumZeroed(640);
        PushMicChunk(Chunk);
    }
    bSessionReady = false;
    SendMicEnd();
    UE_LOG(LogACEConversation, Display, TEXT("[AudioInputSent] PCMBytes=%d Frames=%d Rate=16000 Hz; mic.end sent"), Pcm16Mono.Num(), (Pcm16Mono.Num() + 639) / 640);
    return true;
}

void UConversationBridgeComponent::PushMicChunk(const TArray<uint8>& Pcm16Mono)
{
    if (!Socket.IsValid() || !Socket->IsConnected() || !bSessionStarted || Pcm16Mono.IsEmpty())
    {
        return;
    }
    const TArray<uint8> Frame = BuildMicAudioFrame(Pcm16Mono);
    Socket->Send(Frame.GetData(), Frame.Num(), true);
}

void UConversationBridgeComponent::SendMicEnd()
{
    if (!Socket.IsValid() || !Socket->IsConnected() || !bSessionStarted)
    {
        return;
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("type"), TEXT("mic.end"));
    Root->SetStringField(TEXT("session_id"), SessionId.ToString(EGuidFormats::DigitsWithHyphensLower));
    Root->SetStringField(TEXT("timestamp"), FDateTime::UtcNow().ToIso8601());
    Root->SetObjectField(TEXT("payload"), MakeShared<FJsonObject>());

    FString Json;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
    FJsonSerializer::Serialize(Root, Writer);
    Socket->Send(Json);
}

void UConversationBridgeComponent::HandleConnected()
{
    UE_LOG(LogACEConversation, Display, TEXT("[WebSocketConnected] %s"), *ServerUrl);
    SessionId = FGuid::NewGuid();
    bSessionStarted = true;
    SendSessionStart();
}

void UConversationBridgeComponent::HandleConnectionError(const FString& ErrorMessage)
{
    UE_LOG(LogACEConversation, Error, TEXT("[WebSocketError] %s"), *ErrorMessage);
    bSessionReady = false;
    OnError.Broadcast(TEXT("connection_error"), ErrorMessage);
}

void UConversationBridgeComponent::HandleClosed(int32 StatusCode, const FString& Reason, bool bWasClean)
{
    UE_LOG(LogACEConversation, Warning, TEXT("[WebSocketClosed] Code=%d Clean=%d Reason=%s"), StatusCode, bWasClean, *Reason);
    bSessionStarted = false;
    bSessionReady = false;
    BinaryMessage.Reset();
    ActiveTurnId.Invalidate();
    OnError.Broadcast(TEXT("connection_closed"), Reason);
}

void UConversationBridgeComponent::HandleTextMessage(const FString& Message)
{
    UE_LOG(LogACEConversation, Verbose, TEXT("[Control] %s"), *Message);
    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Message);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        return;
    }

    FString Type;
    if (!Root->TryGetStringField(TEXT("type"), Type))
    {
        return;
    }
    const TSharedPtr<FJsonObject>* Payload = nullptr;
    Root->TryGetObjectField(TEXT("payload"), Payload);

    FString TurnIdString;
    Root->TryGetStringField(TEXT("turn_id"), TurnIdString);
    if (!TurnIdString.IsEmpty())
    {
        FGuid::Parse(TurnIdString, ActiveTurnId);
    }

    if (Type == TEXT("state") && Payload && Payload->IsValid())
    {
        const FString StateString = (*Payload)->GetStringField(TEXT("state"));
        UE_LOG(LogACEConversation, Display, TEXT("[State] %s"), *StateString);
        if (StateString == TEXT("LISTENING"))
        {
            bSessionReady = true;
            OnStateChanged.Broadcast(EACEConversationState::Listening);
            FString Reason;
            if ((*Payload)->TryGetStringField(TEXT("reason"), Reason) && Reason == TEXT("ready"))
            {
                OnSessionReady.Broadcast();
            }
        }
        else if (StateString == TEXT("THINKING"))
        {
            bSessionReady = false;
            OnStateChanged.Broadcast(EACEConversationState::Thinking);
        }
        else if (StateString == TEXT("SPEAKING"))
        {
            bSessionReady = false;
            OnStateChanged.Broadcast(EACEConversationState::Speaking);
        }
        return;
    }

    if ((Type == TEXT("asr.partial") || Type == TEXT("asr.final") || Type == TEXT("llm.delta")) && Payload && Payload->IsValid())
    {
        const FString Text = (*Payload)->GetStringField(TEXT("text"));
        if (Type == TEXT("asr.partial"))
        {
            OnAsrPartial.Broadcast(Text);
        }
        else if (Type == TEXT("asr.final"))
        {
            OnAsrFinal.Broadcast(Text);
        }
        else
        {
            OnLlmDelta.Broadcast(Text);
        }
        return;
    }

    if (Type == TEXT("tts.start") && Payload && Payload->IsValid())
    {
        // UE dispatches text and binary via separate queues. Start on the first
        // binary frame so an early audio callback cannot be erased by this text event.
        return;
    }

    if (Type == TEXT("tts.end"))
    {
        const uint64 EndFrame = GFrameCounter;
        FTSTicker::GetCoreTicker().RemoveTicker(EndTicker);
        EndTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this,
            [this, EndFrame](float)
            {
                // Let the WebSocket manager finish draining its binary queue.
                if (GFrameCounter <= EndFrame || !BinaryMessage.IsEmpty()) { return true; }
                EndTicker.Reset();
                bTtsAudioStarted = false;
                OnTtsEnded.Broadcast();
                return false;
            }));
        return;
    }

    if (Type == TEXT("error") && Payload && Payload->IsValid())
    {
        OnError.Broadcast((*Payload)->GetStringField(TEXT("code")), (*Payload)->GetStringField(TEXT("message")));
    }
}

void UConversationBridgeComponent::HandleBinaryMessage(const void* Data, SIZE_T Size, bool bIsLastFragment)
{
    if (Size > 1024 * 1024 || SIZE_T(BinaryMessage.Num()) + Size > 1024 * 1024)
    {
        Disconnect();
        OnError.Broadcast(TEXT("invalid_audio_frame"), TEXT("Audio message exceeds 1 MiB"));
        return;
    }
    BinaryMessage.Append(static_cast<const uint8*>(Data), int32(Size));
    if (!bIsLastFragment) { return; }
    int32 SampleRateHz = 0;
    TArray<uint8> Payload;
    if (!ParseTtsAudioFrame(BinaryMessage.GetData(), BinaryMessage.Num(), SampleRateHz, Payload))
    {
        BinaryMessage.Reset();
        OnError.Broadcast(TEXT("invalid_audio_frame"), TEXT("Invalid ACE1 TTS frame"));
        return;
    }
    const uint8* Id = BinaryMessage.GetData() + 16;
    ActiveTurnId = FGuid(ReadUint32BE(Id), ReadUint32BE(Id + 4), ReadUint32BE(Id + 8), ReadUint32BE(Id + 12));
    BinaryMessage.Reset();
    if (!bTtsAudioStarted)
    {
        bTtsAudioStarted = true;
        OnTtsStarted.Broadcast(SampleRateHz);
    }
    OnTtsAudioChunk.Broadcast(Payload, SampleRateHz, ActiveTurnId);
}

void UConversationBridgeComponent::SendSessionStart()
{
    if (!Socket.IsValid() || !Socket->IsConnected())
    {
        return;
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("type"), TEXT("session.start"));
    Root->SetStringField(TEXT("session_id"), SessionId.ToString(EGuidFormats::DigitsWithHyphensLower));
    Root->SetStringField(TEXT("timestamp"), FDateTime::UtcNow().ToIso8601());

    TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("locale"), Locale);
    Root->SetObjectField(TEXT("payload"), Payload);

    FString Json;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
    FJsonSerializer::Serialize(Root, Writer);
    Socket->Send(Json);
    UE_LOG(LogACEConversation, Display, TEXT("[SessionStartSent] Locale=%s Session=%s"), *Locale, *SessionId.ToString());
}

TArray<uint8> UConversationBridgeComponent::BuildMicAudioFrame(const TArray<uint8>& Pcm16Mono) const
{
    TArray<uint8> Buffer;
    Buffer.Reserve(HeaderSize + Pcm16Mono.Num());

    Buffer.Append(reinterpret_cast<const uint8*>(AceMagic), 4);
    Buffer.Add(FrameVersion);
    Buffer.Add(MicKind);
    Buffer.Add(PcmS16Le);
    Buffer.Add(1);
    AppendUint32BE(Buffer, 16000);
    AppendUint32BE(Buffer, static_cast<uint32>(Pcm16Mono.Num()));
    Buffer.AddZeroed(16);
    Buffer.Append(Pcm16Mono);
    return Buffer;
}

bool UConversationBridgeComponent::ParseTtsAudioFrame(const void* Data, SIZE_T Size, int32& OutSampleRateHz, TArray<uint8>& OutPayload)
{
    if (Size < HeaderSize)
    {
        return false;
    }

    const uint8* Bytes = static_cast<const uint8*>(Data);
    if (FMemory::Memcmp(Bytes, AceMagic, 4) != 0)
    {
        return false;
    }
    if (Bytes[4] != FrameVersion || Bytes[5] != TtsKind || Bytes[6] != PcmS16Le || Bytes[7] != 1)
    {
        return false;
    }

    const uint32 SampleRate = ReadUint32BE(Bytes + 8);
    const uint32 PayloadSize = ReadUint32BE(Bytes + 12);
    if (uint64(HeaderSize) + PayloadSize != Size || PayloadSize % 2 || SampleRate < 8000 || SampleRate > 192000)
    {
        return false;
    }

    OutSampleRateHz = static_cast<int32>(SampleRate);
    OutPayload.SetNumUninitialized(static_cast<int32>(PayloadSize));
    FMemory::Memcpy(OutPayload.GetData(), Bytes + HeaderSize, PayloadSize);
    return true;
}

void UConversationBridgeComponent::AppendUint32BE(TArray<uint8>& Buffer, uint32 Value)
{
    Buffer.Add(static_cast<uint8>((Value >> 24) & 0xFF));
    Buffer.Add(static_cast<uint8>((Value >> 16) & 0xFF));
    Buffer.Add(static_cast<uint8>((Value >> 8) & 0xFF));
    Buffer.Add(static_cast<uint8>(Value & 0xFF));
}

uint32 UConversationBridgeComponent::ReadUint32BE(const uint8* Bytes)
{
    return (static_cast<uint32>(Bytes[0]) << 24)
        | (static_cast<uint32>(Bytes[1]) << 16)
        | (static_cast<uint32>(Bytes[2]) << 8)
        | static_cast<uint32>(Bytes[3]);
}
