from __future__ import annotations

from collections import deque
from datetime import datetime, timedelta, timezone
from threading import RLock
from typing import Any


HISTORY_HOURS = 24
HISTORY_MAX_SAMPLES = 120_000
STALE_SECONDS = 15

READING_SOURCES = {
    "operation_on": "OPERATION",
    "target_voltage_v": "VOUT_SET",
    "target_current_a": "IOUT_SET",
    "output_voltage_v": "READ_VOUT",
    "output_current_a": "READ_IOUT",
    "temperature_c": "READ_TEMPERATURE_1",
    "curve_current_a": "CURVE_CC",
    "curve_cv_v": "CURVE_CV",
    "curve_float_v": "CURVE_FV",
    "curve_taper_a": "CURVE_TC",
    "restart_voltage_v": "CHG_RST_VBAT",
}

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
        self.notices: dict[str, dict[str, Any]] = {}
        self.status_events = deque(maxlen=5000)
        self.history = deque(maxlen=HISTORY_MAX_SAMPLES)
        self.data_exchange_paused = False
        self.session_started_at_dt = utc_now_dt()
        self.session_started_at = self.session_started_at_dt.isoformat(timespec="seconds")
        self._session_sample_count = 0
        self._session_event_count = 0
        self._session_watt_hours = 0.0
        self._session_last_sample: dict[str, Any] | None = None
        self._session_max: dict[str, float | None] = {
            "voltage": None,
            "current": None,
            "power": None,
            "temperature": None,
        }
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

    def add_notice(self, notice_id: str, severity: str, title: str, message: str) -> None:
        with self._lock:
            self.notices[notice_id] = {
                "id": notice_id,
                "severity": severity,
                "title": title,
                "message": message,
                "at": utc_now(),
            }
            self.add_event_locked("notice", title)

    def clear_notice(self, notice_id: str) -> None:
        with self._lock:
            self.notices.pop(notice_id, None)

    def set_data_exchange_paused(self, paused: bool) -> None:
        with self._lock:
            if self.data_exchange_paused == paused:
                return
            self.data_exchange_paused = paused
            self.add_event_locked("poll", "data exchange paused" if paused else "data exchange resumed")

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
            now_dt = utc_now_dt()
            self._prune_history_locked(now_dt)
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
            readings_meta = self._readings_meta_locked(now_dt)
            if power is not None:
                readings_meta["output_power_w"] = {
                    "source": "READ_VOUT + READ_IOUT",
                    "stale": readings_meta["output_voltage_v"]["stale"] or readings_meta["output_current_a"]["stale"],
                    "age_s": max(
                        readings_meta["output_voltage_v"]["age_s"] or 0,
                        readings_meta["output_current_a"]["age_s"] or 0,
                    ),
                    "at": max(
                        readings_meta["output_voltage_v"]["at"] or "",
                        readings_meta["output_current_a"]["at"] or "",
                    )
                    or None,
                }
            else:
                readings_meta["output_power_w"] = {"source": "READ_VOUT + READ_IOUT", "stale": True, "age_s": None, "at": None}
            alerts = self._build_alerts_locked(readings_meta, now_dt)
            return {
                "mqtt_connected": self.mqtt_connected,
                "wican_status": self.wican_status,
                "last_rx_at": self.last_rx_at,
                "last_tx_at": self.last_tx_at,
                "data_exchange_paused": self.data_exchange_paused,
                "stale_seconds": STALE_SECONDS,
                "readings": {**readings, "output_power_w": power},
                "readings_meta": readings_meta,
                "commands": self.commands,
                "flags": self.flags,
                "alerts": alerts,
                "notices": list(self.notices.values()),
                "frames": list(self.frames),
                "events": list(self.events),
                "session": self.session_summary_locked(now_dt),
            }

    def session_summary(self) -> dict[str, Any]:
        with self._lock:
            return self.session_summary_locked(utc_now_dt())

    def session_summary_locked(self, now_dt: datetime | None = None) -> dict[str, Any]:
        now_dt = now_dt or utc_now_dt()
        elapsed_s = max(0.0, (now_dt - self.session_started_at_dt).total_seconds())
        return {
            "started_at": self.session_started_at,
            "updated_at": now_dt.isoformat(timespec="seconds"),
            "elapsed_seconds": elapsed_s,
            "sample_count": self._session_sample_count,
            "event_count": self._session_event_count,
            "watt_hours": self._session_watt_hours,
            "max_voltage_v": self._session_max["voltage"],
            "max_current_a": self._session_max["current"],
            "max_power_w": self._session_max["power"],
            "max_temperature_c": self._session_max["temperature"],
            "last_sample": self._session_last_sample,
        }

    def _command_value(self, name: str) -> Any:
        command = self.commands.get(name)
        if command is None:
            return None
        return command.get("value")

    def _readings_meta_locked(self, now_dt: datetime) -> dict[str, dict[str, Any]]:
        return {
            reading: self._command_meta_locked(command, now_dt)
            for reading, command in READING_SOURCES.items()
        }

    def _command_meta_locked(self, command_name: str, now_dt: datetime) -> dict[str, Any]:
        command = self.commands.get(command_name)
        at = command.get("at") if command else None
        age_s = self._age_seconds(at, now_dt)
        return {
            "source": command_name,
            "at": at,
            "age_s": age_s,
            "stale": age_s is None or age_s > STALE_SECONDS,
        }

    def _age_seconds(self, at: str | None, now_dt: datetime) -> float | None:
        if at is None:
            return None
        try:
            return max(0.0, (now_dt - datetime.fromisoformat(at)).total_seconds())
        except ValueError:
            return None

    def _build_alerts_locked(self, readings_meta: dict[str, dict[str, Any]], now_dt: datetime) -> list[dict[str, Any]]:
        alerts: list[dict[str, Any]] = []
        rx_age = self._age_seconds(self.last_rx_at, now_dt)
        if rx_age is None:
            alerts.append(
                {
                    "id": "no_rx",
                    "severity": "warning",
                    "title": "No incoming charger data",
                    "message": "No RX frames have been decoded in this session.",
                }
            )
        elif rx_age > STALE_SECONDS:
            alerts.append(
                {
                    "id": "stale_rx",
                    "severity": "warning",
                    "title": "Stale charger data",
                    "message": f"Last incoming decoded frame is {rx_age:.0f} seconds old.",
                }
            )
        if self.data_exchange_paused:
            alerts.append(
                {
                    "id": "paused",
                    "severity": "info",
                    "title": "Data exchange paused",
                    "message": "Automatic polling is stopped so the frame log can be inspected.",
                }
            )

        alert_flags = {
            "over_temperature": ("critical", "Over temperature", "Charger reports over-temperature."),
            "output_over_voltage": ("critical", "Output over-voltage", "Charger reports output over-voltage."),
            "output_over_current": ("critical", "Output over-current", "Charger reports output over-current."),
            "output_short_circuit": ("critical", "Output short circuit", "Charger reports output short circuit."),
            "ac_abnormal": ("critical", "AC abnormal", "Charger reports abnormal AC input."),
            "internal_temperature_abnormal": ("critical", "Internal temperature abnormal", "Charger reports internal temperature abnormal."),
            "temperature_compensation_short": ("warning", "Temperature sensor short", "Temperature compensation input appears shorted."),
            "no_battery_detected": ("warning", "No battery detected", "Charger does not detect a battery at the output."),
            "cc_timeout": ("warning", "CC timeout", "Constant-current stage timed out."),
            "cv_timeout": ("warning", "CV timeout", "Constant-voltage stage timed out."),
            "float_timeout": ("warning", "Float timeout", "Float stage timed out."),
            "eeprom_access_error": ("critical", "EEPROM access error", "Charger reports an EEPROM access error."),
        }
        for key, (severity, title, message) in alert_flags.items():
            if self.flags.get(key):
                alerts.append({"id": key, "severity": severity, "title": title, "message": message})
        if self.flags.get("operation_on") and self.flags.get("output_off"):
            alerts.append(
                {
                    "id": "operation_on_output_off",
                    "severity": "warning",
                    "title": "Output off while operation is on",
                    "message": "Operation is enabled, but the charger fault status still reports output off.",
                }
            )
        if any(meta["stale"] for meta in readings_meta.values()):
            alerts.append(
                {
                    "id": "stale_readbacks",
                    "severity": "info",
                    "title": "Some readbacks are stale",
                    "message": "Gray values have not been refreshed by recent charger replies.",
                }
            )
        return alerts

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
        sample = {
            "at": now_dt.isoformat(timespec="milliseconds"),
            "ts": int(now_dt.timestamp() * 1000),
            "voltage": voltage,
            "current": current,
            "power": voltage * current,
            "temperature": self._latest_measurements["temperature"],
        }
        self.history.append(sample)
        self._record_session_sample_locked(sample)

    def _record_session_sample_locked(self, sample: dict[str, Any]) -> None:
        previous = self._session_last_sample
        if previous is not None:
            dt_hours = max(0.0, (sample["ts"] - previous["ts"]) / 3_600_000)
            self._session_watt_hours += ((sample["power"] + previous["power"]) / 2) * dt_hours
        self._session_sample_count += 1
        self._session_last_sample = sample
        for key in ("voltage", "current", "power", "temperature"):
            value = sample.get(key)
            if value is None:
                continue
            current_max = self._session_max[key]
            if current_max is None or value > current_max:
                self._session_max[key] = float(value)

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
        self._session_event_count += 1
        self.status_events.appendleft(event)
        self.events.appendleft({"kind": "status", "message": message, "at": event["at"]})

    def _prune_history_locked(self, now_dt: datetime) -> None:
        cutoff_ms = int((now_dt - timedelta(hours=HISTORY_HOURS)).timestamp() * 1000)
        while self.history and self.history[0]["ts"] < cutoff_ms:
            self.history.popleft()
        while self.status_events and self.status_events[-1]["ts"] < cutoff_ms:
            self.status_events.pop()
