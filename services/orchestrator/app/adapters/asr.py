from __future__ import annotations
import os

import asyncio
import queue
from collections.abc import AsyncIterator, Iterator
from typing import TYPE_CHECKING

import httpx

from app.adapters.base import AsrClient, AsrEvent, StreamingAsrSession
from app.service_status import ServiceStatus

if TYPE_CHECKING:
    from app.settings import Settings


_END = object()


class MockAsrStream:
    def __init__(self) -> None:
        self._queue: asyncio.Queue[AsrEvent | object] = asyncio.Queue()
        self._audio_bytes = 0
        self._sent_partial = False

    async def push_audio(self, chunk: bytes) -> None:
        self._audio_bytes += len(chunk)
        if self._audio_bytes >= 3200 and not self._sent_partial:
            self._sent_partial = True
            await self._queue.put(AsrEvent(kind="partial", text="Hello, can you"))

    async def end(self) -> None:
        await self._queue.put(AsrEvent(kind="final", text="Hello, can you introduce yourself?"))
        await self._queue.put(_END)

    async def cancel(self) -> None:
        await self._queue.put(_END)

    async def events(self) -> AsyncIterator[AsrEvent]:
        while True:
            item = await self._queue.get()
            if item is _END:
                break
            yield item


class MockAsrClient:
    def open_stream(self) -> StreamingAsrSession:
        return MockAsrStream()

    async def healthcheck(self) -> ServiceStatus:
        return ServiceStatus(name="asr", ok=True, detail="mock ASR enabled")


class RivaAsrStream:
    def __init__(self, settings: Settings) -> None:
        self._settings = settings
        self._loop = asyncio.get_running_loop()

        # Store one utterance until VAD says the user finished speaking
        self._audio_buffer = bytearray()

        self._event_queue: asyncio.Queue[
            AsrEvent | Exception | object
        ] = asyncio.Queue()

        self._ended = False

    async def push_audio(self, chunk: bytes) -> None:
        if self._ended:
            return

        self._audio_buffer.extend(chunk)

    async def end(self) -> None:
        if self._ended:
            return

        self._ended = True

        try:
            # Run blocking gRPC call outside asyncio event loop
            await asyncio.to_thread(self._run_offline)
        finally:
            await self._event_queue.put(_END)

    async def cancel(self) -> None:
        self._ended = True
        self._audio_buffer.clear()
        await self._event_queue.put(_END)

    async def events(self) -> AsyncIterator[AsrEvent]:
        while True:
            item = await self._event_queue.get()

            if item is _END:
                break

            if isinstance(item, Exception):
                raise item

            yield item

    def _run_offline(self) -> None:
        try:
            import riva.client

            metadata_args = []

            if self._settings.asr_use_hosted_api:
                metadata_args = [
                    ["function-id", self._settings.asr_function_id],
                    [
                        "authorization",
                        f"Bearer {self._settings.asr_api_key}",
                    ],
                ]

            auth = riva.client.Auth(
                uri=self._settings.asr_server,
                use_ssl=self._settings.asr_use_ssl,
                metadata_args=metadata_args,

                # Very important on AutoDL:
                # disable its HTTP proxy only for this gRPC channel
                options=[
                    ("grpc.enable_http_proxy", 0),
                ],
            )

            asr_service = riva.client.ASRService(auth)

            config = riva.client.RecognitionConfig(
                encoding=riva.client.AudioEncoding.LINEAR_PCM,
                sample_rate_hertz=self._settings.asr_sample_rate_hz,
                audio_channel_count=1,
                language_code=self._settings.asr_language_code,
                model=self._settings.asr_model,
                max_alternatives=1,
                profanity_filter=False,
                enable_automatic_punctuation=True,
                verbatim_transcripts=False,
            )

            future = asr_service.offline_recognize(
                audio_bytes=bytes(self._audio_buffer),
                config=config,
                future=True,
            )
            try:
                response = future.result(timeout=60)
            finally:
                future.cancel()

            final_parts = []

            for result in getattr(response, "results", []):
                alternatives = getattr(result, "alternatives", [])

                if not alternatives:
                    continue

                text = alternatives[0].transcript.strip()

                if text:
                    final_parts.append(text)

            transcript = " ".join(final_parts).strip()

            if transcript:
                event = AsrEvent(
                    kind="final",
                    text=transcript,
                )

                self._loop.call_soon_threadsafe(
                    self._event_queue.put_nowait,
                    event,
                )

        except Exception as exc:
            self._loop.call_soon_threadsafe(
                self._event_queue.put_nowait,
                exc,
            )

class RivaAsrClient:
    def __init__(self, settings: "Settings") -> None:
        self._settings = settings

    def open_stream(self) -> StreamingAsrSession:
        return RivaAsrStream(self._settings)

    async def healthcheck(self) -> ServiceStatus:
        if self._settings.asr_use_hosted_api:
            if not self._settings.asr_api_key:
                return ServiceStatus(
                    name="asr",
                    ok=False,
                    detail="ACE_ASR_API_KEY is not configured",
                    meta={"grpc": self._settings.asr_server},
                )

            if not self._settings.asr_function_id:
                return ServiceStatus(
                    name="asr",
                    ok=False,
                    detail="ACE_ASR_FUNCTION_ID is not configured",
                    meta={"grpc": self._settings.asr_server},
                )

            return ServiceStatus(
                name="asr",
                ok=True,
                detail="hosted NVIDIA ASR configured; connectivity is verified by an audio turn",
                meta={
                    "grpc": self._settings.asr_server,
                    "language": self._settings.asr_language_code,
                },
            )

        try:
            async with httpx.AsyncClient(
                timeout=httpx.Timeout(5.0, connect=2.0)
            ) as client:
                response = await client.get(
                    f"{self._settings.asr_http_url.rstrip('/')}"
                    "/v1/health/ready"
                )

                response.raise_for_status()

            return ServiceStatus(
                name="asr",
                ok=True,
                detail="ASR NIM is ready",
                meta={
                    "grpc": self._settings.asr_server,
                    "http": self._settings.asr_http_url,
                },
            )

        except Exception as exc:
            return ServiceStatus(
                name="asr",
                ok=False,
                detail=str(exc),
                meta={
                    "grpc": self._settings.asr_server,
                    "http": self._settings.asr_http_url,
                },
            )

def build_asr_client(settings: "Settings") -> AsrClient:
    if settings.mock_asr:
        return MockAsrClient()
    return RivaAsrClient(settings)
