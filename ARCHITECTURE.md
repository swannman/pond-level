# Pond Water-Level Sensor — Architecture & Build Guide

A from-scratch guide to building a WiFi-connected, OTA-updatable pond water-level
sensor that publishes directly to Grafana Cloud. This document explains **what to
buy, how to wire it, how the firmware is structured, and — most importantly — why
each design decision was made**, so you can reproduce or adapt it.

For day-to-day usage (provisioning commands, build/flash, metric list) see
[`README.md`](./README.md). This file is the design rationale.

---

## 1. What it does

A fixed, downward-facing ultrasonic sensor measures the distance to a calm pond
surface. The ESP32 converts that distance to a water level, median-filters out
noise, and pushes both values to Grafana Cloud every 30 s. It runs unattended,
survives WiFi/network dropouts, and can be reflashed over the air from a GitHub
release — the enclosure is sealed and never has to be opened again.

```
A02YYUW ultrasonic (UART 9600)
        │  4-byte distance frames, ~10 Hz
        ▼
   ESP32 (parse → checksum → median filter → level convert)
        │  OTLP HTTP/JSON, every 30 s
        ▼
   Grafana Cloud (Prometheus/Mimir) ──► Grafana dashboards
        ▲
        │  hourly + on-boot: GET latest release, self-flash if newer
   GitHub Releases (public firmware.bin)
```

There is **no MQTT broker and no Home Assistant** — the node writes metrics
straight to Grafana Cloud (see §5).

---

## 2. Bill of materials

| Component | Part | Notes |
|---|---|---|
| MCU board | ESP32 DOIT DevKit V1 | WROOM-32, USB (CP2102). GPIO16/17 free (no PSRAM). |
| Distance sensor | DFRobot A02YYUW (SEN0311) | Waterproof ultrasonic, IP67, 3–450 cm range, **3 cm blind zone**, ±1 cm, **UART 9600 8N1**, 3.3–5 V. Fully sealed/integrated. |
| Sensor power | ESP32 `3V3` pin | Powering at 3.3 V keeps UART logic at ESP32 level — no level shifter needed. |
| Sensor cable | JST PH 2.0 4-pin, extended to ~1.5–2 m with 26 AWG 4-conductor UL2464 | The link is digital UART, so extending it is safe. |
| Sensor housing | Polycase WP-23, inverted, lid off | Acts as a hood; sits on a grate over a calm spot; sensor fires straight down through the open bottom. |
| Electronics enclosure | Polycase WC-36 (IP66 polycarbonate) | Houses the ESP32. |
| Cable glands | CG3 M12 | For the sensor cable + USB power entry. |
| Breather vent | M12 IP67 ePTFE membrane | Equalizes pressure / prevents condensation buildup in a sealed box. |
| Power | USB 5 V (mains adapter) | Always-on; **no battery/sleep** is needed or implemented. |

### Mounting that matters for good readings
- Aim straight **down** at a **calm** patch of water. Keep it away from any
  aeration/bubbler column.
- The ultrasonic beam is a ~60° cone. Make sure the grate opening and the clear
  path down are **wider than the cone** so it reads water, not a grate edge or
  wall (edge hits show up as sudden out-of-band jumps).
- **Leave headroom above the full waterline.** The blind zone is 3 cm: if the
  full pond surface ends up within ~3 cm of the sensor face, those reads get
  rejected. Aim for the full-pond surface to sit comfortably **> 5–10 cm** below
  the sensor. (Ask me how I know.)

---

## 3. Wiring

| A02YYUW wire (by function) | ESP32 pin | Required? |
|---|---|---|
| VCC | `3V3` | yes — **not 5 V** |
| GND | `GND` | yes |
| **TX** (sensor data out) | **`GPIO16`** (UART2 RX) | yes — this carries the data |
| RX (sensor data in) | `GPIO17` (UART2 TX) | no — unused in continuous mode |

- Use a **hardware UART (UART2)**. Do **not** use UART0 — that's the USB/serial
  console.
- ⚠️ **Vendor cable colors vary.** If you get no data, swap the two signal wires
  — sensor-TX is probably on GPIO17 instead of GPIO16. (The `sensortest` build,
  below, will tell you.)

---

## 4. Sensor protocol (A02YYUW, default continuous mode)

The sensor streams a 4-byte frame ~10 Hz at 9600 8N1:

