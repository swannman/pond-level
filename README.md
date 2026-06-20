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

Two kinds of config, deliberately separated:

- **Compile-time tunables — `include/config.h`** (override via `-D` in
  `platformio.ini`): UART pins/baud, validation range, median window,
  `SENSOR_OFFSET_MM`, push interval, watchdog timeout, `FIRMWARE_VERSION`, and
  the pull-OTA target (`OTA_GITHUB_OWNER` / `OTA_GITHUB_REPO`). None of these
  are secret.
- **Runtime secrets — NVS flash** (WiFi SSID/password, Grafana OTLP URL /
  instance id / token). These are **not compiled into the binary** — they're
  provisioned once over the serial console and stored in NVS, so release images
  are safe to publish publicly for OTA.

### Provisioning (serial console)

On a fresh device (empty NVS) the firmware boots into a provisioning console.
You can also enter it anytime by typing `config` + Enter in a serial monitor.

```
ssid <your-wifi-ssid>
pass <your-wifi-password>
url https://prometheus-prod-XX-prod-us-west-0.grafana.net/otlp/v1/metrics
id <grafana-instance-id>
token <grafana-access-policy-token>
show     # verify (secrets masked)
save     # persist to NVS
exit
```

`clear` wipes stored config (factory reset of credentials).

### Calibration

`level_mm = SENSOR_OFFSET_MM - distance_mm`. Set `SENSOR_OFFSET_MM` to the
measured distance (mm) from the sensor face to your chosen zero datum, taken
against one known waterline. Until calibrated, `pond_level_mm` is relative.

## Build / flash

```bash
pio run                      # build
pio run -t upload            # first flash over USB
pio device monitor           # serial log @ 115200 — then provision (above)
```

## OTA — pull-based from public GitHub releases

The enclosure is sealed and the node lives on an isolated IoT VLAN that blocks
inbound/connect-back connections, so push-OTA (ArduinoOTA/espota) can't reach
it. Instead the node **pulls** firmware: it queries the latest GitHub release
and self-flashes if the tag differs from `FIRMWARE_VERSION`. All connections are
device-initiated outbound, so they pass the firewall. Because secrets live in
NVS (not the binary), the release `firmware.bin` is safe to publish publicly.

Setup:

1. Set `OTA_GITHUB_OWNER` (and repo) in `platformio.ini`; push this repo to a
   **public** GitHub repo of that name.
2. To ship an update: bump `-DFIRMWARE_VERSION` to the new version, `pio run`,
   and publish a GitHub **release** tagged with that version, attaching
   `.pio/build/esp32doit/firmware.bin` as a release asset named `firmware.bin`.
3. Within `OTA_CHECK_INTERVAL_MS` (default 6 h) each node notices the new tag,
   downloads it, flashes, and reboots into it.

> The published `firmware.bin` must itself carry the new `FIRMWARE_VERSION`
> (it's compiled in), otherwise nodes would re-flash the same version on a loop.

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
