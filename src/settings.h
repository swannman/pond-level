// Runtime configuration (WiFi + Grafana credentials) stored in NVS flash, not
// compiled into the firmware. This keeps the binary free of secrets so release
// images can be published publicly for pull-OTA. Provisioned once over the
// serial console (type `config`).
#pragma once

#include <Arduino.h>

namespace settings {

struct Config {
    String wifi_ssid;
    String wifi_pass;
    String graf_url;     // Grafana Cloud OTLP metrics URL
    String graf_id;      // Prometheus instance id (basic-auth user)
    String graf_tok;     // Grafana Cloud access-policy token
};

// Load config from NVS into memory.
void begin();

// True once the minimum required fields are present.
bool provisioned();

// Current config (valid after begin()).
const Config& get();

// Non-blocking: call each loop. If a `config` line arrives on serial, opens
// the blocking provisioning console.
void pollSerial();

// Blocking interactive provisioning console over serial. Feeds the watchdog.
void runConsole();

}  // namespace settings
