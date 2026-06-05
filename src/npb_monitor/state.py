from __future__ import annotations

from collections import deque
from datetime import datetime, timedelta, timezone
from threading import RLock
from typing import Any


HISTORY_HOURS = 24
HISTORY_MAX_SAMPLES = 120_000

FLAG_LABELS = {
    "operation_on": "Operation",
    "over_temperature": "Over temperature",
    "output_over_voltage": "Output over-voltage",
    "output_over_current": "Output over-current",
    "output_short_circuit": "Output short circuit",
    "ac_abnormal": "AC abnormal",
    "output_off": "Output off",
    "internal_temperature_abnormal": "Internal temperature",
    "fully_charged": "Charging finished",
    "constant_current_mode": "Constant current",
    "constant_voltage_mode": "Constant voltage",
    "float_mode": "Float mode",
    "wake_up_unfinished": "Wake-up unfinished",
    "temperature_compensation_short": "Temperature compensation short",
    "no_battery_detected": "No battery detected",
    "cc_timeout": "CC timeout",
    "cv_timeout": "CV timeout",
    "float_timeout": "Float timeout",
    "dc_output_normal_range": "DC output normal",
    "initial_state": "Initial state",
    "eeprom_access_error": "EEPROM access error",
    "charger_mode": "Charger mode",
    "restart_after_full_enabled": "Restart after full",
    "disable_eeprom_writes": "EEPROM writes disabled",
}

ANNOTATION_FLAGS = {
    "operation_on",
    "output_off",
    "fully_charged",
    "constant_current_mode",
    "constant_voltage_mode",
    "float_mode",
    "no_battery_detected",
    "cc_timeout",
    "cv_timeout",
    "float_timeout",
    "charger_mode",
}


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def utc_now_dt() -> datetime:
    return datetime.now(timezone.utc)


