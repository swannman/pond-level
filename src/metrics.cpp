#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <esp_system.h>
#include <ArduinoJson.h>

#include "metrics.h"
#include "sensor.h"
#include "config.h"
#include "settings.h"
#include "logger.h"

namespace metrics {

static uint32_t s_last_push_ms = 0;
static bool     s_wifi_ok      = false;
static bool     s_ntp_ok       = false;

bool wifi_ok() { return s_wifi_ok; }

bool push_due(uint32_t now_ms) {
    return (now_ms - s_last_push_ms) >= (uint32_t)METRICS_PUSH_INTERVAL_MS;
}

// --- WiFi + NTP -----------------------------------------------------------
static const char* wifi_status_str(wl_status_t s) {
    switch (s) {
        case WL_IDLE_STATUS:     return "IDLE";
        case WL_NO_SSID_AVAIL:   return "NO_SSID_AVAIL (SSID not visible)";
        case WL_SCAN_COMPLETED:  return "SCAN_COMPLETED";
        case WL_CONNECTED:       return "CONNECTED";
        case WL_CONNECT_FAILED:  return "CONNECT_FAILED (auth — check password)";
        case WL_CONNECTION_LOST: return "CONNECTION_LOST";
        case WL_DISCONNECTED:    return "DISCONNECTED";
        default:                 return "UNKNOWN";
    }
}

// Scan once and confirm whether our SSID is visible — disambiguates
// "wrong password" from "AP out of range / typo".
static bool ssid_visible_in_scan() {
    Serial.println("[wifi] scanning for nearby APs...");
    int n = WiFi.scanNetworks();
    if (n <= 0) {
        Serial.println("[wifi] scan returned 0 networks (radio issue?)");
        return false;
    }
    const String& ssid = settings::get().wifi_ssid;
    bool found = false;
    int found_rssi = 0;
    for (int i = 0; i < n; ++i) {
        if (WiFi.SSID(i) == ssid) {
            found = true;
            found_rssi = WiFi.RSSI(i);
            break;
        }
    }
    if (found) {
        Serial.printf("[wifi] target SSID '%s' visible (rssi=%d dBm)\n",
                      ssid.c_str(), found_rssi);
    } else {
        Serial.printf("[wifi] target SSID '%s' NOT in scan results — first %d seen:\n",
                      ssid.c_str(), n);
        int show = n < 8 ? n : 8;
        for (int i = 0; i < show; ++i) {
            Serial.printf("        '%s' (rssi=%d)\n",
                          WiFi.SSID(i).c_str(), WiFi.RSSI(i));
        }
    }
    WiFi.scanDelete();
    return found;
}

static bool connect_wifi() {
    if (WiFi.status() == WL_CONNECTED) { s_wifi_ok = true; return true; }
    const auto& c = settings::get();
    Serial.printf("[wifi] connecting to '%s'...\n", c.wifi_ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.disconnect(true, true);  // clear previous state so failure code is fresh
    delay(100);
    WiFi.begin(c.wifi_ssid.c_str(), c.wifi_pass.c_str());

    uint32_t deadline = millis() + METRICS_WIFI_CONNECT_TIMEOUT_MS;
    wl_status_t last = WL_IDLE_STATUS;
    while (millis() < deadline) {
        wl_status_t st = WiFi.status();
        if (st == WL_CONNECTED) break;
        if (st == WL_NO_SSID_AVAIL || st == WL_CONNECT_FAILED) { last = st; break; }
        last = st;
        delay(200);
    }

    wl_status_t final_status = WiFi.status();
    s_wifi_ok = (final_status == WL_CONNECTED);

    if (s_wifi_ok) {
        LOGI("wifi connected ip=%s rssi=%d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } else {
        LOGW("wifi connect FAILED status=%s (last %s)",
             wifi_status_str(final_status), wifi_status_str(last));
        ssid_visible_in_scan();
    }
    return s_wifi_ok;
}

static bool sync_ntp() {
    if (s_ntp_ok) return true;
    configTime(0, 0, NTP_SERVER);
    Serial.print("[ntp] syncing");
    uint32_t deadline = millis() + 10000;
    time_t now = 0;
    while ((now = time(nullptr)) < 1700000000 && millis() < deadline) {
        Serial.print(".");
        delay(250);
    }
    Serial.println();
    s_ntp_ok = (now >= 1700000000);
    if (s_ntp_ok) Serial.printf("[ntp] synced (epoch=%ld)\n", (long)now);
    else          Serial.println("[ntp] sync failed");
    return s_ntp_ok;
}

bool begin() {
    if (!connect_wifi()) return false;
    sync_ntp();
    return s_wifi_ok;
}

// --- OTLP push ------------------------------------------------------------
// Decode the cause of the most recent reset so a reboot self-diagnoses in
// Grafana (watchdog hang vs panic vs brownout vs intentional SW/OTA reset).
static const char* reset_reason_str() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "POWERON";    // cold power-up
        case ESP_RST_EXT:       return "EXT";        // external reset pin
        case ESP_RST_SW:        return "SW";         // esp_restart() — e.g. OTA
        case ESP_RST_PANIC:     return "PANIC";      // exception / crash
        case ESP_RST_INT_WDT:   return "INT_WDT";    // interrupt watchdog
        case ESP_RST_TASK_WDT:  return "TASK_WDT";   // our task watchdog (hang)
        case ESP_RST_WDT:       return "WDT";        // other watchdog
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";   // supply voltage sag
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

// One OTLP/HTTP/JSON document, one gauge/sum dataPoint per metric, all sharing
// resource attributes (service.name, host.name) that become labels in Mimir.
static void build_payload(String& out) {
    JsonDocument doc;

    // Timestamp in nanoseconds — OTLP requires a string for int64 to avoid
    // precision loss in JS-style parsers.
    int64_t now_ns = (int64_t)time(nullptr) * 1000000000LL;
    char ts_buf[24];
    snprintf(ts_buf, sizeof(ts_buf), "%lld", (long long)now_ns);

    JsonObject rm = doc["resourceMetrics"].add<JsonObject>();
    JsonObject resource = rm["resource"].to<JsonObject>();
    JsonArray attrs = resource["attributes"].to<JsonArray>();
    auto add_attr = [&](const char* k, const char* v) {
        JsonObject a = attrs.add<JsonObject>();
        a["key"] = k;
        a["value"]["stringValue"] = v;
    };
    add_attr("service.name", METRICS_SERVICE_NAME);
    add_attr("host.name",    METRICS_HOST_NAME);

    JsonObject sm = rm["scopeMetrics"].add<JsonObject>();
    sm["scope"]["name"] = "pond.level";
    JsonArray metrics_arr = sm["metrics"].to<JsonArray>();

    auto add_gauge_double = [&](const char* name, const char* desc, double val) {
        JsonObject m = metrics_arr.add<JsonObject>();
        m["name"] = name;
        m["description"] = desc;
        JsonObject dp = m["gauge"]["dataPoints"].add<JsonObject>();
        dp["asDouble"] = val;
        dp["timeUnixNano"] = ts_buf;
    };
    auto add_sum_int = [&](const char* name, const char* desc, int64_t val) {
        JsonObject m = metrics_arr.add<JsonObject>();
        m["name"] = name;
        m["description"] = desc;
        JsonObject sum = m["sum"].to<JsonObject>();
        sum["aggregationTemporality"] = 2;   // CUMULATIVE
        sum["isMonotonic"] = true;
        JsonObject dp = sum["dataPoints"].add<JsonObject>();
        dp["asInt"] = val;
        dp["timeUnixNano"] = ts_buf;
    };

    // Build-info: value 1, firmware version carried as a label so an OTA shows
    // up in Grafana as the series' `version` flipping to the new release.
    {
        JsonObject m = metrics_arr.add<JsonObject>();
        m["name"] = "pond_build_info";
        m["description"] = "Firmware build info (value 1; version as label)";
        JsonObject dp = m["gauge"]["dataPoints"].add<JsonObject>();
        dp["asDouble"] = 1.0;
        dp["timeUnixNano"] = ts_buf;
        JsonObject va = dp["attributes"].add<JsonObject>();
        va["key"] = "version";
        va["value"]["stringValue"] = FIRMWARE_VERSION;
    }

    // Reset reason of the current boot, carried as a label. After an unexpected
    // reboot this names the cause (TASK_WDT / PANIC / BROWNOUT / ...).
    {
        JsonObject m = metrics_arr.add<JsonObject>();
        m["name"] = "pond_reset_reason";
        m["description"] = "Cause of the last reset (value 1; reason as label)";
        JsonObject dp = m["gauge"]["dataPoints"].add<JsonObject>();
        dp["asDouble"] = 1.0;
        dp["timeUnixNano"] = ts_buf;
        JsonObject va = dp["attributes"].add<JsonObject>();
        va["key"] = "reason";
        va["value"]["stringValue"] = reset_reason_str();
    }

    uint32_t now = millis();
    add_gauge_double("pond_free_heap_bytes", "Free heap in bytes (watch for leaks)",
                     (double)ESP.getFreeHeap());
    if (sensor::have_reading()) {
        int32_t dist = sensor::distance_mm();
        int32_t level = (int32_t)SENSOR_OFFSET_MM - dist;
        add_gauge_double("pond_distance_mm", "Median sensor-to-water distance in mm",          (double)dist);
        add_gauge_double("pond_level_mm",    "Water level above datum in mm (offset - distance)", (double)level);
        double age_s = sensor::last_sample_age_ms(now) / 1000.0;
        add_gauge_double("pond_sample_age_seconds", "Seconds since last valid sensor sample",  age_s);
    }
    add_gauge_double("pond_wifi_rssi_dbm",  "WiFi signal strength in dBm",        (double)WiFi.RSSI());
    add_gauge_double("pond_uptime_seconds", "Seconds since boot",                 (double)(now / 1000UL));
    add_sum_int("pond_sensor_frames_total",     "Cumulative valid sensor frames accepted",            (int64_t)sensor::frames_ok());
    add_sum_int("pond_sensor_bad_frames_total", "Cumulative sensor frames rejected (checksum/range)", (int64_t)sensor::frames_bad());

    serializeJson(doc, out);
}

void push() {
    s_last_push_ms = millis();

    if (!connect_wifi()) return;
    if (!s_ntp_ok && !sync_ntp()) {
        Serial.println("[metrics] skip push — no NTP");
        return;
    }

    String payload;
    payload.reserve(1536);
    build_payload(payload);

    const auto& c = settings::get();
    WiFiClientSecure client;
    client.setInsecure();   // skip cert validation — adequate for this hop
    client.setHandshakeTimeout(10);  // bound TLS handshake; default 120s > 60s watchdog
    HTTPClient http;
    http.setTimeout(8000);
    if (!http.begin(client, c.graf_url)) {
        Serial.println("[metrics] http.begin FAILED");
        return;
    }
    http.addHeader("Content-Type", "application/json");
    http.setAuthorization(c.graf_id.c_str(), c.graf_tok.c_str());

    LOGD("metrics: POST %u B", (unsigned)payload.length());   // breadcrumb before blocking call
    int code = http.POST(payload);
    if (code >= 200 && code < 300) {
        LOGD("metrics push ok %d", code);
    } else {
        String resp = http.getString();
        LOGW("metrics push FAILED HTTP %d: %s", code, resp.substring(0, 120).c_str());
    }
    http.end();
}

}  // namespace metrics
