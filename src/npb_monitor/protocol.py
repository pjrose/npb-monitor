from __future__ import annotations

from dataclasses import dataclass
from typing import Any


@dataclass(frozen=True, slots=True)
class CommandSpec:
    code: int
    name: str
    scale: float | None
    unit: str
    writable: bool
    description: str


COMMANDS: dict[int, CommandSpec] = {
    0x0000: CommandSpec(0x0000, "OPERATION", None, "", True, "Output on/off control"),
    0x0020: CommandSpec(0x0020, "VOUT_SET", 0.01, "V", True, "PSU output voltage target"),
    0x0030: CommandSpec(0x0030, "IOUT_SET", 0.01, "A", True, "PSU output current target"),
    0x0040: CommandSpec(0x0040, "FAULT_STATUS", None, "", False, "Fault flags"),
    0x0050: CommandSpec(0x0050, "READ_VIN", 0.1, "V", False, "Input voltage"),
    0x0060: CommandSpec(0x0060, "READ_VOUT", 0.01, "V", False, "Measured output voltage"),
    0x0061: CommandSpec(0x0061, "READ_IOUT", 0.01, "A", False, "Measured output current"),
    0x0062: CommandSpec(0x0062, "READ_TEMPERATURE_1", 0.1, "C", False, "Internal ambient temperature"),
    0x00B0: CommandSpec(0x00B0, "CURVE_CC", 0.01, "A", True, "Charge curve constant current"),
    0x00B1: CommandSpec(0x00B1, "CURVE_CV", 0.01, "V", True, "Charge curve constant voltage"),
    0x00B2: CommandSpec(0x00B2, "CURVE_FV", 0.01, "V", True, "Charge curve float voltage"),
    0x00B3: CommandSpec(0x00B3, "CURVE_TC", 0.01, "A", True, "Charge curve taper current"),
    0x00B4: CommandSpec(0x00B4, "CURVE_CONFIG", None, "", True, "Charge curve configuration"),
    0x00B5: CommandSpec(0x00B5, "CURVE_CC_TIMEOUT", 1.0, "min", True, "CC timeout"),
    0x00B6: CommandSpec(0x00B6, "CURVE_CV_TIMEOUT", 1.0, "min", True, "CV timeout"),
    0x00B7: CommandSpec(0x00B7, "CURVE_FV_TIMEOUT", 1.0, "min", True, "Float timeout"),
    0x00B8: CommandSpec(0x00B8, "CHG_STATUS", None, "", False, "Charge status flags"),
    0x00B9: CommandSpec(0x00B9, "CHG_RST_VBAT", 0.01, "V", True, "Voltage to restart charging after full"),
    0x00C1: CommandSpec(0x00C1, "SYSTEM_STATUS", None, "", False, "System status flags"),
    0x00C2: CommandSpec(0x00C2, "SYSTEM_CONFIG", None, "", True, "System configuration"),
}

POLL_COMMANDS = [
    0x0000,
    0x0020,
    0x0030,
    0x0040,
    0x0060,
    0x0061,
    0x0062,
    0x00B0,
    0x00B1,
    0x00B2,
    0x00B3,
    0x00B4,
    0x00B8,
    0x00B9,
    0x00C1,
    0x00C2,
]


def controller_can_id(address: int) -> int:
    validate_address(address)
    return 0x000C0100 | address


def charger_can_id(address: int) -> int:
    validate_address(address)
    return 0x000C0000 | address


def charger_address_from_can_id(can_id: int) -> int | None:
    if 0x000C0000 <= can_id <= 0x000C0003:
        return can_id & 0x03
    return None


def validate_address(address: int) -> None:
    if address < 0x00 or address > 0x03:
        raise ValueError("NPB CAN address must be in range 0x00..0x03")


def command_bytes(command: int, raw_value: int | None = None) -> list[int]:
    validate_command(command)
    data = [command & 0xFF, (command >> 8) & 0xFF]
    if raw_value is not None:
        if raw_value < 0 or raw_value > 0xFFFF:
            raise ValueError("Raw command value must fit in 16 bits")
        data += [raw_value & 0xFF, (raw_value >> 8) & 0xFF]
    return data


def scaled_command_bytes(command: int, value: float | int) -> list[int]:
    spec = get_command(command)
    if spec.scale is None:
        if not isinstance(value, int):
            raise ValueError(f"{spec.name} requires a raw integer value")
        return command_bytes(command, value)
    raw = int(round(float(value) / spec.scale))
    return command_bytes(command, raw)


def operation_bytes(enabled: bool) -> list[int]:
    return [0x00, 0x00, 0x01 if enabled else 0x00]


