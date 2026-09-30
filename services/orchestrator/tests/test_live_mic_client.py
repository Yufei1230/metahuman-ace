from __future__ import annotations

import asyncio
import json
import queue
import tempfile
import unittest
import wave
from pathlib import Path
from uuid import uuid4
from unittest.mock import MagicMock, patch

import numpy as np

from app.protocol import AudioFrameKind, pack_audio_frame, unpack_audio_frame
from tools.live_mic_client import Microphone, PcmConverter, Response, publisher_url, receive_response, send_capture


class ConversionTests(unittest.TestCase):
    def convert(self, data, rate, block):
        converter = PcmConverter(rate, data.shape[1])
        chunks = []
        for offset in range(0, len(data), block):
            chunks += converter.feed(data[offset:offset + block])
        chunks += converter.finish()
        self.assertTrue(all(len(chunk) == 640 for chunk in chunks))
        return b"".join(chunks)

    def test_little_endian_extrema_nonfinite_padding(self):
        pcm = self.convert(np.array([[-2], [0], [2], [np.nan]], dtype=np.float32), 16000, 3)
        self.assertEqual(pcm[:8], b"\x00\x80\x00\x00\xff\x7f\x00\x00")
        self.assertEqual(pcm[8:], bytes(632))

    def test_continuity_duration_and_downmix(self):
        for rate in (16000, 44100, 48000):
            with self.subTest(rate=rate):
                tone = (0.5 * np.sin(2 * np.pi * 1000 * np.arange(rate) / rate)).astype(np.float32)
                stereo = np.column_stack([tone, tone])
                small = self.convert(stereo, rate, 137)
                whole = self.convert(stereo, rate, rate)
                self.assertEqual(len(small), 32000)
                self.assertEqual(small, whole)
                silence = self.convert(np.column_stack([tone, -tone]), rate, 931)
                self.assertEqual(silence, bytes(32000))

    def test_antialias(self):
        rms = []
        for frequency in (1000, 12000):
            tone = (0.5 * np.sin(2 * np.pi * frequency * np.arange(48000) / 48000)).astype(np.float32)
            pcm = self.convert(tone[:, None], 48000, 937)
            samples = np.frombuffer(pcm, dtype="<i2")[500:-500] / 32768
            rms.append(float(np.sqrt(np.mean(samples ** 2))))
        self.assertGreater(rms[0], 0.3)
        self.assertLess(rms[1], 0.02)

    def test_invalid_shape_and_double_flush(self):
        converter = PcmConverter(48000, 2)
        with self.assertRaises(ValueError):
            converter.feed(np.zeros((32, 1)))
        converter.finish()
        with self.assertRaises(ValueError):
            converter.finish()

    def test_url(self):
        self.assertEqual(publisher_url("ws://host:8080/ws/session", "mac"), "ws://host:8080/ws/session?publish=mac")
        with self.assertRaises(ValueError):
            publisher_url("ws://host/ws/session?observe=mac", None)


class CaptureAdapterTests(unittest.TestCase):
    def test_default_device_callback_copies_and_bounds_queue(self):
        driver = MagicMock()
        driver.query_devices.return_value = {"name": "SIMULATED input", "default_samplerate": 48000}
        with patch.dict("sys.modules", {"sounddevice": driver}):
            microphone = Microphone()
        driver.query_devices.assert_called_once_with(None, "input")
        params = driver.InputStream.call_args.kwargs
        self.assertEqual((params["samplerate"], params["channels"], params["blocksize"]), (48000, 1, 960))
        block = np.full((960, 1), 0.25, dtype=np.float32)
        params["callback"](block, 960, None, None)
        block[:] = 0
        self.assertTrue(np.all(microphone.blocks.get_nowait() == 0.25))
        for _ in range(101):
            params["callback"](block, 960, None, None)
        self.assertIn("overflow", microphone.failure)
        self.assertEqual(microphone.blocks.qsize(), 100)
        microphone.start()
        microphone.stop()
        microphone.close()
        driver.InputStream.return_value.close.assert_called_once()

    def test_capture_status_propagates(self):
        driver = MagicMock()
        driver.query_devices.return_value = {"name": "SIMULATED input", "default_samplerate": 44100}
        with patch.dict("sys.modules", {"sounddevice": driver}):
            microphone = Microphone(channels=2)
        driver.InputStream.call_args.kwargs["callback"](np.zeros((882, 2)), 882, None, "input overflow")
        self.assertIn("input overflow", microphone.failure)
        self.assertTrue(microphone.blocks.empty())


