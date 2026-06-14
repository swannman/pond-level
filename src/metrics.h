// WiFi + Grafana Cloud OTLP HTTP/JSON metrics push.
// Reads the current sensor state and batches a single metrics snapshot to
// Grafana Cloud Prometheus on METRICS_PUSH_INTERVAL_MS.
#pragma once

#include <stdint.h>

namespace metrics {

// Connect WiFi (blocking with timeout), sync NTP. Returns true on success.
// Safe to call repeatedly; retries on failure.
bool begin();

// True if the push interval has elapsed since the last push.
bool push_due(uint32_t now_ms);

// Build the OTLP JSON payload and POST it. Returns quickly on WiFi failure,
// otherwise blocks for the HTTPS POST (~1-3 s typical). Logs to Serial.
void push();

// True once WiFi is connected.
bool wifi_ok();

}  // namespace metrics
