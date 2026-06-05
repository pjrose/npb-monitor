from __future__ import annotations

from dataclasses import dataclass


@dataclass(slots=True)
class AppConfig:
    http_host: str = "127.0.0.1"
    http_port: int = 8088
    mqtt_host: str = "192.168.4.73"
    mqtt_port: int = 1883
    mqtt_username: str | None = None
    mqtt_password: str | None = None
    tx_topic: str = "wican/f412fa2e9d59/can/tx"
    rx_topic: str = "wican/f412fa2e9d59/can/rx"
    status_topic: str = "wican/f412fa2e9d59/can/stat"
    device_address: int = 0x03
    mqtt_enabled: bool = True
    poll_interval_s: float = 1.0
    packet_gap_s: float = 0.05

    @property
    def controller_can_id(self) -> int:
        return 0x000C0100 | self.device_address

    @property
    def charger_can_id(self) -> int:
        return 0x000C0000 | self.device_address

