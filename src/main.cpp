// Pond water-level node — ESP32 DOIT DevKit V1.
// Continuously reads the A02YYUW ultrasonic sensor on UART2, median-filters the
// distance, and pushes distance + computed level to Grafana Cloud (OTLP).
// Credentials live in NVS (provisioned over the serial `config` console), so no
// secrets are compiled in. Reflashes itself from public GitHub releases
// (pull-OTA) and self-reboots on hang (task watchdog).

#include <Arduino.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "settings.h"
#include "sensor.h"
#include "metrics.h"
#include "ota.h"

static const int LED_PIN = 2;   // onboard LED on the DOIT DevKit V1

static void wdt_begin() {
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_task_wdt_config_t cfg = {
        .timeout_ms = WDT_TIMEOUT_MS,
        .idle_core_mask = 0,        // watch only tasks we explicitly add
        .trigger_panic = true,      // panic -> reboot on timeout
    };
    // The Arduino core already initializes the TWDT, so reconfigure() applies
    // our WDT_TIMEOUT_MS cleanly. Fall back to init() only if it somehow
    // isn't initialized yet (avoids the core's "already initialized" error
    // that calling init() first would log).
    if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) {
        esp_task_wdt_init(&cfg);
    }
#else
    esp_task_wdt_init(WDT_TIMEOUT_MS / 1000, true);
#endif
    esp_task_wdt_add(NULL);         // supervise the Arduino loop task
}

void setup() {
    Serial.begin(115200);
    uint32_t deadline = millis() + 2000;
    while (!Serial && (int32_t)(deadline - millis()) > 0) { delay(10); }

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    Serial.printf("\n[boot] pond-level firmware v%s\n", FIRMWARE_VERSION);

    settings::begin();
    sensor::begin();
    Serial.printf("[boot] sensor UART%d on RX=%d TX=%d @ %d baud\n",
                  SENSOR_UART_NUM, SENSOR_RX_PIN, SENSOR_TX_PIN, SENSOR_BAUD);

    if (!settings::provisioned()) {
        Serial.println("[boot] no credentials in NVS — entering provisioning console");
        settings::runConsole();
    }
    Serial.println("[boot] (type 'config' + Enter anytime to reconfigure)");

    if (!metrics::begin()) Serial.println("[boot] WiFi init FAILED — will retry on next push");
    wdt_begin();

    Serial.println("[boot] ready");
}

void loop() {
    esp_task_wdt_reset();
    settings::pollSerial();         // 'config' to re-provision
    sensor::poll();

    uint32_t now = millis();
    if (metrics::push_due(now)) {
        metrics::push();
    }
    // Only check for OTA once WiFi is up (otherwise check_due stays armed and
    // fires the moment we connect, rather than burning the interval offline).
    if (metrics::wifi_ok() && ota::check_due(now)) {
        ota::checkAndUpdate();
    }

    // Solid = WiFi up, off = down. Cheap at-a-glance status on the onboard LED.
    digitalWrite(LED_PIN, metrics::wifi_ok() ? HIGH : LOW);
}
