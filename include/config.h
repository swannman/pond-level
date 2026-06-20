// Compile-time config for the pond water-level node. Defaults are set as -D
// flags in platformio.ini; override them there rather than editing here.
#pragma once

#include <stdint.h>

// --- A02YYUW ultrasonic sensor (UART) -------------------------------------
// Default continuous mode: streams a 4-byte frame (~10 Hz) at 9600 8N1:
//   [0]=0xFF header  [1]=DATA_H  [2]=DATA_L  [3]=SUM
//   distance_mm = (DATA_H<<8)|DATA_L
//   checksum OK if SUM == ((0xFF + DATA_H + DATA_L) & 0xFF)
#ifndef SENSOR_UART_NUM
#define SENSOR_UART_NUM 2
#endif
#ifndef SENSOR_RX_PIN
#define SENSOR_RX_PIN 16        // ESP32 RX2  <- sensor TX
#endif
#ifndef SENSOR_TX_PIN
#define SENSOR_TX_PIN 17        // ESP32 TX2  -> sensor RX (unused in continuous mode)
#endif
#ifndef SENSOR_BAUD
#define SENSOR_BAUD 9600
#endif

// --- Validation / filtering ------------------------------------------------
#ifndef MIN_VALID_MM
#define MIN_VALID_MM 30         // below 30 mm is inside the 3 cm blind zone
#endif
#ifndef MAX_VALID_MM
#define MAX_VALID_MM 4500       // sensor max range
#endif
#ifndef MEDIAN_WINDOW
#define MEDIAN_WINDOW 5         // odd; median over the last N valid samples
#endif

// --- Level calibration -----------------------------------------------------
// Distance (mm) from the sensor face to the chosen zero datum. Calibrated
// against one measured waterline:  level_mm = SENSOR_OFFSET_MM - distance_mm.
#ifndef SENSOR_OFFSET_MM
#define SENSOR_OFFSET_MM 1000
#endif

// --- Metrics push (Grafana Cloud via OTLP HTTP/JSON) ----------------------
#ifndef METRICS_PUSH_INTERVAL_MS
#define METRICS_PUSH_INTERVAL_MS 30000   // 30 s batch interval
#endif
#ifndef METRICS_WIFI_CONNECT_TIMEOUT_MS
#define METRICS_WIFI_CONNECT_TIMEOUT_MS 20000  // 20 s to connect at boot
#endif
#ifndef METRICS_SERVICE_NAME
#define METRICS_SERVICE_NAME "pond-level"
#endif
#ifndef METRICS_HOST_NAME
#define METRICS_HOST_NAME "esp32-pond"
#endif
// Time pool — used for OTLP timestamps. pool.ntp.org is reliable globally.
#ifndef NTP_SERVER
#define NTP_SERVER "pool.ntp.org"
#endif

// --- Watchdog --------------------------------------------------------------
#ifndef WDT_TIMEOUT_MS
#define WDT_TIMEOUT_MS 60000    // reboot if the loop hangs this long
#endif

// --- Firmware version + pull-OTA (public GitHub release) ------------------
// Bump FIRMWARE_VERSION to match the release tag you publish; the node updates
// whenever the latest release tag differs from this. Owner/repo point at the
// PUBLIC repo that hosts the firmware asset (secrets live in NVS, not the
// binary, so the release image is safe to publish).
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.1.0"
#endif
#ifndef OTA_GITHUB_OWNER
#define OTA_GITHUB_OWNER "YOUR_GH_USER"   // <-- set to your GitHub username/org
#endif
#ifndef OTA_GITHUB_REPO
#define OTA_GITHUB_REPO "pond-level"
#endif
#ifndef OTA_ASSET_NAME
#define OTA_ASSET_NAME "firmware.bin"     // release asset to download
#endif
#ifndef OTA_CHECK_INTERVAL_MS
#define OTA_CHECK_INTERVAL_MS 21600000UL  // 6 h
#endif
