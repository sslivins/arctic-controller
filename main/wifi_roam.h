/*
 * Arctic Heat Pump Controller
 * WiFi roaming (see wifi_roam_policy.h). Driven by the WiFi supervisor task.
 */
#pragma once

#include <stdint.h>

// One supervisor tick while associated: samples RSSI and roams when the
// policy says so. Blocks for the scan (~2 s) and any roam (up to ~25 s).
void wifi_roam_poll(void);

// Roams completed this boot.
uint32_t wifi_roam_count(void);

// True while a roam check (scan and any move) is running. Its disconnects are
// deliberate, so they mustn't count toward "WiFi unstable".
bool wifi_roam_in_progress(void);

// Event payload for EVENT_WIFI_ROAMED: RSSI before/after (as positive dB)
// and the new channel.
static inline uint32_t wifi_roam_pack(int from_rssi, int to_rssi, uint8_t channel)
{
    return (uint32_t)(uint8_t)(-from_rssi) | ((uint32_t)(uint8_t)(-to_rssi) << 8) |
           ((uint32_t)channel << 16);
}
static inline int wifi_roam_from_rssi(uint32_t p) { return -(int)(p & 0xFF); }
static inline int wifi_roam_to_rssi(uint32_t p) { return -(int)((p >> 8) & 0xFF); }
static inline unsigned wifi_roam_channel(uint32_t p) { return (unsigned)((p >> 16) & 0xFF); }

// Test firmware only (CONFIG_TEST_ENDPOINTS): scan now and roam. `any` moves
// to the strongest other AP even if it isn't better; `bogus` pins a
// non-existent BSSID to exercise the fallback; `dry` scans and picks without
// moving. Returns a short result string.
const char* wifi_roam_test_run(bool any, bool bogus, bool dry);