def wican_tx_payload(can_id: int, data: list[int]) -> dict[str, Any]:
    if not 0 <= can_id <= 0x1FFFFFFF:
        raise ValueError("CAN ID must fit a 29-bit extended identifier")
    if not 0 <= len(data) <= 8:
        raise ValueError("CAN payload must contain 0..8 bytes")
    for byte in data:
        if not 0 <= byte <= 0xFF:
            raise ValueError("CAN data bytes must fit 0..255")
    return {
        "bus": "0",
        "type": "tx",
        "frame": [
            {
                "id": can_id,
                "dlc": len(data),
                "rtr": False,
                "extd": True,
                "data": data,
            }
        ],
    }


def parse_raw_bytes(text: str) -> list[int]:
    parts = text.replace(",", " ").split()
    out: list[int] = []
    for part in parts:
        out.append(int(part, 0))
    if len(out) > 8:
        raise ValueError("A CAN data frame can contain at most 8 bytes")
    if any(byte < 0 or byte > 255 for byte in out):
        raise ValueError("CAN data bytes must fit 0..255")
    return out


def decode_frame(can_id: int, data: list[int], expected_address: int | None = None) -> dict[str, Any]:
    if len(data) < 2:
        raise ValueError("NPB reply must contain at least command low/high bytes")
    if expected_address is not None and can_id != charger_can_id(expected_address):
        raise ValueError(f"Unexpected charger CAN ID 0x{can_id:X}")
    device_address = charger_address_from_can_id(can_id)

    command = data[0] | (data[1] << 8)
    spec = COMMANDS.get(command, CommandSpec(command, f"UNKNOWN_0x{command:04X}", None, "", False, "Unknown command"))
    raw_value = None
    value = None
    if len(data) >= 4:
        raw_value = data[2] | (data[3] << 8)
        if spec.scale is not None:
            value = raw_value * spec.scale

    decoded: dict[str, Any] = {
        "can_id": can_id,
        "device_address": device_address,
        "command": command,
        "name": spec.name,
        "raw_value": raw_value,
        "value": value,
        "unit": spec.unit,
        "data": data,
    }

    if command == 0x0000 and len(data) >= 3:
        decoded["value"] = bool(data[2])
        decoded["flags"] = {"operation_on": bool(data[2])}
    elif command == 0x0040 and len(data) >= 3:
        decoded["flags"] = decode_fault_status(data[2])
    elif command == 0x00B4 and len(data) >= 4:
        decoded["flags"] = decode_curve_config(data[2], data[3])
    elif command == 0x00B8 and len(data) >= 4:
        decoded["flags"] = decode_charge_status(data[2], data[3])
    elif command == 0x00C1 and len(data) >= 3:
        decoded["flags"] = decode_system_status(data[2])
    elif command == 0x00C2 and len(data) >= 4:
        decoded["flags"] = decode_system_config(data[2], data[3])

    return decoded


def decode_fault_status(low: int) -> dict[str, bool]:
    return {
        "over_temperature": bool(low & 0x02),
        "output_over_voltage": bool(low & 0x04),
        "output_over_current": bool(low & 0x08),
        "output_short_circuit": bool(low & 0x10),
        "ac_abnormal": bool(low & 0x20),
        "output_off": bool(low & 0x40),
        "internal_temperature_abnormal": bool(low & 0x80),
    }


def decode_charge_status(low: int, high: int) -> dict[str, bool]:
    return {
        "fully_charged": bool(low & 0x01),
        "constant_current_mode": bool(low & 0x02),
        "constant_voltage_mode": bool(low & 0x04),
        "float_mode": bool(low & 0x08),
        "wake_up_unfinished": bool(low & 0x40),
        "temperature_compensation_short": bool(high & 0x04),
        "no_battery_detected": bool(high & 0x08),
        "cc_timeout": bool(high & 0x20),
        "cv_timeout": bool(high & 0x40),
        "float_timeout": bool(high & 0x80),
    }


def decode_system_status(low: int) -> dict[str, bool]:
    return {
        "dc_output_normal_range": bool(low & 0x02),
        "initial_state": bool(low & 0x20),
        "eeprom_access_error": bool(low & 0x40),
    }


def decode_curve_config(low: int, high: int) -> dict[str, Any]:
    return {
        "charger_mode": bool(low & 0x80),
        "restart_after_full_enabled": bool(high & 0x08),
        "curve_select": low & 0x03,
        "temperature_compensation": (high >> 2) & 0x03,
        "raw_low": low,
        "raw_high": high,
    }


def decode_system_config(low: int, high: int) -> dict[str, Any]:
    return {
        "operation_init": (low >> 1) & 0x03,
        "disable_eeprom_writes": bool(high & 0x04),
        "raw_low": low,
        "raw_high": high,
    }


def get_command(command: int) -> CommandSpec:
    validate_command(command)
    return COMMANDS[command]


def validate_command(command: int) -> None:
    if command not in COMMANDS:
        raise ValueError(f"Unsupported named command 0x{command:04X}; use raw CAN for manual-only commands")
