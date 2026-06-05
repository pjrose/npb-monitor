from __future__ import annotations

import asyncio
import threading
from typing import Any

from .config import AppConfig
from .state import MonitorState

try:
    from amqtt.broker import Broker
except ImportError:  # pragma: no cover - dependency is installed in the project venv
    Broker = None  # type: ignore[assignment]


class EmbeddedMqttBroker:
    def __init__(self, config: AppConfig, state: MonitorState) -> None:
        self.config = config
        self.state = state
        self._thread: threading.Thread | None = None
        self._started = threading.Event()
        self._stopped = threading.Event()
        self._loop: asyncio.AbstractEventLoop | None = None
        self._stop_event: asyncio.Event | None = None
        self._error: BaseException | None = None

    def start(self) -> None:
        if not self.config.embedded_mqtt:
            self.state.add_event("broker", "embedded broker disabled")
            return
        if Broker is None:
            raise RuntimeError("amqtt is not installed")
        self._thread = threading.Thread(target=self._run_thread, name="npb-mqtt-broker", daemon=True)
        self._thread.start()
        self._started.wait(timeout=8)
        if self._error is not None:
            raise RuntimeError(f"Embedded MQTT broker failed to start: {self._error}") from self._error
        if not self._started.is_set():
            raise RuntimeError("Embedded MQTT broker did not finish startup in time")

    def stop(self) -> None:
        if self._loop is not None and self._stop_event is not None:
            self._loop.call_soon_threadsafe(self._stop_event.set)
        if self._thread is not None:
            self._thread.join(timeout=5)

    def _run_thread(self) -> None:
        try:
            asyncio.run(self._run_broker())
        except BaseException as exc:  # pragma: no cover - startup failures are environment-specific
            self._error = exc
            self.state.add_event("broker", f"startup failed: {exc}")
            self._started.set()
        finally:
            self._stopped.set()

    async def _run_broker(self) -> None:
        self._loop = asyncio.get_running_loop()
        self._stop_event = asyncio.Event()
        broker = Broker(_broker_config(self.config))
        await broker.start()
        self.state.add_event(
            "broker",
            f"embedded MQTT broker listening on {self.config.broker_bind_host}:{self.config.broker_port}",
        )
        self._started.set()
        try:
            await self._stop_event.wait()
        finally:
            await broker.shutdown()
            self.state.add_event("broker", "embedded MQTT broker stopped")


def _broker_config(config: AppConfig) -> dict[str, Any]:
    return {
        "listeners": {
            "default": {
                "type": "tcp",
                "bind": f"{config.broker_bind_host}:{config.broker_port}",
            }
        },
        "sys_interval": 0,
        "auth": {
            "allow-anonymous": True,
        },
    }

