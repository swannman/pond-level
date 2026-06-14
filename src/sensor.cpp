#include <Arduino.h>

#include "sensor.h"
#include "config.h"

namespace sensor {

// UART2 instance. Constructed with the port number from config.
static HardwareSerial s_uart(SENSOR_UART_NUM);

// --- Frame assembly state machine -----------------------------------------
// Frame: [0]=0xFF  [1]=DATA_H  [2]=DATA_L  [3]=SUM
static uint8_t  s_buf[4];
static uint8_t  s_idx = 0;        // 0 = waiting for header

// --- Sliding median window (ring buffer of last N valid samples) ----------
static int32_t  s_window[MEDIAN_WINDOW];
static uint8_t  s_count = 0;       // valid samples in window (<= MEDIAN_WINDOW)
static uint8_t  s_head  = 0;       // next write position
static uint32_t s_last_ms = 0;     // millis() of last valid sample

static uint32_t s_frames_ok  = 0;
static uint32_t s_frames_bad = 0;

void begin() {
    s_uart.begin(SENSOR_BAUD, SERIAL_8N1, SENSOR_RX_PIN, SENSOR_TX_PIN);
}

// Accept one validated distance into the ring buffer.
static void push_sample(int32_t mm) {
    s_window[s_head] = mm;
    s_head = (s_head + 1) % MEDIAN_WINDOW;
    if (s_count < MEDIAN_WINDOW) s_count++;
    s_last_ms = millis();
    s_frames_ok++;
}

// A completed 4-byte frame is in s_buf — validate and record it.
static void handle_frame() {
    uint8_t sum = (0xFF + s_buf[1] + s_buf[2]) & 0xFF;
    if (sum != s_buf[3]) { s_frames_bad++; return; }   // checksum mismatch

    int32_t mm = ((int32_t)s_buf[1] << 8) | s_buf[2];
    if (mm < MIN_VALID_MM || mm > MAX_VALID_MM) {       // no-echo / blind zone / glitch
        s_frames_bad++;
        return;
    }
    push_sample(mm);
}

void poll() {
    while (s_uart.available()) {
        uint8_t b = (uint8_t)s_uart.read();
        if (s_idx == 0) {
            // Resync: only a 0xFF can start a frame.
            if (b == 0xFF) { s_buf[0] = b; s_idx = 1; }
            continue;
        }
        s_buf[s_idx++] = b;
        if (s_idx == 4) {
            handle_frame();
            s_idx = 0;   // back to hunting for the next header
        }
    }
}

bool have_reading() { return s_count > 0; }

int32_t distance_mm() {
    // Copy the filled portion and sort to find the median. MEDIAN_WINDOW is
    // small (5-9), so an insertion sort is plenty and keeps the loop tight.
    int32_t tmp[MEDIAN_WINDOW];
    uint8_t n = s_count;
    for (uint8_t i = 0; i < n; i++) tmp[i] = s_window[i];
    for (uint8_t i = 1; i < n; i++) {
        int32_t key = tmp[i];
        int8_t j = i - 1;
        while (j >= 0 && tmp[j] > key) { tmp[j + 1] = tmp[j]; j--; }
        tmp[j + 1] = key;
    }
    return tmp[n / 2];
}

uint32_t last_sample_age_ms(uint32_t now_ms) {
    if (s_count == 0) return UINT32_MAX;
    return now_ms - s_last_ms;
}

uint32_t frames_ok()  { return s_frames_ok; }
uint32_t frames_bad() { return s_frames_bad; }

}  // namespace sensor
