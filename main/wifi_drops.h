/*
 * Arctic Heat Pump Controller
 * WiFi drop events - feeds WifiDropTracker (wifi_drop_tracker.h) from the
 * WiFi event handlers and writes its events from the supervisor task.
 *
 * The note_* calls are cheap and safe from the esp_event task. poll() writes
 * flash, so it must run on a task with a normal stack (the WiFi supervisor).
 */
#pragma once

#include <stdint.h>
#include "sdkconfig.h"
#include "wifi_drop_tracker.h"

void wifi_drops_note_down(uint16_t reason);
void wifi_drops_note_up(void);
// The next disconnect was requested by the controller, not a drop.
void wifi_drops_note_local_disconnect(void);
void wifi_drops_poll(void);
WifiDropStats wifi_drops_stats(void);

#if CONFIG_TEST_ENDPOINTS
// Test firmware: treat the next disconnect as an unplanned drop with `reason`,
// even though the controller makes it.
void wifi_drops_test_unplanned_next(uint16_t reason);
#endif