```
Byte 0: 0xFF      header
Byte 1: DATA_H    distance high byte (mm)
Byte 2: DATA_L    distance low byte (mm)
Byte 3: SUM       checksum

distance_mm = (DATA_H << 8) | DATA_L
checksum OK if SUM == ((0xFF + DATA_H + DATA_L) & 0xFF)
```

The parser (`src/sensor.cpp`) resyncs on `0xFF`, validates the checksum, rejects
anything outside `MIN_VALID_MM..MAX_VALID_MM` (30–4500 mm), and feeds valid
samples into a sliding **median filter** (default window 5). The median is what
gets published — it rejects the stray spikes/zeros from condensation, no-echo,
and momentary beam-edge hits without lag-laden smoothing.

`level_mm = SENSOR_OFFSET_MM - distance_mm`, where `SENSOR_OFFSET_MM` is the
distance from the sensor face to your chosen zero datum (calibrate once against a
known waterline). Both raw distance and computed level are published.

---

## 5. Firmware architecture

PlatformIO project, Arduino-ESP32 framework. Small, single-responsibility modules
coordinated by a non-blocking main loop.

```
src/
  main.cpp       setup() + loop(): orchestration, watchdog
  sensor.{h,cpp} UART2 frame parser + median filter
  metrics.{h,cpp}WiFi + NTP + OTLP push to Grafana Cloud
  settings.{h,cpp}NVS-backed secrets + serial provisioning console
  ota.{h,cpp}    pull-OTA from public GitHub releases
  sensor_test.cpp standalone bring-up firmware (separate build env)
include/
  config.h       compile-time tunables (pins, intervals, versions) — no secrets
```

### Main loop (non-blocking)
```
loop():
  feed watchdog
  poll serial for a `config` command (re-provisioning)
  drain UART + update median filter
  every PUBLISH_INTERVAL: push metrics
  on boot + hourly (if WiFi up): check GitHub for a newer release
  drive status LED (solid = WiFi up)
```

The only blocking operations are the HTTPS POSTs (push ~1–3 s, OTA download
~10–30 s); the watchdog timeout (60 s) is sized to ride through them, and the OTA
path feeds the watchdog during the download.

### Two kinds of configuration — a deliberate split
- **Compile-time, non-secret → `include/config.h`** (overridable via `-D` in
  `platformio.ini`): UART pins, validation range, median window,
  `SENSOR_OFFSET_MM`, push interval, watchdog timeout, `FIRMWARE_VERSION`, and
  the OTA repo coordinates.
- **Runtime secrets → NVS flash** (WiFi SSID/password, Grafana URL/instance/token):
  provisioned once over a serial console, stored in NVS, **never compiled into the
  binary**. See §6 and §7 for why this matters.

---

## 6. Networking: direct to Grafana Cloud

Metrics are pushed as **OTLP HTTP/JSON** straight to the Grafana Cloud OTLP
endpoint (which lands in Mimir/Prometheus), authenticated with the instance ID +
an access-policy token via HTTP basic auth. `WiFiClientSecure` with
`setInsecure()` (no cert pinning — adequate for this outbound hop), NTP for the
OTLP nanosecond timestamps.

**Why no MQTT / Home Assistant?** The original plan routed through an MQTT broker
and Home Assistant. Dropping both removes two always-on dependencies and a whole
class of "is the broker up?" failure modes. One device → one HTTPS POST → Grafana.
Simpler to reason about, fewer moving parts, and it mirrors an existing LoRa node
that already pushed to the same Grafana stack.

Each push is one JSON document carrying gauges (`pond_distance_mm`,
`pond_level_mm`, `pond_sample_age_seconds`, `pond_wifi_rssi_dbm`,
`pond_uptime_seconds`, `pond_build_info{version}`) and cumulative counters
(`pond_sensor_frames_total`, `pond_sensor_bad_frames_total`). Resource attributes
`service.name` / `host.name` become labels.

---

## 7. OTA: pull-based, from public GitHub releases

This is the most opinionated part of the design, driven by a real constraint.

### The constraint
The node lives on an **isolated IoT VLAN**. The firewall allows the workstation →
device, but **blocks device → workstation (no connect-back)** — standard IoT
hygiene. Push-style OTA (ArduinoOTA / `espota`) is fundamentally incompatible
with this: after authentication, the *device* must open a TCP stream **back** to
the uploader to receive the firmware. That connect-back is exactly what the VLAN
drops, so the transfer always stalls.

