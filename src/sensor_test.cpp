// Standalone sensor bring-up / debug firmware. Self-contained (no WiFi, no
// Grafana) — reads UART2 directly and prints every A02YYUW frame live so you
// can confirm wiring, baud, and readings before flashing the real firmware.
//
// Build & flash:   pio run -e sensortest -t upload
// Watch output:    pio device monitor          (115200 baud)
//
// Per valid frame it prints raw bytes, parsed distance, checksum + range
// status, and a rolling 5-sample median. If nothing arrives it nags you to
// check wiring (most often: swap the two signal wires).

#include <Arduino.h>
#include "config.h"   // SENSOR_* and MIN/MAX_VALID_MM defaults

static HardwareSerial s_uart(SENSOR_UART_NUM);

// 4-byte frame assembly: [0]=0xFF [1]=DATA_H [2]=DATA_L [3]=SUM
static uint8_t  s_buf[4];
static uint8_t  s_idx = 0;

// Rolling median over the last 5 valid samples.
static int32_t  s_win[5];
static uint8_t  s_count = 0, s_head = 0;

static uint32_t s_frames_ok = 0, s_frames_bad = 0;
static uint32_t s_last_byte_ms = 0;
static uint32_t s_last_nag_ms  = 0;

static int32_t median5() {
    int32_t t[5];
    uint8_t n = s_count;
    for (uint8_t i = 0; i < n; i++) t[i] = s_win[i];
    for (uint8_t i = 1; i < n; i++) {       // insertion sort
        int32_t k = t[i]; int8_t j = i - 1;
        while (j >= 0 && t[j] > k) { t[j + 1] = t[j]; j--; }
        t[j + 1] = k;
    }
    return t[n / 2];
}

static void handle_frame() {
    uint8_t sum = (0xFF + s_buf[1] + s_buf[2]) & 0xFF;
    bool csum_ok = (sum == s_buf[3]);
    int32_t mm   = ((int32_t)s_buf[1] << 8) | s_buf[2];
    bool in_range = (mm >= MIN_VALID_MM && mm <= MAX_VALID_MM);

    if (csum_ok && in_range) {
        s_win[s_head] = mm;
        s_head = (s_head + 1) % 5;
        if (s_count < 5) s_count++;
        s_frames_ok++;
    } else {
        s_frames_bad++;
    }

    Serial.printf("raw=%02X %02X %02X %02X  dist=%4ld mm  csum=%s  range=%s",
                  s_buf[0], s_buf[1], s_buf[2], s_buf[3],
                  (long)mm, csum_ok ? "OK " : "BAD",
                  in_range ? "OK " : "OUT");
    if (s_count > 0) Serial.printf("  median5=%4ld mm", (long)median5());
    Serial.printf("  [ok=%lu bad=%lu]\n",
                  (unsigned long)s_frames_ok, (unsigned long)s_frames_bad);
}

void setup() {
    Serial.begin(115200);
    uint32_t deadline = millis() + 2000;
    while (!Serial && (int32_t)(deadline - millis()) > 0) { delay(10); }

    s_uart.begin(SENSOR_BAUD, SERIAL_8N1, SENSOR_RX_PIN, SENSOR_TX_PIN);

    Serial.println();
    Serial.println("=== A02YYUW sensor test ===");
    Serial.printf("UART%d  RX=GPIO%d (<- sensor TX)  TX=GPIO%d  @ %d 8N1\n",
                  SENSOR_UART_NUM, SENSOR_RX_PIN, SENSOR_TX_PIN, SENSOR_BAUD);
    Serial.printf("valid range %d..%d mm\n", MIN_VALID_MM, MAX_VALID_MM);
    Serial.println("Wave a hand / target under the sensor to see the distance change.");
    Serial.println();
    s_last_byte_ms = millis();
}

void loop() {
    while (s_uart.available()) {
        s_last_byte_ms = millis();
        uint8_t b = (uint8_t)s_uart.read();
        if (s_idx == 0) {
            if (b == 0xFF) { s_buf[0] = b; s_idx = 1; }   // resync on header
            continue;
        }
        s_buf[s_idx++] = b;
        if (s_idx == 4) { handle_frame(); s_idx = 0; }
    }

    // No bytes for >1 s? Nag every 2 s with the usual culprits.
    uint32_t now = millis();
    if (now - s_last_byte_ms > 1000 && now - s_last_nag_ms > 2000) {
        s_last_nag_ms = now;
        Serial.printf("[no data %lus] check: sensor VCC->3V3, GND->GND, "
                      "sensor TX->GPIO%d. If still nothing, SWAP the two signal "
                      "wires (vendor TX/RX labels vary).\n",
                      (unsigned long)((now - s_last_byte_ms) / 1000), SENSOR_RX_PIN);
    }
}