class FakeMic:
    rate, channels, failure = 48000, 2, ""
    def __init__(self):
        self.blocks = queue.Queue()
        self.blocks.put(np.ones((960, 2), dtype=np.float32) * 0.125)
        self.stopped = False
    def start(self):
        pass
    def stop(self):
        self.stopped = True


class Socket:
    close_code, close_reason = 1000, "test close"
    def __init__(self, incoming=()):
        self.sent = []
        self.incoming = list(incoming)
    async def send(self, message):
        self.sent.append(message)
    def __aiter__(self):
        return self
    async def __anext__(self):
        if not self.incoming:
            raise StopAsyncIteration
        return self.incoming.pop(0)


class CaptureTests(unittest.IsolatedAsyncioTestCase):
    async def test_simulated_capture_exact_ace1_and_end(self):
        mic, socket, stop = FakeMic(), Socket(), asyncio.Event()
        stop.set()
        totals = [0, 0]
        await send_capture(socket, mic, uuid4(), stop, asyncio.Event(), 1, totals)
        self.assertTrue(mic.stopped)
        self.assertEqual(totals, [1, 640])
        self.assertEqual(socket.sent[0][:16], bytes.fromhex("414345310101010100003e8000000280"))
        self.assertEqual(socket.sent[0][16:32], bytes(16))
        frame = unpack_audio_frame(socket.sent[0])
        self.assertEqual(frame.kind, AudioFrameKind.MIC)
        self.assertEqual(len(frame.payload), 640)
        self.assertEqual(json.loads(socket.sent[-1])["type"], "mic.end")

    async def test_overflow_fails_and_stops(self):
        mic = FakeMic()
        mic.failure = "overflow"
        with self.assertRaisesRegex(RuntimeError, "overflow"):
            await send_capture(Socket(), mic, uuid4(), asyncio.Event(), asyncio.Event(), 1, [0, 0])
        self.assertTrue(mic.stopped)

    async def test_server_endpoint_discards_pending_pcm(self):
        stop = asyncio.Event()
        stop.set()
        socket = Socket()
        await send_capture(socket, FakeMic(), uuid4(), stop, stop, 1, [0, 0])
        self.assertEqual(len(socket.sent), 1)
        self.assertEqual(json.loads(socket.sent[0])["type"], "mic.end")

    async def test_response_wav_rate_and_events(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "response.wav"
            ready, stop, server_stop = asyncio.Event(), asyncio.Event(), asyncio.Event()
            pcm = b"\x12\x34" * 100
            messages = [json.dumps({"type": "state", "payload": {"state": "LISTENING", "reason": "ready"}}),
                        json.dumps({"type": "llm.delta", "payload": {"text": "Hello."}}),
                        pack_audio_frame(kind=AudioFrameKind.TTS, sample_rate_hz=44100, channels=1, payload=pcm, turn_id=uuid4()),
                        json.dumps({"type": "tts.end"}),
                        json.dumps({"type": "state", "payload": {"state": "LISTENING", "reason": "turn_complete"}})]
            response = Response(output)
            await receive_response(Socket(messages), response, ready, stop, server_stop)
            self.assertTrue(ready.is_set() and stop.is_set() and server_stop.is_set())
            self.assertEqual(response.parts, ["Hello."])
            with wave.open(str(output), "rb") as wav:
                self.assertEqual(wav.getframerate(), 44100)
                self.assertEqual(wav.readframes(100), pcm)

    async def test_disconnect_and_backend_error(self):
        for messages, expected in [([], "disconnected"), ([json.dumps({"type": "error", "payload": {"code": "oops"}})], "backend error")]:
            with self.assertRaisesRegex(RuntimeError, expected):
                await receive_response(Socket(messages), Response(Path("unused.wav")), asyncio.Event(), asyncio.Event(), asyncio.Event())

    def test_rate_change_rejected(self):
        response = Response(Path("unused.wav"))
        turn = uuid4()
        response.binary(pack_audio_frame(kind=AudioFrameKind.TTS, sample_rate_hz=24000, channels=1, payload=b"\0\0", turn_id=turn))
        with self.assertRaises(ValueError):
            response.binary(pack_audio_frame(kind=AudioFrameKind.TTS, sample_rate_hz=44100, channels=1, payload=b"\0\0", turn_id=turn))


if __name__ == "__main__":
    unittest.main()
