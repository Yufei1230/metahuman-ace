from __future__ import annotations

import asyncio
from collections.abc import AsyncIterator
from typing import TYPE_CHECKING

import httpx

from app.service_status import ServiceStatus

if TYPE_CHECKING:
    from app.settings import Settings

import io
import wave


class MockTtsClient:
    async def stream_text(self, text: str) -> AsyncIterator[bytes]:
        silence = b"\x00\x00" * 960
        for _ in range(3):
            yield silence
            await asyncio.sleep(0)

    async def healthcheck(self) -> ServiceStatus:
        return ServiceStatus(name="tts", ok=True, detail="mock TTS enabled")


class NvidiaHostedTtsClient:
    def __init__(self, settings: "Settings") -> None:
        self._settings = settings

    async def list_voices(self) -> list[str]:
        url = self._settings.tts_api_url.rsplit("/", 1)[0] + "/list_voices"
        async with httpx.AsyncClient(timeout=30) as client:
            response = await client.get(url, headers={"Authorization": f"Bearer {self._settings.nim_api_key}"})
            response.raise_for_status()
            data = response.json()
        return sorted({voice for group in data.values() for voice in group.get("voices", [])})

    async def stream_text(self, text: str) -> AsyncIterator[bytes]:
        if not self._settings.nim_api_key:
            raise RuntimeError("ACE_NIM_API_KEY is not set")

        async with httpx.AsyncClient(timeout=httpx.Timeout(60.0, connect=10.0)) as client:
            response = await client.post(
                self._settings.tts_api_url,
                headers={
                    "Authorization": f"Bearer {self._settings.nim_api_key}",
                },
                data={
                    "text": text,
                    "language": self._settings.tts_language_code,
                    "voice": self._settings.tts_voice,
                    "encoding": self._settings.tts_encoding,
                    "sample_rate_hz": str(self._settings.tts_sample_rate_hz),
                },
            )
            response.raise_for_status()

        wav_bytes = response.content

        with wave.open(io.BytesIO(wav_bytes), "rb") as wav_file:
            channels = wav_file.getnchannels()
            sample_width = wav_file.getsampwidth()
            sample_rate = wav_file.getframerate()

            if channels != 1:
                raise RuntimeError(f"unexpected TTS channels: {channels}")

            if sample_width != 2:
                raise RuntimeError(f"unexpected TTS sample width: {sample_width}")

            if sample_rate != self._settings.tts_sample_rate_hz:
                raise RuntimeError(
                    f"unexpected TTS sample rate: {sample_rate}, "
                    f"expected {self._settings.tts_sample_rate_hz}"
                )

            pcm = wav_file.readframes(wav_file.getnframes())

        # 100 ms per chunk at 24 kHz, 16-bit mono:
        # 24000 samples/s * 0.1 s * 2 bytes = 4800 bytes
        chunk_size = self._settings.tts_sample_rate_hz // 10 * 2

        for offset in range(0, len(pcm), chunk_size):
            yield pcm[offset : offset + chunk_size]
            await asyncio.sleep(0)

    async def healthcheck(self) -> ServiceStatus:
        if not self._settings.nim_api_key:
            return ServiceStatus(
                name="tts",
                ok=False,
                detail="ACE_NIM_API_KEY is not set",
            )

        try:
            voices = await self.list_voices()
        except Exception:
            return ServiceStatus(name="tts", ok=False, detail="Hosted TTS voice discovery failed")
        return ServiceStatus(
            name="tts",
            ok=self._settings.tts_voice in voices,
            detail="Hosted TTS voice verified" if self._settings.tts_voice in voices else "Configured TTS voice is unavailable",
            meta={
                "api_url": self._settings.tts_api_url,
                "voice": self._settings.tts_voice,
                "language": self._settings.tts_language_code,
            },
        )

class RivaTtsClient:
    def __init__(self, settings: "Settings") -> None:
        self._settings = settings

    async def stream_text(self, text: str) -> AsyncIterator[bytes]:
        loop = asyncio.get_running_loop()
        queue: asyncio.Queue[bytes | Exception | None] = asyncio.Queue()

        def run_blocking() -> None:
            try:
                import riva.client
                from riva.client.proto.riva_audio_pb2 import AudioEncoding

                auth = riva.client.Auth(uri=self._settings.tts_server, use_ssl=False)
                service = riva.client.SpeechSynthesisService(auth)
                responses = service.synthesize_online(
                    [text],
                    self._settings.tts_voice,
                    self._settings.tts_language_code,
                    sample_rate_hz=self._settings.tts_sample_rate_hz,
                    encoding=(
                        AudioEncoding.OGGOPUS
                        if self._settings.tts_encoding == "OGGOPUS"
                        else AudioEncoding.LINEAR_PCM
                    ),
                )
                for response in responses:
                    loop.call_soon_threadsafe(queue.put_nowait, response.audio)
            except Exception as exc:
                loop.call_soon_threadsafe(queue.put_nowait, exc)
            finally:
                loop.call_soon_threadsafe(queue.put_nowait, None)

        worker = asyncio.create_task(asyncio.to_thread(run_blocking))
        try:
            while True:
                item = await queue.get()
                if item is None:
                    break
                if isinstance(item, Exception):
                    raise item
                yield item
        finally:
            await worker

    async def healthcheck(self) -> ServiceStatus:
        try:
            async with httpx.AsyncClient(timeout=httpx.Timeout(5.0, connect=2.0)) as client:
                response = await client.get(f"{self._settings.tts_http_url.rstrip('/')}/v1/health/ready")
                response.raise_for_status()
            return ServiceStatus(
                name="tts",
                ok=True,
                detail="TTS NIM is ready",
                meta={"grpc": self._settings.tts_server, "http": self._settings.tts_http_url},
            )
        except Exception as exc:
            return ServiceStatus(
                name="tts",
                ok=False,
                detail=str(exc),
                meta={"grpc": self._settings.tts_server, "http": self._settings.tts_http_url},
            )


def build_tts_client(
    settings: "Settings",
) -> MockTtsClient | NvidiaHostedTtsClient | RivaTtsClient:
    if settings.mock_tts:
        return MockTtsClient()

    if settings.tts_use_hosted_api:
        return NvidiaHostedTtsClient(settings)

    return RivaTtsClient(settings)
