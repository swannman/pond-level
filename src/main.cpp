// Pond water-level node — ESP32 DOIT DevKit V1.
// Continuously reads the A02YYUW ultrasonic sensor on UART2, median-filters the
// distance, and pushes distance + computed level to Grafana Cloud (OTLP). Stays
// reflashable over WiFi (ArduinoOTA) and self-reboots on hang (task watchdog).

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "secrets.h"
#include "sensor.h"
#include "metrics.h"

static const int LED_PIN = 2;   // onboard LED on the DOIT DevKit V1

static void wdt_begin() {
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_task_wdt_config_t cfg = {
        .timeout_ms = WDT_TIMEOUT_MS,
        .idle_core_mask = 0,        // watch only tasks we explicitly add
        .trigger_panic = true,      // panic -> reboot on timeout
    };
    esp_task_wdt_init(&cfg);
#else
    esp_task_wdt_init(WDT_TIMEOUT_MS / 1000, true);
#endif
    esp_task_wdt_add(NULL);         // supervise the Arduino loop task
}

static void ota_begin() {
    ArduinoOTA.setHostname(METRICS_HOST_NAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    // A flash write can outlast the watchdog window — pet it during upload.
    ArduinoOTA.onStart([]() { Serial.println("[ota] update starting"); });
    ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
        esp_task_wdt_reset();
        Serial.printf("[ota] %u%%\r", (t ? p * 100 / t : 0));
    });
    ArduinoOTA.onEnd([]()   { Serial.println("\n[ota] update complete"); });
    ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[ota] error %u\n", e); });
    ArduinoOTA.begin();
}

void setup() {
    Serial.begin(115200);
    uint32_t deadline = millis() + 2000;
    while (!Serial && (int32_t)(deadline - millis()) > 0) { delay(10); }

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    sensor::begin();
    Serial.printf("[boot] sensor UART%d on RX=%d TX=%d @ %d baud\n",
                  SENSOR_UART_NUM, SENSOR_RX_PIN, SENSOR_TX_PIN, SENSOR_BAUD);

    if (!metrics::begin()) Serial.println("[boot] WiFi init FAILED — will retry on next push");
    ota_begin();
    wdt_begin();

    Serial.println("[boot] ready");
}

void loop() {
    esp_task_wdt_reset();
    ArduinoOTA.handle();
    sensor::poll();

    uint32_t now = millis();
    if (metrics::push_due(now)) {
        metrics::push();
    }

    // Solid = WiFi up, off = down. Cheap at-a-glance status on the onboard LED.
    digitalWrite(LED_PIN, metrics::wifi_ok() ? HIGH : LOW);
}
