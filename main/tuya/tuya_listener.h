/*
 * Passive Tuya bus listener for the Arctic heat pump.
 *
 * Splices onto the existing controller <-> heat pump RS485 bus in
 * RECEIVE-ONLY mode and decodes the Tuya MCU framing (55 AA ...) using
 * tuya_codec. It NEVER transmits: the RS485 direction pin is held low so
 * the transceiver's driver is permanently disabled, and the UART TX line
 * is left unrouted. This makes the Tab5 electrically invisible on the bus
 * and safe to run alongside the real controller (which remains the master).
 *
 * Wire settings (validated against the real bus): 4800 baud, 8-E-1.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

namespace tuya {

struct ListenerStats {
    uint32_t bytes_rx;        // total raw bytes received off the bus
    uint32_t frames_ok;       // frames that parsed with a valid checksum
    uint32_t req_frames;      // controller -> heat pump requests (0xF0)
    uint32_t resp_frames;     // heat pump -> controller responses (0x0F)
    uint32_t checksum_err;    // plausible frames that failed checksum
    uint32_t resync;          // resync events (junk / dropped bytes)
    uint32_t last_frame_ms;   // esp_timer millis of the last good frame
};

// Install the raw UART1 RX-only driver (4800 8E1) on the RS485 pins and
// force the direction pin low. Returns ESP_OK on success.
esp_err_t listener_init();

// Spawn the background receive/decode task. Call after listener_init().
void listener_start();

// Thread-safe snapshot of the listener statistics.
ListenerStats listener_get_stats();

}  // namespace tuya
