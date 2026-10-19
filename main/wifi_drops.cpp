/*
 * Arctic Heat Pump Controller
 * WiFi drop events (see wifi_drops.h)
 */
#include "wifi_drops.h"
#include "event_log.h"

#include <freertos/FreeRTOS.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi_types.h>

static const char* TAG = "wifi_drops";

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static WifiDropTracker s_tracker;
static bool s_local_pending = false;
static uint16_t s_test_reason = 0;

static int64_t now_ms() { return esp_timer_get_time() / 1000; }

void wifi_drops_note_down(uint16_t reason)
{
    portENTER_CRITICAL(&s_mux);
    bool local = s_local_pending;
    if (s_test_reason != 0) {
        reason = s_test_reason;
        local = false;
        s_test_reason = 0;
    }
    s_local_pending = false;
    s_tracker.on_down(now_ms(), reason, local);
    portEXIT_CRITICAL(&s_mux);
}

#if CONFIG_TEST_ENDPOINTS
void wifi_drops_test_unplanned_next(uint16_t reason)
{
    portENTER_CRITICAL(&s_mux);
    s_test_reason = reason ? reason : WIFI_REASON_BEACON_TIMEOUT;
    portEXIT_CRITICAL(&s_mux);
}
#endif

void wifi_drops_note_up(void)
{
    portENTER_CRITICAL(&s_mux);
    s_local_pending = false;
    s_tracker.on_up(now_ms());
    portEXIT_CRITICAL(&s_mux);
}

void wifi_drops_note_local_disconnect(void)
{
    portENTER_CRITICAL(&s_mux);
    s_local_pending = true;
    portEXIT_CRITICAL(&s_mux);
}

void wifi_drops_poll(void)
{
    WifiDropEvent ev[WifiDropTracker::MAX_PENDING];
    portENTER_CRITICAL(&s_mux);
    const int n = s_tracker.poll(now_ms(), ev, WifiDropTracker::MAX_PENDING);
    portEXIT_CRITICAL(&s_mux);

    for (int i = 0; i < n; i++) {
        switch (ev[i].action) {
            case WifiDropAction::LOST:
                ESP_LOGW(TAG, "WiFi down %llds (reason %lu, %s) - logging wifi_lost",
                         (long long)(WifiDropTracker::LONG_MS / 1000),
                         (unsigned long)ev[i].payload,
                         event_wifi_reason_key(ev[i].payload));
                event_log_record(EVENT_WIFI_LOST, ev[i].payload);
                break;
            case WifiDropAction::RESTORED:
                ESP_LOGW(TAG, "WiFi restored after %lus", (unsigned long)ev[i].payload);
                event_log_record(EVENT_WIFI_RESTORED, ev[i].payload);
                break;
            case WifiDropAction::UNSTABLE:
                ESP_LOGW(TAG, "%lu short WiFi drops within an hour - logging wifi_unstable",
                         (unsigned long)ev[i].payload);
                event_log_record(EVENT_WIFI_UNSTABLE, ev[i].payload);
                break;
        }
    }
}

WifiDropStats wifi_drops_stats(void)
{
    portENTER_CRITICAL(&s_mux);
    const WifiDropStats s = s_tracker.stats(now_ms());
    portEXIT_CRITICAL(&s_mux);
    return s;
}
