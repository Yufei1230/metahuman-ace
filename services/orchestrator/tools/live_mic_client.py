#!/usr/bin/env python3
"""One push-to-talk turn using the existing ACE protocol; no server SDK needed."""
from __future__ import annotations

import argparse
import asyncio
import json
import queue
import sys
import threading
import time
import wave
from pathlib import Path
from urllib.parse import parse_qsl, urlencode, urlsplit, urlunsplit
from uuid import uuid4

import numpy as np
import soxr
import websockets

# Allow `python3 tools/live_mic_client.py` from a source checkout without
# installing the orchestrator (and its server/provider dependencies) on the Mac.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from app.protocol import AudioFrameKind, PCM_S16LE, pack_audio_frame, serialize_control_message, unpack_audio_frame


def log(message: str) -> None:
    print(message, flush=True)


class PcmConverter:
    """Continuous anti-alias resampling; pad only the final 20 ms frame."""
    def __init__(self, rate: int, channels: int) -> None:
        if not 8000 <= rate <= 192000 or not 1 <= channels <= 32:
            raise ValueError("unsupported capture rate/channel count")
        self.channels = channels
        self.resampler = soxr.ResampleStream(rate, 16000, 1, dtype="float32", quality="HQ") if rate != 16000 else None
        self.pending = bytearray()
        self.finished = False

    def feed(self, samples, *, last: bool = False) -> list[bytes]:
        if self.finished:
            raise ValueError("converter already flushed")
        data = np.asarray(samples, dtype=np.float32)
        if data.ndim != 2 or data.shape[1] != self.channels:
            raise ValueError("expected frames x channels float samples")
        data = np.clip(np.nan_to_num(data, nan=0.0, posinf=1.0, neginf=-1.0), -1, 1)
        mono = data.mean(axis=1, dtype=np.float32)
        if self.resampler is not None:
            mono = self.resampler.resample_chunk(mono, last=last)
        mono = np.clip(mono, -1, 1)
        pcm = np.rint(mono * np.where(mono < 0, 32768, 32767)).astype("<i2")
        self.pending.extend(pcm.tobytes())
        if last:
            self.finished = True
            if len(self.pending) % 640:
                self.pending.extend(b"\0" * (640 - len(self.pending) % 640))
        frames = []
        while len(self.pending) >= 640:
            frames.append(bytes(self.pending[:640]))
            del self.pending[:640]
        return frames

    def finish(self) -> list[bytes]:
        return self.feed(np.empty((0, self.channels), dtype=np.float32), last=True)


