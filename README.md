# NPB Monitor

Web monitor and control interface for a Mean Well NPB-450-24NFC charger on a LAN via a WiCAN MQTT-to-CAN bridge.

Defaults match the attached WiCAN configuration:

- MQTT broker: `192.168.4.73:1883`
- TX topic: `wican/f412fa2e9d59/can/tx`
- RX topic: `wican/f412fa2e9d59/can/rx`
- Status topic: `wican/f412fa2e9d59/can/stat`
- Charger CAN address: `0x03`

The charger protocol uses 250 kbps CAN 2.0B extended frames. Controller-to-charger CAN IDs are `0x000C01XX`, charger-to-controller IDs are `0x000C00XX`, and command/value bytes are sent little-endian. Writes do not generate callbacks, so the app polls status and readback registers after control actions.

## Run

```powershell
.\.venv\Scripts\Activate.ps1
pip install -e .
npb-monitor
```

Then open:

```text
http://127.0.0.1:8088
```

The 24-hour voltage/current chart is here:

```text
http://127.0.0.1:8088/history
```

The chart supports mouse-wheel zoom, drag-to-pan time scrolling, 1/6/12/24-hour range buttons, PNG export, and CSV export. Status-bit transitions such as constant current, constant voltage, float mode, output off, and fully charged are retained for the same 24-hour window and drawn as annotated vertical markers.

Useful overrides:

```powershell
npb-monitor --http-host 0.0.0.0 --http-port 8088 --mqtt-host 192.168.4.73 --device-address 3
npb-monitor --no-mqtt
```

## Mosquitto LAN check

WiCAN is configured to connect to `192.168.4.73:1883`, so Mosquitto must listen on the LAN interface, not only `127.0.0.1`. On this PC, check:

```powershell
Test-NetConnection -ComputerName 192.168.4.73 -Port 1883
Get-NetTCPConnection -LocalPort 1883
```

If the listener only shows `127.0.0.1` or `::1`, add a LAN listener to `mosquitto.conf`, then restart the `mosquitto` service:

```text
listener 1883 0.0.0.0
allow_anonymous true
```

Use authentication instead of `allow_anonymous true` if you configure a WiCAN MQTT username and password.

## Tests

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s tests
```

The tests verify CAN ID construction, little-endian command/write payloads, WiCAN MQTT frame JSON, and status/readback decoding.

## Command handling

Buttons and forms call `/api/control`. The server maps each action to a Mean Well command code, encodes command and value bytes little-endian, wraps the frame in WiCAN MQTT JSON, and publishes to the configured TX topic. Writes do not wait for a charger callback because the manual says there is no reply for writes; the app confirms changes by polling the corresponding readback/status registers.

Raw CAN is available through `/api/raw` and the Raw CAN panel. Named controls currently send:

- Operation: `0x0000`, one data byte `0x01` or `0x00`
- PSU voltage/current: `0x0020` / `0x0030`, factor `0.01`
- Charge curve CC/CV/float/taper/restart voltage: `0x00B0` / `0x00B1` / `0x00B2` / `0x00B3` / `0x00B9`, factor `0.01`
- Curve config: `0x00B4`, raw low/high bytes
- System config: `0x00C2`, raw low/high bytes

The `Enable CAN control` button sends `SYSTEM_CONFIG = low 0x03, high 0x04`, encoded on the wire as `[0xC2, 0x00, 0x03, 0x04]`. That sets the CAN-control low bit and also sets high-byte bit 10 to disable real-time EEPROM writes.