class MonitorState:
    def __init__(self) -> None:
        self._lock = RLock()
        self.mqtt_connected = False
        self.wican_status = "unknown"
        self.last_rx_at: str | None = None
        self.last_tx_at: str | None = None
        self.commands: dict[str, dict[str, Any]] = {}
        self.flags: dict[str, bool] = {}
        self.frames = deque(maxlen=80)
        self.events = deque(maxlen=80)
        self.status_events = deque(maxlen=5000)
        self.history = deque(maxlen=HISTORY_MAX_SAMPLES)
        self._latest_measurements: dict[str, float | None] = {
            "voltage": None,
            "current": None,
            "temperature": None,
        }

    def set_mqtt_connected(self, connected: bool) -> None:
        with self._lock:
            self.mqtt_connected = connected
            self.add_event_locked("mqtt", "connected" if connected else "disconnected")

    def set_wican_status(self, status: str) -> None:
        with self._lock:
            self.wican_status = status
            self.add_event_locked("wican", status)

    def record_tx(self, topic: str, payload: dict[str, Any]) -> None:
        with self._lock:
            self.last_tx_at = utc_now()
            self.frames.appendleft({"direction": "tx", "topic": topic, "payload": payload, "at": self.last_tx_at})

    def record_rx(self, topic: str, payload: dict[str, Any]) -> None:
        with self._lock:
            self.last_rx_at = utc_now()
            self.frames.appendleft({"direction": "rx", "topic": topic, "payload": payload, "at": self.last_rx_at})

    def record_decoded(self, decoded: dict[str, Any]) -> None:
        with self._lock:
            now_dt = utc_now_dt()
            now = now_dt.isoformat(timespec="milliseconds")
            name = decoded["name"]
            self.commands[name] = {**decoded, "at": now}
            for key, value in decoded.get("flags", {}).items():
                if isinstance(value, bool):
                    self._record_flag_change_locked(key, value, now_dt)
                    self.flags[key] = value
            self._record_measurement_locked(decoded, now_dt)
            self._prune_history_locked(now_dt)
            self.last_rx_at = now

    def add_event(self, kind: str, message: str) -> None:
        with self._lock:
            self.add_event_locked(kind, message)

    def add_event_locked(self, kind: str, message: str) -> None:
        self.events.appendleft({"kind": kind, "message": message, "at": utc_now()})

    def history_snapshot(self) -> dict[str, Any]:
        with self._lock:
            self._prune_history_locked(utc_now_dt())
            return {
                "hours": HISTORY_HOURS,
                "samples": list(self.history),
                "events": list(self.status_events),
            }

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            readings = {
                "operation_on": self._command_value("OPERATION"),
                "target_voltage_v": self._command_value("VOUT_SET"),
                "target_current_a": self._command_value("IOUT_SET"),
                "output_voltage_v": self._command_value("READ_VOUT"),
                "output_current_a": self._command_value("READ_IOUT"),
                "temperature_c": self._command_value("READ_TEMPERATURE_1"),
                "curve_current_a": self._command_value("CURVE_CC"),
                "curve_cv_v": self._command_value("CURVE_CV"),
                "curve_float_v": self._command_value("CURVE_FV"),
                "curve_taper_a": self._command_value("CURVE_TC"),
                "restart_voltage_v": self._command_value("CHG_RST_VBAT"),
            }
            power = None
            if readings["output_voltage_v"] is not None and readings["output_current_a"] is not None:
                power = readings["output_voltage_v"] * readings["output_current_a"]
            return {
                "mqtt_connected": self.mqtt_connected,
                "wican_status": self.wican_status,
                "last_rx_at": self.last_rx_at,
                "last_tx_at": self.last_tx_at,
                "readings": {**readings, "output_power_w": power},
                "commands": self.commands,
                "flags": self.flags,
                "frames": list(self.frames),
                "events": list(self.events),
            }

    def _command_value(self, name: str) -> Any:
        command = self.commands.get(name)
        if command is None:
            return None
        return command.get("value")

    def _record_measurement_locked(self, decoded: dict[str, Any], now_dt: datetime) -> None:
        name = decoded["name"]
        value = decoded.get("value")
        if not isinstance(value, (int, float)):
            return
        if name == "READ_VOUT":
            self._latest_measurements["voltage"] = float(value)
        elif name == "READ_IOUT":
            self._latest_measurements["current"] = float(value)
        elif name == "READ_TEMPERATURE_1":
            self._latest_measurements["temperature"] = float(value)
        else:
            return

        if name != "READ_IOUT":
            return
        voltage = self._latest_measurements["voltage"]
        current = self._latest_measurements["current"]
        if voltage is None or current is None:
            return
        self.history.append(
            {
                "at": now_dt.isoformat(timespec="milliseconds"),
                "ts": int(now_dt.timestamp() * 1000),
                "voltage": voltage,
                "current": current,
                "power": voltage * current,
                "temperature": self._latest_measurements["temperature"],
            }
        )

    def _record_flag_change_locked(self, key: str, value: bool, now_dt: datetime) -> None:
        previous = self.flags.get(key)
        if previous == value:
            return
        label = FLAG_LABELS.get(key, key.replace("_", " "))
        if previous is None and not value:
            return
        if previous is None:
            message = f"{label} observed active"
        else:
            message = f"{label} {'active' if value else 'cleared'}"
        event = {
            "kind": "status",
            "flag": key,
            "value": value,
            "message": message,
            "at": now_dt.isoformat(timespec="milliseconds"),
            "ts": int(now_dt.timestamp() * 1000),
            "annotate": key in ANNOTATION_FLAGS,
        }
        self.status_events.appendleft(event)
        self.events.appendleft({"kind": "status", "message": message, "at": event["at"]})

    def _prune_history_locked(self, now_dt: datetime) -> None:
        cutoff_ms = int((now_dt - timedelta(hours=HISTORY_HOURS)).timestamp() * 1000)
        while self.history and self.history[0]["ts"] < cutoff_ms:
            self.history.popleft()
        while self.status_events and self.status_events[-1]["ts"] < cutoff_ms:
            self.status_events.pop()