### The solution: invert the direction
The device **pulls**. On boot and hourly it does an outbound HTTPS GET to
`api.github.com/repos/<owner>/<repo>/releases/latest`, compares the release tag to
its compiled-in `FIRMWARE_VERSION`, and if they differ, downloads the
`firmware.bin` asset and self-flashes (`HTTPUpdate`), rebooting into the new image.
Every connection is **device-initiated outbound** — the same direction as the
Grafana push — so it sails through the firewall with no exceptions.

```
boot/hourly → GET releases/latest (api.github.com)
            → tag != FIRMWARE_VERSION ?
                 → resolve asset redirect (github.com → signed objects URL)
                 → HTTPUpdate.update(signed URL)   # downloads + flashes
                 → reboot into new firmware
```

### Why secrets had to leave the binary
For the device to pull from a **public** GitHub release without an embedded
credential, the release `firmware.bin` must be safe to publish. But a naive build
compiles WiFi password + Grafana token into the image (`strings firmware.bin`
would reveal them). So secrets were moved out of the binary entirely into **NVS**,
provisioned once over serial. Now the firmware image contains no secrets and can
live in a public release. (The alternative — a private repo + an embedded
read-only GitHub token — also works and is less effort, but keeps secrets in the
binary; we chose the cleaner NVS path.)

### Release workflow (no USB after the first flash)
1. Bump `-DFIRMWARE_VERSION` in `platformio.ini`, `pio run`.
2. `gh release create vX.Y.Z .pio/build/esp32doit/firmware.bin`.
3. Node picks it up within the hour (or immediately on a power cycle).

> Two gotchas:
> - The published binary must itself carry the new `FIRMWARE_VERSION` (it's
>   compiled in) or nodes will re-flash the same version forever.
> - GitHub needs a short window after release creation before the asset's signed
>   download URL is live — an *immediate* power-cycle check can race it and 404.

### Bootstrapping
There's a one-time chicken-and-egg: the **first** image with the correct
`OTA_GITHUB_OWNER` must be flashed over USB. After that, OTA is self-sustaining.

---

## 8. Reliability

- **Watchdog** — `esp_task_wdt`, 60 s, reboots on a hang. Note: the Arduino core
  already initializes the TWDT, so the firmware **reconfigures** it (rather than
  `init()`-ing again, which logs an error and leaves the timeout at the core's
  short default).
- **WiFi** — auto-reconnect; the push path reconnects on demand and returns fast
  on failure rather than stalling the loop.
- **Bad-read rejection** — range + checksum gate, then median filter; a garbage
  value is never published (skip / hold last-good). `pond_sample_age_seconds`
  exposes staleness so you can alert if reads stop.

---

## 9. Build, flash, provision, calibrate

```bash
# 1. Bring-up test (no WiFi) — confirm wiring/baud before anything else
pio run -e sensortest -t upload
pio device monitor              # watch live distance frames

# 2. Real firmware — set OTA_GITHUB_OWNER in platformio.ini first
pio run -e esp32doit -t upload  # first flash is over USB
pio device monitor

# 3. Provision over serial (type `config`, then):
#    ssid <...> / pass <...> / url <grafana otlp url>
#    id <instance id> / token <access policy token> / save / exit

# 4. Calibrate: note `distance` at a known waterline, set SENSOR_OFFSET_MM
```

### Local build-environment note
This project uses the pioarduino `espressif32` platform, which needs extra Python
packages in PlatformIO's environment. If a build fails with
`No module named 'fatfs'` / `yaml` / etc., install them into pio's interpreter:

```bash
uv pip install --python <pio-python> \
  littlefs-python fatfs-ng pyyaml rich-click zopfli intelhex rich 'urllib3<2' \
  cryptography certifi ecdsa bitstring 'reedsolo<1.8' esp-idf-size esp-coredump
```

---

## 10. Known limitations / future hooks

- **No temperature compensation.** Ultrasonic speed-of-sound drifts ~0.17 %/°C and
  the A02YYUW doesn't correct for it — expect ~cm-level drift over outdoor
  temperature swings. A temperature input + correction is a clean future add.
- **Blind zone (3 cm).** Mount with headroom (see §2).
- **Condensation / no-echo** produce occasional bad/zero reads — absorbed by the
  range+checksum gate and the median filter.
- **No low-power mode** — mains-powered by design; the firmware never sleeps.
