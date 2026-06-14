# pond-level

ESP32 firmware for an outdoor pond water-level sensor. A downward-facing
DFRobot **A02YYUW** ultrasonic sensor measures the distance to a calm water
surface; the firmware median-filters it, converts it to a water level, and
pushes both straight to **Grafana Cloud** over OTLP HTTP/JSON (Prometheus).
Mains-powered, always-on, reflashable over WiFi (OTA).

```
A02YYUW (UART 9600) -> ESP32 (parse + median filter + level convert)
                    -> WiFi -> Grafana Cloud (OTLP / Prometheus) -> Grafana
```

There is **no MQTT and no Home Assistant** — the node writes metrics directly
to Grafana Cloud, mirroring the `lora-sensor/receiver-node` push layer.

## Hardware / wiring

| A02YYUW | ESP32 (DOIT DevKit V1) | Notes |
|---|---|---|
| VCC | `3V3` | 3.3 V keeps UART logic at ESP32 level |
| GND | `GND` | |
| TX  | `GPIO16` (RX2) | sensor data out → ESP32 in |
| RX  | `GPIO17` (TX2) | unused in default continuous mode |

UART2, 9600 8N1. Do **not** use UART0 (USB console). Pins are configurable in
`platformio.ini` (`SENSOR_RX_PIN` / `SENSOR_TX_PIN`).

## Sensor protocol

Continuous mode streams a 4-byte frame ~10 Hz:

```
[0]=0xFF header  [1]=DATA_H  [2]=DATA_L  [3]=SUM
distance_mm = (DATA_H<<8)|DATA_L
checksum OK if SUM == ((0xFF + DATA_H + DATA_L) & 0xFF)
```

The parser resyncs on `0xFF`, validates the checksum, rejects anything outside
`MIN_VALID_MM..MAX_VALID_MM` (30–4500 mm), and feeds a sliding median window
(`MEDIAN_WINDOW`, default 5) to absorb condensation/no-echo spikes.

## Configuration

- **`include/config.h`** — tunables with `#ifndef` defaults; override via `-D`
  flags in `platformio.ini` (UART pins/baud, validation range, median window,
  `SENSOR_OFFSET_MM`, push interval, watchdog timeout).
- **`include/secrets.h`** — WiFi, Grafana Cloud endpoint/credentials, OTA
  password. Gitignored. Copy `secrets.h.example` → `secrets.h` and fill it in.

### Calibration

`level_mm = SENSOR_OFFSET_MM - distance_mm`. Set `SENSOR_OFFSET_MM` to the
measured distance (mm) from the sensor face to your chosen zero datum, taken
against one known waterline. Until calibrated, `pond_level_mm` is relative.

## Build / flash

```bash
cp include/secrets.h.example include/secrets.h   # then edit values
pio run                      # build
pio run -t upload            # first flash over USB
pio device monitor           # serial log @ 115200
```

### OTA (sealed enclosure)

After the first USB flash, uncomment the `upload_protocol`/`upload_port`/
`upload_flags` lines in `platformio.ini` (set the auth to `OTA_PASSWORD`), then
`pio run -t upload` pushes firmware over WiFi to `esp32-pond.local`.

## Metrics (Grafana Cloud / Prometheus)

Pushed every `METRICS_PUSH_INTERVAL_MS` (default 30 s), labelled
`service_name="pond-level"`, `host_name="esp32-pond"`:

| Metric | Type | Meaning |
|---|---|---|
| `pond_distance_mm` | gauge | median sensor-to-water distance |
| `pond_level_mm` | gauge | water level above datum (`offset − distance`) |
| `pond_sample_age_seconds` | gauge | seconds since last valid sample (staleness) |
| `pond_wifi_rssi_dbm` | gauge | WiFi signal strength |
| `pond_uptime_seconds` | gauge | seconds since boot |
| `pond_sensor_frames_total` | counter | valid frames accepted |
| `pond_sensor_bad_frames_total` | counter | frames rejected (checksum/range) |

`pond_distance_mm` / `pond_level_mm` / `pond_sample_age_seconds` are only
emitted once at least one valid sample exists.

## Notes

- **No temperature compensation.** Speed of sound drifts ~0.17%/°C and the
  A02YYUW doesn't correct for it — expect ~cm drift over outdoor temperature
  swings. A temperature-correction hook can be added later.
- **Watchdog**: `esp_task_wdt` reboots on a hang (60 s, wide enough to ride out
  transient WiFi reconnects).

## Local build environment

This repo's `pio` is installed under `uv` (Python 3.14). The pioarduino
espressif32 platform needs extra Python packages in that env; if a build fails
with `No module named 'fatfs'`/`yaml`/etc., install them into pio's env:

```bash
uv pip install --python ~/.local/share/uv/tools/platformio/bin/python \
  littlefs-python fatfs-ng pyyaml rich-click zopfli intelhex rich 'urllib3<2' \
  cryptography certifi ecdsa bitstring 'reedsolo<1.8' esp-idf-size esp-coredump
```
