#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include <esp_attr.h>
#include <time.h>
#include <stdarg.h>

#include "logger.h"
#include "config.h"
#include "settings.h"

namespace logger {

// --- RTC ring (survives watchdog/panic/SW reset; lost on power loss) -------
static const uint32_t RTC_MAGIC = 0x504F4E44;  // "POND"

struct RtcRing {
    uint32_t magic;
    uint16_t head;                          // next write slot
    uint16_t count;                         // valid entries (<= LOG_CRASH_RING)
    uint32_t epoch[LOG_CRASH_RING];
    char     line[LOG_CRASH_RING][LOG_LINE_MAX];
};
RTC_NOINIT_ATTR static RtcRing s_rtc;

// --- Live batch (INFO+; shipped each flush) --------------------------------
static char     s_live[LOG_BATCH_MAX][LOG_LINE_MAX];
static uint32_t s_liveEpoch[LOG_BATCH_MAX];
static uint16_t s_liveHead = 0, s_liveCount = 0;

// --- Crash dump staged at boot from the prior session's RTC ring -----------
static char       s_crash[LOG_CRASH_RING][LOG_LINE_MAX];
static uint32_t   s_crashEpoch[LOG_CRASH_RING];
static uint16_t   s_crashCount = 0;
static const char* s_crashReason = "";

static char level_char(Level l) {
    switch (l) { case L_ERROR: return 'E'; case L_WARN: return 'W';
                 case L_INFO: return 'I'; default: return 'D'; }
}

static const char* abnormal_reason(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_INT_WDT:  return "INT_WDT";
        case ESP_RST_WDT:      return "WDT";
        case ESP_RST_PANIC:    return "PANIC";
        default:               return nullptr;   // not a crash we dump
    }
}

void begin() {
    const char* reason = abnormal_reason(esp_reset_reason());
    if (s_rtc.magic == RTC_MAGIC && reason && s_rtc.count > 0) {
        // Snapshot the prior session's ring (oldest -> newest) to ship as a
        // crash dump on the next flush().
        uint16_t n = s_rtc.count;
        for (uint16_t i = 0; i < n; i++) {
            uint16_t idx = (s_rtc.head + LOG_CRASH_RING - n + i) % LOG_CRASH_RING;
            strncpy(s_crash[i], s_rtc.line[idx], LOG_LINE_MAX);
            s_crash[i][LOG_LINE_MAX - 1] = '\0';
            s_crashEpoch[i] = s_rtc.epoch[idx];
        }
        s_crashCount = n;
        s_crashReason = reason;
    }
    // (Re)initialize the ring for this session.
    s_rtc.magic = RTC_MAGIC;
    s_rtc.head = 0;
    s_rtc.count = 0;
}

void log(Level lvl, const char* fmt, ...) {
    char buf[LOG_LINE_MAX];
    int pre = snprintf(buf, sizeof(buf), "%c ", level_char(lvl));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + pre, sizeof(buf) - pre, fmt, ap);
    va_end(ap);

    Serial.println(buf);

    uint32_t ep = (uint32_t)time(nullptr);

    // Always record into the RTC ring (verbose breadcrumb for crash dumps).
    strncpy(s_rtc.line[s_rtc.head], buf, LOG_LINE_MAX);
    s_rtc.line[s_rtc.head][LOG_LINE_MAX - 1] = '\0';
    s_rtc.epoch[s_rtc.head] = ep;
    s_rtc.head = (s_rtc.head + 1) % LOG_CRASH_RING;
    if (s_rtc.count < LOG_CRASH_RING) s_rtc.count++;

    // Ship INFO+ live (lifecycle + warnings/errors). DEBUG stays ring-only.
    if (lvl >= L_INFO) {
        strncpy(s_live[s_liveHead], buf, LOG_LINE_MAX);
        s_live[s_liveHead][LOG_LINE_MAX - 1] = '\0';
        s_liveEpoch[s_liveHead] = ep;
        s_liveHead = (s_liveHead + 1) % LOG_BATCH_MAX;
        if (s_liveCount < LOG_BATCH_MAX) s_liveCount++;
    }
}

// Append a Loki stream object with the given labels and (epoch,line) arrays,
// read oldest->newest. `ringHead`/`ringCount` describe a ring; pass head=count
// for a plain 0-based array.
static void add_stream(JsonArray streams, const char* streamLabel,
                       const char* reasonLabel,
                       char lines[][LOG_LINE_MAX], const uint32_t* epochs,
                       uint16_t head, uint16_t count, uint16_t cap,
                       uint32_t fallback_epoch) {
    if (count == 0) return;
    JsonObject s = streams.add<JsonObject>();
    JsonObject lbl = s["stream"].to<JsonObject>();
    lbl["service_name"] = METRICS_SERVICE_NAME;
    lbl["host_name"]    = METRICS_HOST_NAME;
    lbl["source"]       = streamLabel;
    if (reasonLabel) lbl["reason"] = reasonLabel;
    JsonArray vals = s["values"].to<JsonArray>();
    for (uint16_t i = 0; i < count; i++) {
        uint16_t idx = (head + cap - count + i) % cap;
        uint32_t ep = epochs[idx];
        if (ep < 1700000000UL) ep = fallback_epoch;     // pre-NTP line
        unsigned long long ns = (unsigned long long)ep * 1000000000ULL + i;  // +i keeps order/uniqueness
        char ts[24];
        snprintf(ts, sizeof(ts), "%llu", ns);
        JsonArray v = vals.add<JsonArray>();
        v.add(ts);
        v.add(lines[idx]);
    }
}

void flush() {
    if (WiFi.status() != WL_CONNECTED) return;
    uint32_t now = (uint32_t)time(nullptr);
    if (now < 1700000000UL) return;                      // need real time for Loki
    if (s_crashCount == 0 && s_liveCount == 0) return;

    JsonDocument doc;
    JsonArray streams = doc["streams"].to<JsonArray>();
    // Crash dump is a plain 0-based array (head == count, cap == count).
    add_stream(streams, "crash", s_crashReason, s_crash, s_crashEpoch,
               s_crashCount, s_crashCount, s_crashCount ? s_crashCount : 1, now);
    add_stream(streams, "firmware", nullptr, s_live, s_liveEpoch,
               s_liveHead, s_liveCount, LOG_BATCH_MAX, now);

    String body;
    body.reserve(2048);
    serializeJson(doc, body);

    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(10);   // bound TLS handshake; < 60s watchdog
    HTTPClient http;
    http.setTimeout(8000);
    if (!http.begin(client, LOKI_PUSH_URL)) return;
    http.addHeader("Content-Type", "application/json");
    http.setAuthorization(LOKI_USER, settings::get().graf_tok.c_str());

    int code = http.POST(body);
    if (code >= 200 && code < 300) {
        s_crashCount = 0;                 // shipped once
        s_liveHead = 0; s_liveCount = 0;
        Serial.printf("[loki] push ok (HTTP %d, %u bytes)\n", code, (unsigned)body.length());
    } else {
        // Keep live ring (bounded) for retry; log to Serial only (avoid feedback).
        String resp = http.getString();
        Serial.printf("[loki] push FAILED HTTP %d: %s\n", code, resp.substring(0, 120).c_str());
    }
    http.end();
}

}  // namespace logger
