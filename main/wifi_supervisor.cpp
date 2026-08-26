/*
 * Arctic Heat Pump Controller
 * WiFi health supervisor - implementation
 */
#include "wifi_supervisor.h"
#include "wifi_manager.h"
#include "system_restart.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_system.h>
#include <string.h>

static const char* TAG = "wifi_super";

// Cadence / thresholds. Chosen so the supervisor is invisible during normal
// operation and gentle during an outage (the local touchscreen keeps working
// without WiFi), escalating to a reboot only as a genuine last resort.
static const int CHECK_INTERVAL_MS  = 15000;    // poll cadence
static const int GRACE_MS           = 60000;    // tolerate this long before acting
static const int RECONNECT_EVERY_MS = 30000;    // in-place reconnect cadence
static const int REBOOT_AFTER_MS    = 900000;   // 15 min continuously down -> reboot

static void wifi_supervisor_task(void* param) {
    (void)param;
    int disconnected_ms = 0;
    int since_attempt_ms = RECONNECT_EVERY_MS;  // allow a first attempt after grace

    // Let the initial boot-time connection flow run before supervising.
    vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_MS));

    for (;;) {
        wifi_mgr_state_t state = wifi_mgr_get_state();

        if (state == WIFI_MGR_STATE_CONNECTED || !wifi_mgr_has_saved_credentials()) {
            // Healthy, or nothing to supervise: reset all recovery bookkeeping
            // and stay passive.
            disconnected_ms = 0;
            since_attempt_ms = RECONNECT_EVERY_MS;
            vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_MS));
            continue;
        }

        // Not connected, saved credentials present. Track total downtime for the
        // reboot backstop regardless of sub-state.
        disconnected_ms += CHECK_INTERVAL_MS;
        since_attempt_ms += CHECK_INTERVAL_MS;

        // While an association attempt is already in flight (including the app's
        // own boot-time or user-initiated connect), do NOT issue a competing
        // reconnect — that would restart the handshake and could clobber a
        // manual attempt. Only re-issue once the link is genuinely idle
        // (DISCONNECTED / ERROR) past the grace + backoff windows.
        bool connecting = (state == WIFI_MGR_STATE_CONNECTING);
        if (!connecting && disconnected_ms >= GRACE_MS && since_attempt_ms >= RECONNECT_EVERY_MS) {
            char ssid[33] = {0};
            char password[65] = {0};
            if (wifi_mgr_load_credentials(ssid, sizeof(ssid), password, sizeof(password))) {
                ESP_LOGW(TAG, "WiFi down %ds - reconnecting to '%s'",
                         disconnected_ms / 1000, ssid);
                wifi_mgr_connect(ssid, password, nullptr);
            }
            since_attempt_ms = 0;
        }

        if (disconnected_ms >= REBOOT_AFTER_MS) {
            ESP_LOGE(TAG, "WiFi down %ds and unrecoverable in place - restarting to "
                     "reinitialize the network stack", disconnected_ms / 1000);
            vTaskDelay(pdMS_TO_TICKS(500));  // flush logs
            system_safe_restart();
        }

        vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_MS));
    }
}

void wifi_supervisor_start(void) {
    xTaskCreate(wifi_supervisor_task, "wifi_super", 4096, nullptr, 4, nullptr);
    ESP_LOGI(TAG, "WiFi health supervisor started");
}
