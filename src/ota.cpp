#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>

#include "ota.h"
#include "config.h"
#include "logger.h"

namespace ota {

static uint32_t s_last_check_ms = 0;
static bool     s_checked_once  = false;

bool check_due(uint32_t now_ms) {
    if (!s_checked_once) return true;                       // check shortly after boot
    return (now_ms - s_last_check_ms) >= (uint32_t)OTA_CHECK_INTERVAL_MS;
}

// Drop a leading 'v' so tag "v1.2.3" matches FIRMWARE_VERSION "1.2.3".
static String normalize(String v) {
    v.trim();
    if (v.startsWith("v") || v.startsWith("V")) v.remove(0, 1);
    return v;
}

// Fetch the latest release's tag and the download URL of OTA_ASSET_NAME.
static bool fetchLatest(String& tag, String& assetUrl) {
    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(10);  // bound TLS handshake; default 120s > 60s watchdog
    HTTPClient http;
    http.setTimeout(8000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    String api = String("https://api.github.com/repos/") +
                 OTA_GITHUB_OWNER + "/" + OTA_GITHUB_REPO + "/releases/latest";
    if (!http.begin(client, api)) return false;
    http.addHeader("User-Agent", "pond-level-ota");          // GitHub requires a UA
    http.addHeader("Accept", "application/vnd.github+json");

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[ota] release check HTTP %d\n", code);
        http.end();
        return false;
    }

    // Stream-parse with a filter so the (large) release JSON never lands in RAM
    // whole. For arrays, element [0] of the filter is the template for all.
    JsonDocument filter;
    filter["tag_name"] = true;
    filter["assets"][0]["name"] = true;
    filter["assets"][0]["browser_download_url"] = true;

    JsonDocument doc;
    DeserializationError err =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (err) {
        Serial.printf("[ota] JSON parse failed: %s\n", err.c_str());
        return false;
    }

    tag = doc["tag_name"].as<String>();
    for (JsonObject a : doc["assets"].as<JsonArray>()) {
        if (String(a["name"].as<const char*>()) == OTA_ASSET_NAME) {
            assetUrl = a["browser_download_url"].as<String>();
            return tag.length() && assetUrl.length();
        }
    }
    Serial.printf("[ota] asset '%s' not in release %s\n", OTA_ASSET_NAME, tag.c_str());
    return false;
}

// Resolve a single redirect hop (github.com -> signed objects URL) so we hand
// httpUpdate a direct 200 URL.
static String resolveRedirect(const String& url) {
    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(10);  // bound TLS handshake; default 120s > 60s watchdog
    HTTPClient http;
    http.setTimeout(8000);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    if (!http.begin(client, url)) return url;
    http.addHeader("User-Agent", "pond-level-ota");
    const char* hdr[] = {"Location"};
    http.collectHeaders(hdr, 1);
    int code = http.GET();
    String out = url;
    if (code == 301 || code == 302 || code == 307 || code == 308) {
        String loc = http.header("Location");
        if (loc.length()) out = loc;
    }
    http.end();
    return out;
}

void checkAndUpdate() {
    s_last_check_ms = millis();
    s_checked_once  = true;
    if (WiFi.status() != WL_CONNECTED) return;

    String tag, assetUrl;
    if (!fetchLatest(tag, assetUrl)) return;

    LOGD("ota: latest=%s current=%s", tag.c_str(), FIRMWARE_VERSION);
    if (normalize(tag) == normalize(FIRMWARE_VERSION)) {
        Serial.println(F("[ota] up to date"));
        return;
    }

    LOGI("ota updating %s -> %s", FIRMWARE_VERSION, tag.c_str());
    String finalUrl = resolveRedirect(assetUrl);

    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(10);  // bound TLS handshake; default 120s > 60s watchdog
    httpUpdate.rebootOnUpdate(true);
    httpUpdate.onProgress([](int cur, int total) {
        esp_task_wdt_reset();                                // long download — keep WDT fed
        static int last = -1;
        int pct = total ? (cur * 100 / total) : 0;
        if (pct != last && pct % 10 == 0) { Serial.printf("[ota] %d%%\n", pct); last = pct; }
    });

    t_httpUpdate_return ret = httpUpdate.update(client, finalUrl);
    switch (ret) {
        case HTTP_UPDATE_FAILED:
            LOGW("ota FAILED (%d): %s", httpUpdate.getLastError(),
                 httpUpdate.getLastErrorString().c_str());
            break;
        case HTTP_UPDATE_NO_UPDATES:
            Serial.println(F("[ota] no update applied"));
            break;
        case HTTP_UPDATE_OK:
            Serial.println(F("[ota] OK — rebooting"));       // usually reboots before this prints
            break;
    }
}

}  // namespace ota
