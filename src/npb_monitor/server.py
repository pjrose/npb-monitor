from __future__ import annotations

import argparse
import json
import mimetypes
from dataclasses import asdict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import urlparse

from .bridge import WicanBridge
from .config import AppConfig
from .protocol import COMMANDS, parse_raw_bytes
from .state import MonitorState

STATIC_DIR = Path(__file__).with_name("static")


class NpbRequestHandler(BaseHTTPRequestHandler):
    bridge: WicanBridge
    state: MonitorState
    config: AppConfig

    def log_message(self, format: str, *args: Any) -> None:
        message = format % args
        if not message.startswith('"GET '):
            self.state.add_event("http", message)

    def do_GET(self) -> None:
        path = urlparse(self.path).path
        if path in ("/", "/index.html"):
            self._send_static("index.html")
        elif path == "/history":
            self._send_static("history.html")
        elif path.startswith("/static/"):
            self._send_static(path.removeprefix("/static/"))
        elif path == "/api/state":
            self._send_json(self.state.snapshot())
        elif path == "/api/history":
            self._send_json(self.state.history_snapshot())
        elif path == "/api/history.csv":
            self._send_history_csv()
        elif path == "/api/config":
            self._send_json(
                {
                    "mqtt_host": self.config.mqtt_host,
                    "mqtt_port": self.config.mqtt_port,
                    "tx_topic": self.config.tx_topic,
                    "rx_topic": self.config.rx_topic,
                    "status_topic": self.config.status_topic,
                    "device_address": self.config.device_address,
                    "controller_can_id": f"0x{self.config.controller_can_id:X}",
                    "charger_can_id": f"0x{self.config.charger_can_id:X}",
                }
            )
        elif path == "/api/commands":
            self._send_json({f"0x{code:04X}": asdict(spec) for code, spec in COMMANDS.items()})
        else:
            self.send_error(404)

    def do_POST(self) -> None:
        try:
            body = self._read_json()
            if self.path == "/api/control":
                result = self._handle_control(body)
            elif self.path == "/api/raw":
                result = self._handle_raw(body)
            elif self.path == "/api/poll":
                self.bridge.poll_once()
                result = {"ok": True}
            else:
                self.send_error(404)
                return
        except Exception as exc:
            self._send_json({"ok": False, "error": str(exc)}, status=400)
            return
        self._send_json(result)

    def _handle_control(self, body: dict[str, Any]) -> dict[str, Any]:
        action = body.get("action")
        if action == "operation":
            self.bridge.set_operation(bool(body["enabled"]))
            self.state.add_event("control", f"requested operation {'on' if body['enabled'] else 'off'}")
        elif action == "set_psu_voltage":
            self.bridge.publish_scaled_write(0x0020, float(body["value"]))
            self.state.add_event("control", f"requested PSU voltage {float(body['value']):.2f} V")
        elif action == "set_psu_current":
            self.bridge.publish_scaled_write(0x0030, float(body["value"]))
            self.state.add_event("control", f"requested PSU current {float(body['value']):.2f} A")
        elif action == "set_curve_cc":
            self.bridge.publish_scaled_write(0x00B0, float(body["value"]))
            self.state.add_event("control", f"requested charge CC {float(body['value']):.2f} A")
        elif action == "set_curve_cv":
            self.bridge.publish_scaled_write(0x00B1, float(body["value"]))
            self.state.add_event("control", f"requested charge CV {float(body['value']):.2f} V")
        elif action == "set_curve_float":
            self.bridge.publish_scaled_write(0x00B2, float(body["value"]))
            self.state.add_event("control", f"requested float voltage {float(body['value']):.2f} V")
        elif action == "set_curve_taper":
            self.bridge.publish_scaled_write(0x00B3, float(body["value"]))
            self.state.add_event("control", f"requested taper current {float(body['value']):.2f} A")
        elif action == "set_restart_voltage":
            self.bridge.publish_scaled_write(0x00B9, float(body["value"]))
            self.state.add_event("control", f"requested restart voltage {float(body['value']):.2f} V")
        elif action == "curve_config_raw":
            low = int(body["low"], 0) if isinstance(body["low"], str) else int(body["low"])
            high = int(body["high"], 0) if isinstance(body["high"], str) else int(body["high"])
            self.bridge.publish_raw_write(0x00B4, low | (high << 8))
            self.state.add_event("control", f"requested CURVE_CONFIG low=0x{low:02X} high=0x{high:02X}")
        elif action == "system_config_raw":
            low = int(body["low"], 0) if isinstance(body["low"], str) else int(body["low"])
            high = int(body["high"], 0) if isinstance(body["high"], str) else int(body["high"])
            self.bridge.publish_raw_write(0x00C2, low | (high << 8))
            self.state.add_event("control", f"requested SYSTEM_CONFIG low=0x{low:02X} high=0x{high:02X}")
        elif action == "initialize_psu":
            self.bridge.initialize_power_supply_mode()
        elif action == "charger_mode_default":
            self.bridge.set_charger_mode_default()
        elif action == "disable_eeprom_writes":
            self.bridge.disable_eeprom_writes()
        elif action == "read_command":
            command = int(body["command"], 0) if isinstance(body["command"], str) else int(body["command"])
            self.bridge.publish_read(command)
        else:
            raise ValueError(f"Unknown control action: {action}")
        return {"ok": True}

    def _handle_raw(self, body: dict[str, Any]) -> dict[str, Any]:
        can_id = int(body.get("can_id", self.config.controller_can_id), 0) if isinstance(body.get("can_id"), str) else int(body.get("can_id", self.config.controller_can_id))
        data = parse_raw_bytes(str(body["data"]))
        self.bridge.publish_data(data, can_id=can_id)
        return {"ok": True}

    def _read_json(self) -> dict[str, Any]:
        length = int(self.headers.get("content-length", "0"))
        raw = self.rfile.read(length).decode("utf-8")
        return json.loads(raw) if raw else {}

    def _send_json(self, payload: Any, status: int = 200) -> None:
        data = json.dumps(payload, indent=2).encode("utf-8")
        self.send_response(status)
        self.send_header("content-type", "application/json; charset=utf-8")
        self.send_header("content-length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _send_history_csv(self) -> None:
        history = self.state.history_snapshot()["samples"]
        lines = ["at,voltage,current,power,temperature"]
        for sample in history:
            lines.append(
                ",".join(
                    [
                        str(sample.get("at", "")),
                        _csv_number(sample.get("voltage")),
                        _csv_number(sample.get("current")),
                        _csv_number(sample.get("power")),
                        _csv_number(sample.get("temperature")),
                    ]
                )
            )
        data = ("\n".join(lines) + "\n").encode("utf-8")
        self.send_response(200)
        self.send_header("content-type", "text/csv; charset=utf-8")
        self.send_header("content-disposition", 'attachment; filename="npb-history-24h.csv"')
        self.send_header("content-length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _send_static(self, name: str) -> None:
        target = (STATIC_DIR / name).resolve()
        if STATIC_DIR.resolve() not in target.parents and target != STATIC_DIR.resolve():
            self.send_error(403)
            return
        if not target.exists() or not target.is_file():
            self.send_error(404)
            return
        data = target.read_bytes()
        self.send_response(200)
        self.send_header("content-type", mimetypes.guess_type(str(target))[0] or "application/octet-stream")
        self.send_header("content-length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def _csv_number(value: Any) -> str:
    if value is None:
        return ""
    return str(value)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Mean Well NPB web monitor via WiCAN MQTT")
    parser.add_argument("--http-host", default="127.0.0.1")
    parser.add_argument("--http-port", type=int, default=8088)
    parser.add_argument("--mqtt-host", default="192.168.4.73")
    parser.add_argument("--mqtt-port", type=int, default=1883)
    parser.add_argument("--mqtt-username")
    parser.add_argument("--mqtt-password")
    parser.add_argument("--tx-topic", default="wican/f412fa2e9d59/can/tx")
    parser.add_argument("--rx-topic", default="wican/f412fa2e9d59/can/rx")
    parser.add_argument("--status-topic", default="wican/f412fa2e9d59/can/stat")
    parser.add_argument("--device-address", type=lambda text: int(text, 0), default=0x03)
    parser.add_argument("--poll-interval", type=float, default=1.0)
    parser.add_argument("--no-mqtt", action="store_true")
    return parser


def main() -> None:
    args = build_parser().parse_args()
    config = AppConfig(
        http_host=args.http_host,
        http_port=args.http_port,
        mqtt_host=args.mqtt_host,
        mqtt_port=args.mqtt_port,
        mqtt_username=args.mqtt_username,
        mqtt_password=args.mqtt_password,
        tx_topic=args.tx_topic,
        rx_topic=args.rx_topic,
        status_topic=args.status_topic,
        device_address=args.device_address,
        mqtt_enabled=not args.no_mqtt,
        poll_interval_s=args.poll_interval,
    )
    state = MonitorState()
    bridge = WicanBridge(config, state)
    bridge.start()

    NpbRequestHandler.bridge = bridge
    NpbRequestHandler.state = state
    NpbRequestHandler.config = config
    server = ThreadingHTTPServer((config.http_host, config.http_port), NpbRequestHandler)
    print(f"NPB monitor running at http://{config.http_host}:{config.http_port}")
    print(f"MQTT broker {config.mqtt_host}:{config.mqtt_port}, TX {config.tx_topic}, RX {config.rx_topic}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        bridge.stop()
        server.server_close()


if __name__ == "__main__":
    main()
