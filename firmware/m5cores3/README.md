# NPB Monitor CoreS3 Firmware

Standalone M5 CoreS3 monitor/controller for a Mean Well NPB-450-24NFC using Unit CAN on Port.C and Unit ACSSR on the I2C Grove port.

## Hardware

- M5Stack CoreS3
- M5Stack Unit CAN on CoreS3 Port.C
  - CoreS3 Port.C: yellow `G17`, white `G18`
  - Unit CAN Grove: `CAN_TX`, `CAN_RX`
  - Firmware uses ESP32-S3 TWAI at `250 kbps`
- M5Stack Unit ACSSR on CoreS3 Port.A I2C
  - CoreS3 Port.A: `SDA=G2`, `SCL=G1`
  - ACSSR must be configured into I2C mode before use
- CoreS3 powered from charger 12V auxiliary supply
- Battery remains connected so CoreS3 can continue running during a 10 second charger AC input power cycle

Docs used for pin/function choices:

- Unit CAN: https://docs.m5stack.com/en/unit/can
- CoreS3 pin map: https://docs.m5stack.com/en/core/CoreS3
- Unit ACSSR I2C example: https://docs.m5stack.com/en/arduino/projects/unit/unit_acssr

## Configure

Edit `src/config.h`:

```cpp
static constexpr const char* WIFI_SSID     = "YOUR_WIFI_SSID";
static constexpr const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
static constexpr const char* MDNS_NAME     = "npb";
```

The web UI is available at:

```text
http://npb.local/
```

or by IP address shown on the CoreS3 maintenance screen.

## Build And Flash

```powershell
cd firmware\m5cores3
pio run
pio run -t upload
pio device monitor
```

## Local UI

The CoreS3 touch UI has four screens:

- Main: mode, voltage, current, power, temperature, operation state, Start, Stop, AC Cycle
- Flags: important charger/fault/status bits
- Maintenance: IP, mdns name, SSID, ACSSR state, SD state/capacity, current log
- Logs: web download address and current in-memory sample count

## Web UI

The device serves a phone-friendly local web UI with:

- live voltage/current/power/temperature
- status flags
- Start and Stop operation buttons
- AC power-cycle button; turns ACSSR off for 10 seconds, then back on
- SD session log listing and CSV downloads
- current in-memory history CSV download at `/history.csv`

## Data Logging

The firmware keeps up to 24 hours of one-sample-per-second history in PSRAM. If PSRAM allocation fails, it falls back to a smaller one-hour heap buffer.

SD card logs are stored under `/sessions`. A new CSV is created per charge session with this name format:

```text
YYYY-MM-DD HHmmSS.csv
```

The firmware prunes the oldest CSV files when SD usage exceeds 75% of card capacity. The active current session file is never deleted by the pruning pass.

## CAN Commands

The firmware polls the same readback/status registers as the Python/WiCAN app:

- `0x0000` operation
- `0x0020` target voltage
- `0x0030` target current
- `0x0040` fault status
- `0x0060` output voltage
- `0x0061` output current
- `0x0062` temperature
- `0x00B0..0x00B4` charge curve/config
- `0x00B8` charge status
- `0x00B9` restart voltage
- `0x00C1` system status
- `0x00C2` system config

Start/Stop sends `OPERATION 0x0000` with one data byte `0x01` or `0x00`.
