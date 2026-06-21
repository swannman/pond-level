// Lightweight logger that tees to Serial and ships to Grafana Cloud Loki.
//
// - INFO/WARN/ERROR are batched and pushed to Loki each flush (lifecycle +
//   problems). DEBUG is verbose and is NOT shipped live.
// - Every line (including DEBUG) is also written to a small ring buffer in RTC
//   memory, which survives watchdog/panic/SW resets (but not power loss). On
//   the next boot after an abnormal reset, that ring is shipped to Loki as a
//   crash dump — the breadcrumb of what ran just before the hang/crash.
#pragma once

#include <Arduino.h>

namespace logger {

enum Level { L_DEBUG = 0, L_INFO = 1, L_WARN = 2, L_ERROR = 3 };

// Init the RTC ring; if the last reset was abnormal, stage its contents to be
// shipped as a crash dump on the next flush(). Call early in setup().
void begin();

// printf-style log at a level. Always mirrors to Serial + RTC ring; INFO+ also
// queues for the next Loki flush.
void log(Level lvl, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Push staged crash dump (if any) + queued live lines to Loki. Best-effort:
// returns fast if WiFi/NTP aren't ready. Call on the metrics push interval.
void flush();

}  // namespace logger

#define LOGD(...) logger::log(logger::L_DEBUG, __VA_ARGS__)
#define LOGI(...) logger::log(logger::L_INFO,  __VA_ARGS__)
#define LOGW(...) logger::log(logger::L_WARN,  __VA_ARGS__)
#define LOGE(...) logger::log(logger::L_ERROR, __VA_ARGS__)
