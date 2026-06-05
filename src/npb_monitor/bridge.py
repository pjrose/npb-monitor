from __future__ import annotations

import json
import threading
import time
from typing import Any

from .config import AppConfig
from . import protocol
from .state import MonitorState

try:
    import paho.mqtt.client as mqtt
except ImportError:  # pragma: no cover - dependency is installed in the project venv
    mqtt = None  # type: ignore[assignment]


class WicanBridge:
    def __init__(self, config: AppConfig, state: MonitorState) -> None:
        self.config = config
        self.state = state
        self._stop = threading.Event()
        self._client: Any = None
        self._poll_thread: threading.Thread | None = None

    def start(self) -> None:
        if not self.config.mqtt_enabled:
            self.state.add_event("mqtt", "disabled")
            return
        if mqtt is None:
            raise RuntimeError("paho-mqtt is not installed")

        self._client = mqtt.Client()
        if self.config.mqtt_username:
            self._client.username_pw_set(self.config.mqtt_username, self.config.mqtt_password)
        self._client.on_connect = self._on_connect
        self._client.on_disconnect = self._on_disconnect
        self._client.on_message = self._on_message
        self.state.add_event("mqtt", f"connecting to {self.config.mqtt_host}:{self.config.mqtt_port}")
        self._client.connect_async(self.config.mqtt_host, self.config.mqtt_port, keepalive=30)
        self._client.loop_start()

        self._poll_thread = threading.Thread(target=self._poll_loop, name="npb-poll", daemon=True)
        self._poll_thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._poll_thread is not None:
            self._poll_thread.join(timeout=2)
        if self._client is not None:
            self._client.loop_stop()
            self._client.disconnect()

    def publish_read(self, command: int) -> None:
        self.publish_data(protocol.command_bytes(command))

    def publish_scaled_write(self, command: int, value: float | int) -> None:
        self.publish_data(protocol.scaled_command_bytes(command, value))

    def publish_raw_write(self, command: int, raw_value: int) -> None:
        self.publish_data(protocol.command_bytes(command, raw_value))

    def set_operation(self, enabled: bool) -> None:
        self.publish_data(protocol.operation_bytes(enabled))

    def publish_data(self, data: list[int], can_id: int | None = None) -> None:
        if self._client is None:
            self.state.add_event("tx", "MQTT client is not running")
            return
        target_id = self.config.controller_can_id if can_id is None else can_id
        payload = protocol.wican_tx_payload(target_id, data)
        text = json.dumps(payload, separators=(",", ":"))
        self._client.publish(self.config.tx_topic, text)
        self.state.record_tx(self.config.tx_topic, payload)

    def scan_addresses(self) -> None:
        for address in range(4):
            can_id = protocol.controller_can_id(address)
            for command in (0x0000, 0x0060, 0x0061, 0x00B8, 0x00C1):
                self.publish_data(protocol.command_bytes(command), can_id=can_id)
                time.sleep(max(self.config.packet_gap_s, 0.05))
        self.state.add_event("diagnostic", "scanned NPB addresses 0x00..0x03")

    def poll_once(self) -> None:
        for command in protocol.POLL_COMMANDS:
            self.publish_read(command)
            time.sleep(self.config.packet_gap_s)

    def initialize_power_supply_mode(self) -> None:
        self.publish_raw_write(0x00B4, 0x0004)
        time.sleep(max(self.config.packet_gap_s, 0.1))
        self.publish_raw_write(0x00C2, 0x0403)
        self.state.add_event("control", "requested PSU mode, CAN control, and EEPROM write disable")
        self.state.add_notice(
            "curve_config_ac_cycle",
            "critical",
            "AC power cycle required",
            "PSU mode was written through CURVE_CONFIG 0x00B4. The manual says 0x00B4 changes take effect only after AC input is power-cycled.",
        )

    def set_charger_mode_default(self) -> None:
        self.publish_raw_write(0x00B4, 0x0084)
        self.state.add_event("control", "requested charger mode default curve config")
        self.state.add_notice(
            "curve_config_ac_cycle",
            "critical",
            "AC power cycle required",
            "Charger mode was written through CURVE_CONFIG 0x00B4. The manual says 0x00B4 changes take effect only after AC input is power-cycled.",
        )

    def disable_eeprom_writes(self) -> None:
        self.publish_raw_write(0x00C2, 0x0403)
        self.state.add_event("control", "requested CAN control and EEPROM write disable")

    def enable_eeprom_writes(self) -> None:
        self.publish_raw_write(0x00C2, 0x0003)
        self.state.add_event("control", "requested CAN control and EEPROM write enable")

    def _poll_loop(self) -> None:
        while not self._stop.is_set():
            if self._client is not None and self.state.mqtt_connected and not self.state.data_exchange_paused:
                try:
                    self.poll_once()
                except Exception as exc:  # pragma: no cover - defensive runtime log
                    self.state.add_event("poll", str(exc))
            self._stop.wait(self.config.poll_interval_s)

    def _on_connect(self, client: Any, userdata: Any, flags: Any, reason_code: Any, properties: Any = None) -> None:
        ok = int(reason_code) == 0 if not isinstance(reason_code, str) else reason_code == "Success"
        self.state.set_mqtt_connected(ok)
        if ok:
            topics = self._subscription_topics()
            for topic in topics:
                client.subscribe(topic)
            self.state.add_event("mqtt", f"subscribed to {', '.join(topics)}")
        else:
            self.state.add_event("mqtt", f"connect failed: {reason_code}")

    def _on_disconnect(self, client: Any, userdata: Any, *args: Any) -> None:
        self.state.set_mqtt_connected(False)

    def _on_message(self, client: Any, userdata: Any, msg: Any) -> None:
        try:
            payload = json.loads(msg.payload.decode("utf-8"))
        except Exception as exc:
            self.state.add_event("rx", f"invalid JSON on {msg.topic}: {exc}")
            return

        self.state.record_rx(msg.topic, payload)
        if msg.topic in self._status_topics():
            status = payload.get("status", str(payload)) if isinstance(payload, dict) else str(payload)
            self.state.set_wican_status(status)
            return

        frames = payload.get("frame", []) if isinstance(payload, dict) else []
        for frame in frames:
            try:
                can_id = int(frame["id"])
                expected = self.config.device_address if can_id == self.config.charger_can_id else None
                decoded = protocol.decode_frame(
                    can_id,
                    [int(byte) for byte in frame.get("data", [])],
                    expected_address=expected,
                )
            except Exception as exc:
                self.state.add_event("decode", str(exc))
                continue
            self.state.record_decoded(decoded)

    def _subscription_topics(self) -> list[str]:
        topics = [self.config.rx_topic, *self._status_topics()]
        out: list[str] = []
        for topic in topics:
            if topic not in out:
                out.append(topic)
        return out

    def _status_topics(self) -> list[str]:
        base = self.config.rx_topic.removesuffix("/rx")
        return [
            self.config.status_topic,
            f"{base}/status",
            f"{base}/stat",
            self.config.rx_topic.replace("/can/rx", "/status"),
        ]