class Microphone:
    def __init__(self, device=None, channels: int = 1) -> None:
        import sounddevice as sd
        info = sd.query_devices(device, "input")
        self.rate = int(info["default_samplerate"])
        self.channels = channels
        self.blocks: queue.Queue = queue.Queue(maxsize=100)  # <= 2 seconds
        self.failure = ""

        def callback(data, frames, timing, status):
            if status:
                self.failure = f"microphone callback: {status}; audio discarded"
                return
            try:
                self.blocks.put_nowait(data.copy())
            except queue.Full:
                self.failure = "microphone queue overflow: network/consumer too slow"

        self.stream = sd.InputStream(device=device, samplerate=self.rate, channels=channels,
                                     dtype="float32", blocksize=max(1, self.rate // 50), callback=callback)
        log(f"microphone opened: {info['name']} rate={self.rate} channels={channels} float32")

    def start(self):
        self.stream.start()
        log("capture started")

    def stop(self):
        self.stream.stop()

    def close(self):
        self.stream.close()


class WavMicrophone:
    """Explicit prerecorded test source, paced like capture; never a mic test."""
    def __init__(self, path: Path):
        self.wav = wave.open(str(path), "rb")
        self.rate, self.channels = self.wav.getframerate(), self.wav.getnchannels()
        if self.wav.getsampwidth() != 2 or self.wav.getcomptype() != "NONE":
            self.wav.close()
            raise ValueError("test WAV must be uncompressed PCM16")
        self.blocks = queue.Queue(maxsize=100)
        self.failure = ""
        self.finished = False
        self.task = None
        log(f"SIMULATED microphone: WAV={path} rate={self.rate} channels={self.channels}; no hardware capture")

    def start(self):
        async def feed():
            while True:
                raw = self.wav.readframes(self.rate // 50)
                if not raw:
                    self.finished = True
                    return
                data = np.frombuffer(raw, dtype="<i2").astype(np.float32).reshape(-1, self.channels) / 32768
                try:
                    self.blocks.put_nowait(data)
                except queue.Full:
                    self.failure = "simulated capture queue overflow"
                    return
                await asyncio.sleep(len(data) / self.rate)
        self.task = asyncio.create_task(feed())
        log("capture started (SIMULATED WAV source)")

    def stop(self):
        if self.task:
            self.task.cancel()

    def close(self):
        self.stop()
        self.wav.close()


async def enter(prompt: str) -> None:
    # A daemon avoids asyncio's executor shutdown hanging on input() after a
    # network error, automatic server endpointing, or Ctrl+C.
    loop = asyncio.get_running_loop()
    result = loop.create_future()
    def complete(error):
        if not result.done():
            if error:
                result.set_exception(error)
            else:
                result.set_result(None)
    def read():
        error = None
        try:
            input(prompt)
        except EOFError:
            error = RuntimeError("stdin closed; run in an interactive terminal")
        try:
            loop.call_soon_threadsafe(complete, error)
        except RuntimeError:
            pass  # loop already closed after disconnect
    threading.Thread(target=read, daemon=True).start()
    await result


async def send_frames(websocket, frames, totals):
    for frame in frames:
        await websocket.send(pack_audio_frame(kind=AudioFrameKind.MIC, sample_rate_hz=16000, channels=1, payload=frame))
        totals[0] += 1
        totals[1] += len(frame)
        if totals[0] == 1 or totals[0] % 50 == 0:
            log(f"PCM sent: chunks={totals[0]} bytes={totals[1]} rate=16000 mono PCM16LE")


async def send_capture(websocket, microphone, session_id, stop, server_stopped, max_seconds, totals):
    converter = PcmConverter(microphone.rate, microphone.channels)
    started = time.monotonic()
    last_block = started
    microphone.start()
    try:
        while not stop.is_set() and time.monotonic() - started < max_seconds:
            if microphone.failure:
                raise RuntimeError(microphone.failure)
            try:
                block = microphone.blocks.get_nowait()
            except queue.Empty:
                if getattr(microphone, "finished", False):
                    break
                if time.monotonic() - last_block > 5:
                    raise RuntimeError("microphone started but delivered no PCM for five seconds")
                await asyncio.sleep(0.01)
                continue
            last_block = time.monotonic()
            await send_frames(websocket, converter.feed(block), totals)
    finally:
        microphone.stop()
    if microphone.failure:
        raise RuntimeError(microphone.failure)
    if not server_stopped.is_set():
        while not microphone.blocks.empty():
            await send_frames(websocket, converter.feed(microphone.blocks.get_nowait()), totals)
        await send_frames(websocket, converter.finish(), totals)
    else:
        log("server ended the utterance; remaining capture discarded")
    await websocket.send(serialize_control_message(event_type="mic.end", session_id=session_id, turn_id=None))
    log(f"mic.end sent; PCM chunks={totals[0]} bytes={totals[1]}")


class Response:
    def __init__(self, output: Path):
        self.output = output
        self.audio = bytearray()
        self.rate = None
        self.turn_id = None
        self.parts = []
        self.chunks = 0
        self.saved = False

    def binary(self, raw: bytes):
        frame = unpack_audio_frame(raw)
        if (frame.kind != AudioFrameKind.TTS or frame.codec != PCM_S16LE or frame.channels != 1
                or not 8000 <= frame.sample_rate_hz <= 192000 or len(frame.payload) % 2):
            raise ValueError("unsupported TTS audio format")
        if self.rate is not None and (self.rate != frame.sample_rate_hz or self.turn_id != frame.turn_id):
            raise ValueError("TTS rate/turn changed within response")
        if len(self.audio) + len(frame.payload) > 64 * 1024 * 1024:
            raise ValueError("TTS response exceeds 64 MiB limit")
        self.rate, self.turn_id = frame.sample_rate_hz, frame.turn_id
        self.audio.extend(frame.payload)
        self.chunks += 1
        log(f"TTS audio: chunk={self.chunks} bytes={len(frame.payload)} total={len(self.audio)} rate={self.rate}")

    def save(self):
        if self.audio and not self.saved:
            self.output.parent.mkdir(parents=True, exist_ok=True)
            with wave.open(str(self.output), "wb") as wav:
                wav.setnchannels(1)
                wav.setsampwidth(2)
                wav.setframerate(self.rate)
                wav.writeframes(self.audio)
            self.saved = True
            log(f"TTS WAV saved: {self.output} bytes={len(self.audio)} rate={self.rate}")


async def receive_response(websocket, response, ready, stop, server_stopped):
    async for message in websocket:
        if isinstance(message, bytes):
            response.binary(message)
            continue
        event = json.loads(message)
        kind, payload = event.get("type"), event.get("payload", {})
        log(f"{kind}: {json.dumps(payload, ensure_ascii=False)}")
        if kind == "error":
            raise RuntimeError(f"backend error: {payload}")
        if kind == "llm.delta":
            response.parts.append(payload["text"])
        if kind == "tts.end":
            response.save()
        if kind == "state":
            if payload.get("reason") == "ready" and payload.get("state") == "LISTENING":
                ready.set()
                log("session ready")
            elif payload.get("state") in ("THINKING", "SPEAKING") or payload.get("reason") == "turn_complete":
                server_stopped.set()
                stop.set()
            if payload.get("reason") == "turn_complete":
                response.save()
                log("LLM final response: " + "".join(response.parts))
                if not response.saved:
                    raise RuntimeError("turn completed without TTS audio; no WAV saved")
                return
    raise RuntimeError(f"disconnected before turn completed: code={websocket.close_code} reason={websocket.close_reason}")


def publisher_url(url: str, channel: str | None) -> str:
    parts = urlsplit(url)
    if parts.scheme not in ("ws", "wss") or parts.path != "/ws/session":
        raise ValueError("URL must be ws(s)://host:port/ws/session")
    query = dict(parse_qsl(parts.query))
    if "observe" in query:
        raise ValueError("microphone client cannot use an observer URL")
    if channel:
        query["publish"] = channel
    return urlunsplit(parts._replace(query=urlencode(query)))


async def run(args):
    ready, stop, server_stopped = asyncio.Event(), asyncio.Event(), asyncio.Event()
    response = Response(args.output)
    tasks = []
    microphone = None
    session_id = uuid4()
    async with websockets.connect(publisher_url(args.url, args.unreal_channel), max_size=1024 * 1024,
                                  ping_interval=20, ping_timeout=60, close_timeout=5) as websocket:
        log("websocket connected")
        await websocket.send(serialize_control_message(event_type="session.start", session_id=session_id,
                                                       turn_id=None, payload={"locale": args.locale}))
        receiver = asyncio.create_task(receive_response(websocket, response, ready, stop, server_stopped))
        tasks.append(receiver)

        async def guarded(awaitable, timeout=None):
            task = asyncio.create_task(awaitable)
            tasks.append(task)
            done, _ = await asyncio.wait([task, receiver], timeout=timeout, return_when=asyncio.FIRST_COMPLETED)
            if not done:
                raise TimeoutError("timed out waiting for session/response")
            if receiver in done:
                receiver.result()
            if task in done:
                return task.result()
            # A completed response can race the final mic.end. Let the sender
            # stop/flush cleanly rather than cancelling it on successful receive.
            return await asyncio.wait_for(task, 5)

        try:
            await guarded(ready.wait(), 30)
            stopper = None
            if args.test_wav:
                microphone = WavMicrophone(args.test_wav)
            else:
                await guarded(enter("Press Enter to start recording: "))
                microphone = Microphone(args.device, args.channels)
                stop_input = asyncio.create_task(enter("Press Enter to stop and send (server VAD may stop earlier): "))
                tasks.append(stop_input)
                async def stop_on_enter():
                    try:
                        await stop_input
                    finally:
                        stop.set()
                stopper = asyncio.create_task(stop_on_enter())
                tasks.append(stopper)
            totals = [0, 0]
            await guarded(send_capture(websocket, microphone, session_id, stop, server_stopped, args.max_seconds, totals), args.max_seconds + 30)
            if stopper and stopper.done():
                stopper.result()
            await asyncio.wait_for(asyncio.shield(receiver), args.timeout)
        finally:
            stop.set()
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            if microphone:
                microphone.close()
            await websocket.close()
            log(f"websocket disconnected: code={websocket.close_code} reason={websocket.close_reason}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="ws://127.0.0.1:8080/ws/session")
    parser.add_argument("--unreal-channel", help="Mirror responses to Unreal's ?observe=<channel> connection")
    parser.add_argument("--device", help="Input device index or name; default macOS input if omitted")
    parser.add_argument("--channels", type=int, default=1, help="Capture channels to average to mono")
    parser.add_argument("--list-devices", action="store_true")
    parser.add_argument("--max-seconds", type=float, default=30)
    parser.add_argument("--timeout", type=float, default=180, help="Response timeout after capture")
    parser.add_argument("--locale", default="en-US")
    parser.add_argument("--output", type=Path, default=Path("live-mic-response.wav"))
    parser.add_argument("--test-wav", type=Path, help="Explicit prerecorded PCM16 simulation, paced live; no microphone or Enter prompts")
    args = parser.parse_args()
    if not 0 < args.max_seconds <= 60 or args.timeout <= 0 or not 1 <= args.channels <= 32:
        parser.error("max-seconds must be (0,60], timeout positive, channels 1..32")
    if args.device and args.device.isdigit():
        args.device = int(args.device)
    try:
        if args.list_devices:
            import sounddevice as sd
            print(sd.query_devices())
            return
        asyncio.run(run(args))
    except KeyboardInterrupt:
        log("cancelled; microphone/socket closed")
    except Exception as exc:
        log(f"ERROR: {exc}")
        raise SystemExit(1)


if __name__ == "__main__":
    main()
