#!/usr/bin/env python3
from __future__ import annotations

import argparse
import asyncio
import json
import time
import uuid
import wave
from pathlib import Path

import websockets
import httpx

from app.protocol import AudioFrameKind, pack_audio_frame, unpack_audio_frame


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="ACE orchestrator demo client")
    parser.add_argument("--url", default="ws://127.0.0.1:8080/ws/session")
    parser.add_argument("--wav", type=Path, default=None, help="16kHz mono PCM16 WAV input")
    parser.add_argument("--mock-audio", action="store_true", help="Generate 1 second of synthetic speech frames")
    parser.add_argument("--realtime", action="store_true", help="Sleep 20ms between mic frames")
    parser.add_argument("--output", type=Path, default=Path("demo-output.wav"))
    parser.add_argument("--locale", default=None, help="Session metadata; server language is configured in .env")
    parser.add_argument("--timeout", type=float, default=180, help="Maximum seconds for a complete turn")
    parser.add_argument("--generate-input", action="store_true", help="Generate an English 16kHz test WAV with configured hosted TTS")
    parser.add_argument("--require-real", action="store_true", help="Fail if any server adapter is mocked or unhealthy")
    return parser


def read_input_frames(path: Path | None) -> list[bytes]:
    if path is None:
        sample = (1200).to_bytes(2, "little", signed=True) * 320
        return [sample for _ in range(50)]

    with wave.open(str(path), "rb") as handle:
        if (
            handle.getnchannels() != 1
            or handle.getsampwidth() != 2
            or handle.getframerate() != 16000
        ):
            raise ValueError("input WAV must be 16kHz mono PCM16")

        frame_samples = 320       # 20 ms at 16 kHz
        frame_bytes = 640         # 320 samples × 2 bytes

        chunks: list[bytes] = []

        while True:
            chunk = handle.readframes(frame_samples)

            if not chunk:
                break

            # WebRTC VAD requires an exact 20 ms frame.
            # Pad the final incomplete frame with silence.
            if len(chunk) < frame_bytes:
                chunk += b"\x00" * (frame_bytes - len(chunk))

            chunks.append(chunk)

        return chunks


async def run_demo(args: argparse.Namespace) -> None:
    from app.settings import Settings

    settings = Settings()
    if args.require_real:
        status_url = args.url.replace("ws://", "http://").replace("wss://", "https://").rsplit("/ws/session", 1)[0] + "/status"
        async with httpx.AsyncClient(timeout=60) as client:
            response = await client.get(status_url)
            response.raise_for_status()
            status = response.json()
        if status["status"] != "ok" or any(status["config"][key] for key in ("mock_asr", "mock_llm", "mock_tts")):
            raise RuntimeError("Real end-to-end test requires healthy non-mock adapters")
    if args.generate_input:
        from app.adapters.tts import NvidiaHostedTtsClient

        input_settings = settings.model_copy(update={"tts_sample_rate_hz": 16000})
        pcm = bytearray()
        async for chunk in NvidiaHostedTtsClient(input_settings).stream_text("Hello. Please introduce yourself in English in one short sentence."):
            pcm.extend(chunk)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.wav = args.output.with_name("demo-input.wav")
        with wave.open(str(args.wav), "wb") as handle:
            handle.setnchannels(1)
            handle.setsampwidth(2)
            handle.setframerate(16000)
            handle.writeframes(pcm)
        print(f"generated audio input {args.wav}")
    frames = read_input_frames(None if args.mock_audio else args.wav)
    if not frames:
        raise ValueError("no audio frames available")
    session_id = uuid.uuid4()
    tts_audio = bytearray()
    tts_rate = None
    transcript = ""
    llm_parts = []
    async with websockets.connect(
        args.url,
        max_size=None,
        ping_interval=None,
    ) as websocket:
        await websocket.send(
            json.dumps(
                {
                    "type": "session.start",
                    "session_id": str(session_id),
                    "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                    "payload": {"locale": args.locale or settings.assistant_language},
                },
                ensure_ascii=False,
            )
        )

        async def sender() -> None:
            for frame in frames:
                payload = pack_audio_frame(kind=AudioFrameKind.MIC, sample_rate_hz=16000, channels=1, payload=frame)
                await websocket.send(payload)
                if args.realtime:
                    await asyncio.sleep(0.02)
            await websocket.send(
                json.dumps(
                    {
                        "type": "mic.end",
                        "session_id": str(session_id),
                        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                        "payload": {},
                    },
                    ensure_ascii=False,
                )
            )

        async def receiver() -> None:
            nonlocal tts_rate, transcript
            while True:
                message = await websocket.recv()
                if isinstance(message, bytes):
                    frame = unpack_audio_frame(message)
                    if frame.kind is AudioFrameKind.TTS:
                        if tts_rate is not None and tts_rate != frame.sample_rate_hz:
                            raise RuntimeError("TTS sample rate changed during the turn")
                        tts_rate = frame.sample_rate_hz
                        tts_audio.extend(frame.payload)
                    continue
                event = json.loads(message)
                print(json.dumps(event, ensure_ascii=False))
                event_type = event.get("type")
                if event_type == "error":
                    raise RuntimeError(f"Backend error: {event.get('payload')}")
                if event_type == "asr.final":
                    transcript = event["payload"]["text"]
                if event_type == "llm.delta":
                    llm_parts.append(event["payload"]["text"])
                if (
                    event_type == "state"
                    and event.get("payload", {}).get("state") == "LISTENING"
                    and event.get("payload", {}).get("reason") == "turn_complete"
                ):
                    break

        await asyncio.wait_for(asyncio.gather(sender(), receiver()), timeout=args.timeout)

    if not transcript or not llm_parts or not tts_audio or not tts_rate:
        raise RuntimeError("Incomplete turn: expected ASR transcript, LLM response, and TTS audio")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(args.output), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(tts_rate)
        handle.writeframes(bytes(tts_audio))
    print(f"wrote {args.output}")
    summary = {"transcript": transcript, "response": "".join(llm_parts), "tts_sample_rate_hz": tts_rate, "tts_bytes": len(tts_audio), "input": str(args.wav), "output": str(args.output), "require_real": args.require_real}
    args.output.with_suffix(".json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary))


def main() -> None:
    parser = build_arg_parser()
    args = parser.parse_args()
    if sum((args.wav is not None, args.mock_audio, args.generate_input)) != 1:
        parser.error("choose exactly one of --wav, --mock-audio, or --generate-input")
    asyncio.run(run_demo(args))


if __name__ == "__main__":
    main()
