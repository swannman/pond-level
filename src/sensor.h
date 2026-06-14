// A02YYUW ultrasonic distance sensor: UART frame parser + median filter.
// Call poll() every loop iteration to drain UART2; it resyncs on the 0xFF
// header, validates the checksum and range, and feeds a sliding median window.
// distance_mm() returns the median of the last MEDIAN_WINDOW valid samples,
// which rejects the stray spikes/zeros caused by condensation and no-echo.
#pragma once

#include <stdint.h>

namespace sensor {

// Open UART2 at SENSOR_BAUD on the configured pins.
void begin();

// Drain any pending UART bytes and update the filter. Non-blocking.
void poll();

// True once at least one valid sample has been collected.
bool have_reading();

// Median-filtered distance in mm (valid only if have_reading()).
int32_t distance_mm();

// Milliseconds since the last valid sample (for staleness detection).
uint32_t last_sample_age_ms(uint32_t now_ms);

// Cumulative counters since boot.
uint32_t frames_ok();    // valid frames accepted into the filter
uint32_t frames_bad();   // frames rejected (bad checksum or out of range)

}  // namespace sensor
