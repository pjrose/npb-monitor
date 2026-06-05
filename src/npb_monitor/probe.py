from __future__ import annotations

import argparse
import json
import threading
import time
from typing import Any

from . import protocol

try:
    import paho.mqtt.client as mqtt
except ImportError:  # pragma: no cover
    mqtt = None  # type: ignore[assignment]


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Probe Mean Well NPB replies through WiCAN MQTT")
    parser.add_argument("--mqtt-host", default="192.168.4.73")
    parser.add_argument("--mqtt-port", type=int, default=1883)
    parser.add_argument("--mqtt-username")
    parser.add_argument("--mqtt-password")
    parser.add_argument("--device-id", default="f412fa2e9d59")
    parser.add_argument("--seconds", type=float, default=8.0)
    parser.add_argument("--gap", type=float, default=0.08)
    return parser


def main() -> None:
    if mqtt is None:
        raise RuntimeError("paho-mqtt is not installed")
    args = build_parser().parse_args()
    tx_topic = f"wican/{args.device_id}/can/tx"
    root_topic = f"wican/{args.device_id}/#"
    stop = threading.Event()
    rx_count = 0

    def on_connect(client: Any, userdata: Any, flags: Any, reason_code: Any, properties: Any = None) -> None:
        print(f"MQTT connected: {reason_code}")
        client.subscribe(root_topic)

    def on_message(client: Any, userdata: Any, msg: Any) -> None:
        nonlocal rx_count
        text = msg.payload.decode("utf-8", errors="replace")
        print(f"{msg.topic} {text}")
        if not msg.topic.endswith("/can/rx"):
            return
        rx_count += 1
        try:
            payload = json.loads(text)
            for frame in payload.get("frame", []):
                decoded = protocol.decode_frame(int(frame["id"]), [int(byte) for byte in frame.get("data", [])])
                address = decoded.get("device_address")
                address_text = "--" if address is None else f"0x{address:02X}"
                print(f"  decoded addr={address_text} {decoded['name']} raw={decoded['raw_value']} value={decoded['value']} {decoded['unit']}")
        except Exception as exc:
            print(f"  decode error: {exc}")

    client = mqtt.Client()
    if args.mqtt_username:
        client.username_pw_set(args.mqtt_username, args.mqtt_password)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(args.mqtt_host, args.mqtt_port, keepalive=30)
    client.loop_start()
    time.sleep(1.0)

    commands = (0x0000, 0x0060, 0x0061, 0x0062, 0x00B8, 0x00C1)
    print("Sending read probes to NPB addresses 0x00..0x03")
    for address in range(4):
        for command in commands:
            can_id = protocol.controller_can_id(address)
            payload = protocol.wican_tx_payload(can_id, protocol.command_bytes(command))
            client.publish(tx_topic, json.dumps(payload, separators=(",", ":")))
            print(f"TX addr=0x{address:02X} id=0x{can_id:X} cmd=0x{command:04X}")
            time.sleep(args.gap)

    deadline = time.time() + args.seconds
    while time.time() < deadline:
        time.sleep(0.1)
    stop.set()
    client.loop_stop()
    client.disconnect()
    print(f"Probe complete. RX messages seen: {rx_count}")


if __name__ == "__main__":
    main()

