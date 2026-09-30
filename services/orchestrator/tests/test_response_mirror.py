from __future__ import annotations

import asyncio
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import numpy as np
from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect

from app.main import app
from app.protocol import AudioFrameKind, pack_audio_frame, unpack_audio_frame
from app.response_mirror import ResponseMirror
from app.settings import Settings
from tools.live_mic_client import PcmConverter


class MirrorTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        settings = Settings(_env_file=None, mock_asr=True, mock_llm=True, mock_tts=True,
                            validate_externals_on_startup=False, log_dir=Path(self.tmp.name) / "logs",
                            audio_dir=Path(self.tmp.name) / "audio", tts_sample_rate_hz=24000)
        self.settings_patch = patch("app.main.Settings", return_value=settings)
        self.settings_patch.start()
        self.addCleanup(self.settings_patch.stop)
        # Synthetic PCM tests are about routing/session handling, not VAD accuracy.
        self.vad_patch = patch("app.vad.TurnDetector._is_speech_frame", return_value=True)
        self.vad_patch.start()
        self.addCleanup(self.vad_patch.stop)
        self.client = self.enterContext(TestClient(app))

    def start(self, socket):
        socket.send_json({"type": "session.start", "payload": {"locale": "en-US"}})
        self.assertEqual(socket.receive_json()["payload"]["reason"], "ready")

    def turn(self, socket):
        converter = PcmConverter(48000, 2)
        frames = converter.feed(np.ones((4800, 2), dtype=np.float32) * 0.1) + converter.finish()
        for frame in frames:
            socket.send_bytes(pack_audio_frame(kind=AudioFrameKind.MIC, sample_rate_hz=16000, channels=1, payload=frame))
        socket.send_json({"type": "mic.end", "payload": {}})
        messages = []
        while True:
            message = socket.receive()
            self.assertEqual(message["type"], "websocket.send")
            raw = message.get("text") or message["bytes"]
            messages.append(raw)
            if isinstance(raw, str):
                event = json.loads(raw)
                self.assertNotEqual(event["type"], "error")
                if event.get("payload", {}).get("reason") == "turn_complete":
                    break
        self.assertTrue(any(isinstance(item, bytes) for item in messages))
        return messages

    def test_synthetic_pcm_roundtrip_mirrors_identical_events_and_audio(self):
        with self.client.websocket_connect("/ws/session?observe=mac") as observer:
            self.start(observer)
            for _ in range(2):  # observer survives completed publisher disconnect
                with self.client.websocket_connect("/ws/session?publish=mac") as publisher:
                    self.start(publisher)
                    expected = self.turn(publisher)
                    actual = []
                    for _ in expected:
                        message = observer.receive()
                        actual.append(message.get("text") or message["bytes"])
                    self.assertEqual(actual, expected)
                    tts = unpack_audio_frame(next(item for item in actual if isinstance(item, bytes)))
                    self.assertEqual(tts.kind, AudioFrameKind.TTS)
                    self.assertEqual(tts.sample_rate_hz, 24000)
                    self.assertTrue(tts.turn_id)

    def test_unmodified_session_still_runs(self):
        with self.client.websocket_connect("/ws/session") as socket:
            self.start(socket)
            self.turn(socket)

    def test_observer_cannot_send_audio(self):
        with self.client.websocket_connect("/ws/session?observe=mac") as socket:
            self.start(socket)
            socket.send_bytes(b"not allowed")
            self.assertEqual(socket.receive()["code"], 1008)

    def test_invalid_and_conflicting_channels(self):
        for url in ("/ws/session?publish=", "/ws/session?publish=a&observe=b"):
            with self.assertRaises(WebSocketDisconnect):
                with self.client.websocket_connect(url):
                    pass
        with self.client.websocket_connect("/ws/session?publish=mac") as publisher:
            self.start(publisher)
            with self.assertRaises(WebSocketDisconnect):
                with self.client.websocket_connect("/ws/session?publish=mac"):
                    pass
            with self.client.websocket_connect("/ws/session?observe=mac") as late:
                late.send_json({"type": "session.start"})
                self.assertEqual(late.receive()["code"], 1008)

    def test_channel_isolation_and_backpressure(self):
        mirror = ResponseMirror()
        slow, other = asyncio.Queue(maxsize=1), asyncio.Queue(maxsize=1)
        mirror.listeners = {"a": {slow}, "b": {other}}
        mirror.publish("a", b"one")
        mirror.publish("a", b"two")
        self.assertIsNone(slow.get_nowait())
        self.assertTrue(other.empty())
        self.assertFalse(mirror.listeners["a"])


if __name__ == "__main__":
    unittest.main()
