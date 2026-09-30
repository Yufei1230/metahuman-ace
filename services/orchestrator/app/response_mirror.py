"""Opt-in response fan-out. ACE1/control messages pass through unchanged."""
from __future__ import annotations

import asyncio
import re
from contextlib import suppress
from uuid import uuid4

from fastapi import WebSocket

from app.protocol import parse_control_message, serialize_control_message


class ResponseMirror:
    def __init__(self) -> None:
        self.publishers: set[str] = set()
        self.listeners: dict[str, set[asyncio.Queue]] = {}

    @staticmethod
    def valid_channel(channel: str) -> bool:
        return bool(re.fullmatch(r"[A-Za-z0-9_-]{1,64}", channel))

    def publish(self, channel: str, message: str | bytes | None) -> None:
        for queue in tuple(self.listeners.get(channel, ())):
            if queue.full():
                # Never stall ASR/TTS behind a slow renderer or silently lose audio.
                self.listeners[channel].discard(queue)
                while not queue.empty():
                    queue.get_nowait()
                queue.put_nowait(None)
            else:
                queue.put_nowait(message)

    async def observe(self, websocket: WebSocket, channel: str) -> None:
        await websocket.accept()
        queue: asyncio.Queue = asyncio.Queue(maxsize=256)
        registered = False

        async def receive() -> None:
            nonlocal registered
            while True:
                message = await websocket.receive()
                if message["type"] == "websocket.disconnect":
                    return
                try:
                    control = parse_control_message(message.get("text") or "")
                    if control.type != "session.start" or registered:
                        raise ValueError("Observer accepts session.start only; audio input is disabled")
                    if channel in self.publishers:
                        raise ValueError("Connect Unreal observer before starting the Mac publisher")
                    self.listeners.setdefault(channel, set()).add(queue)
                    registered = True
                    queue.put_nowait(serialize_control_message(
                        event_type="state", session_id=uuid4(), turn_id=None,
                        payload={"state": "LISTENING", "reason": "ready"}))
                except (ValueError, TypeError):
                    await websocket.close(code=1008, reason="Observer requires session.start before publisher; no input allowed")
                    return

        async def send() -> None:
            while True:
                message = await queue.get()
                if message is None:
                    await websocket.close(code=1000, reason="Publisher disconnected or observer fell behind; reconnect")
                    return
                if isinstance(message, bytes):
                    await websocket.send_bytes(message)
                else:
                    await websocket.send_text(message)

        tasks = [asyncio.create_task(receive()), asyncio.create_task(send())]
        try:
            done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            for task in done:
                task.result()
        finally:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            listeners = self.listeners.get(channel)
            if listeners is not None:
                listeners.discard(queue)
                if not listeners:
                    self.listeners.pop(channel, None)
            with suppress(RuntimeError):
                await websocket.close()


class MirroredWebSocket:
    def __init__(self, websocket: WebSocket, mirror: ResponseMirror, channel: str) -> None:
        self.websocket, self.mirror, self.channel = websocket, mirror, channel
        self.completed = False

    async def accept(self) -> None:
        await self.websocket.accept()

    async def receive(self):
        return await self.websocket.receive()

    async def send_text(self, data: str) -> None:
        await self.websocket.send_text(data)
        control = parse_control_message(data)
        self.completed = control.type == "state" and control.payload.get("reason") == "turn_complete"
        # Observer already received its ready handshake; do not trigger it twice.
        if control.type != "state" or control.payload.get("reason") != "ready":
            self.mirror.publish(self.channel, data)

    async def send_bytes(self, data: bytes) -> None:
        await self.websocket.send_bytes(data)
        self.mirror.publish(self.channel, data)
