# CoreS3-SE direct-CAN firmware (first hardware-verification build)

This is a new, **read-only** embedded target. It does not replace or modify
the original Python+WiCAN desktop application.

## Hardware

- M5Stack CoreS3-SE + genuine M5Stack **Base DIN M132**
- M5Stack **Unit CAN U085** (isolated version, not Unit Mini CAN)
- Mean Well NPB-450-24NFC with CAN support, configured for address 0x00

| Connection | Where |
|---|---|
| 12VDC, center-positive barrel input | Base DIN DC5521, rated 9–24 VDC |
| Unit CAN Grove | Base DIN **PORT.C** (GND, +5V, GPIO17 TX, GPIO18 RX) |
| CAN-H/CAN-L | Unit CAN screw terminal to NPB CAN-H/CAN-L |

The Base DIN has its own 9–24 VDC converter to the Core 5V rail. Do **not**
feed 12 V into the 5 V Grove port. Verify your Base DIN version and pin
assignment before applying power. The Unit CAN has its own isolation circuitry;
use an end-of-bus 120-ohm termination only where required by the actual bus
topology; the U085 includes a supplied 120-ohm resistor.

The NPB bus uses **250,000 bit/s**, extended 29-bit CAN:
- TX controller -> charger: 0x000C0100 | address
- RX charger -> controller: 0x000C0000 | address
- Requests: 2-byte little-endian command code (no write value)
- Response: little-endian command code followed by little-endian raw data

These values are copied from the working Python protocol implementation in
src/npb_monitor/protocol.py, not assumed from generic CAN conventions.

## Build and flash

Use VS Code with PlatformIO, or the PlatformIO CLI:

    cd firmware/cores3se
    pio run -e cores3se
    pio run -e cores3se -t upload
    pio device monitor -b 115200

Optional: copy include/secrets.example.h -> include/secrets.h and fill in your
Wi-Fi credentials. If no credentials are supplied (or STA connection fails),
the device starts a password-protected Wi-Fi access point. Its SSID and randomly
generated password appear on the touchscreen and serial port. Connect your
phone/PC to that AP and browse http://192.168.4.1.

If STA connects, browse the IP displayed on the CoreS3-SE screen.

## Firmware behavior

- Native ESP32-S3 TWAI on GPIO17/TX and GPIO18/RX; no MCP2515 or WiCAN.
- Polls one **read-only** register every 180 ms; no implicit CAN-control,
  EEPROM, charger-mode, operation, or setpoint writes.
- Shows output voltage, output current, temperature, charge phase and bus
  connectivity on the touchscreen.
- Hosts an offline HTML+JS status page for PCs/phones with live JSON at
  /api/status and a small in-memory history via /api/history.
- Keeps 24h maximum of history in RAM (one sample per five minutes); history
  is lost on reboot. 24h history and Wh are approximate and not for billing.
- Unknown/disconnected/stale values are displayed as missing, not zero.

## Not yet implemented

- Charger parameter writes / write readback verification / activation notices.
- Relay or AC contactor control. No AC mains wiring should connect to the M5.
- MQTT broker/client. Direct CAN makes MQTT unnecessary for standalone use;
  an optional LAN publisher can be added without blocking CAN operation.
- Firmware OTA, persistent history, login/authentication, and fully interactive
  touchscreen controls.
- Real-device validation. A successful compile is not a substitute for testing
  on your particular CAN wiring/charger.

### Hardware bring-up checklist

1. With NPB **AC unplugged**, power the DIN base from a current-limited 12VDC
   supply and verify the CoreS3-SE boots/display works.
2. Connect the Unit CAN to **PORT.C**, not PORT.A.
3. Connect CAN-H/CAN-L; check termination and continuity while power is off.
4. Power the charger, keep the output disconnected or controlled by the charger
   and BMS as appropriate, then watch serial and /api/status for RX frames.
5. Confirm 0x0060 voltage, 0x0061 current and 0x0062 temperature against an
   independent reading. Confirm fault bits and stale status by unplugging CAN.
6. Only after reliable monitoring, implement and test authenticated control
   and independent fail-safe contactor hardware.

## Power and safety

The Base DIN's internal battery is a short ride-through aid, **not** a
safety-rated UPS. A 450 W / 120 VAC charger draws about 3.75 A nominal and may
have significant input inrush. Do not switch its mains through the small M5
relay/SSR units. Use an appropriately rated, enclosed and protected contactor
with independent safety interlocks; leave power switching disconnected during
initial firmware validation.
