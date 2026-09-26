/*
 * Arctic Heat Pump Controller
 * Controller health diagnostics document (see device_diagnostics.h).
 */

#include "device_diagnostics.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <sdkconfig.h>

#include "boot_stats.h"
#include "heatpump_controller.h"
#include "ota_manager.h"
#include "time_manager.h"
#include "macon_master_iface.h"
#include "wifi_manager.h"

namespace {

// 64-bit uptime: xTaskGetTickCount() wraps after ~49.7 days at 1 kHz.
int64_t uptime_ms()
{
    return esp_timer_get_time() / 1000;
}

void add_reset_reason(cJSON* system)
{
    const char* name =
        boot_stats_reset_reason_name(boot_stats_last_reset_reason());
    char lower[24];
    size_t i = 0;
    for (; name[i] != '\0' && i < sizeof(lower) - 1; ++i) {
        lower[i] = (char)tolower((unsigned char)name[i]);
    }
    lower[i] = '\0';
    cJSON_AddStringToObject(system, "last_reset_reason", lower);
}

void add_system(cJSON* root)
{
    cJSON* system = cJSON_AddObjectToObject(root, "system");
    if (system == nullptr) return;
    add_reset_reason(system);
    cJSON_AddNumberToObject(system, "brownout_count", boot_stats_brownout_count());
    cJSON_AddNumberToObject(system, "panic_count", boot_stats_panic_count());
    cJSON_AddNumberToObject(system, "watchdog_count", boot_stats_watchdog_count());
    cJSON_AddNumberToObject(system, "crash_streak", boot_stats_panic_streak());
    cJSON_AddBoolToObject(system, "safe_mode", boot_stats_in_safe_mode());
}

void add_memory(cJSON* root)
{
    cJSON* memory = cJSON_AddObjectToObject(root, "memory");
    if (memory == nullptr) return;
    cJSON_AddNumberToObject(memory, "internal_free_bytes",
                            (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(memory, "internal_min_free_bytes",
                            (double)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(memory, "internal_largest_free_block_bytes",
                            (double)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void add_wifi(cJSON* root)
{
    cJSON* wifi = cJSON_AddObjectToObject(root, "wifi");
    if (wifi == nullptr) return;
    const bool connected = wifi_mgr_get_state() == WIFI_MGR_STATE_CONNECTED;
    cJSON_AddBoolToObject(wifi, "connected", connected);
    const char* ssid = connected ? wifi_mgr_get_connected_ssid() : nullptr;
    if (ssid != nullptr && ssid[0] != '\0') {
        cJSON_AddStringToObject(wifi, "ssid", ssid);
    } else {
        cJSON_AddNullToObject(wifi, "ssid");
    }
    if (connected) {
        cJSON_AddNumberToObject(wifi, "rssi_dbm", wifi_mgr_get_rssi());
    } else {
        cJSON_AddNullToObject(wifi, "rssi_dbm");
    }
    cJSON_AddNumberToObject(wifi, "disconnect_count",
                            wifi_mgr_get_disconnect_count());
    const uint16_t reason = wifi_mgr_get_last_disconnect_reason();
    if (reason != 0) {
        cJSON_AddNumberToObject(wifi, "last_disconnect_reason", reason);
    } else {
        cJSON_AddNullToObject(wifi, "last_disconnect_reason");
    }
}

const char* bus_role()
{
    if (arctic::isDemoMode()) return "demo";
#if CONFIG_ARCTIC_TUYA_LISTEN
    return "listener";
#else
    if (macon_master::is_active()) return "master";
    if (macon_master::is_blocked_by_other_master()) return "blocked";
    return "inactive";
#endif
}

void add_last_ok(cJSON* rs485, bool has, int64_t at_ms)
{
    if (has) {
        cJSON_AddNumberToObject(rs485, "last_ok_uptime_ms", (double)at_ms);
    } else {
        cJSON_AddNullToObject(rs485, "last_ok_uptime_ms");
    }
}

void add_rs485(cJSON* root)
{
    cJSON* rs485 = cJSON_AddObjectToObject(root, "rs485");
    if (rs485 == nullptr) return;
    const char* role = bus_role();
    cJSON_AddStringToObject(rs485, "role", role);

    if (strcmp(role, "listener") == 0) {
        // Passive counters describe observed frames, not our transactions, so
        // they use their own keys rather than reusing the master's.
        const arctic::MaconListenerStats s = arctic::getListenerStats();
        cJSON_AddNumberToObject(rs485, "frames_ok", s.frames_ok);
        cJSON_AddNumberToObject(rs485, "checksum_errors", s.checksum_err);
        cJSON_AddNumberToObject(rs485, "resyncs", s.resync);
        // last_frame_ms is a wrapping 32-bit esp_timer stamp; convert it via
        // its (wrap-safe) age.
        const int64_t now = uptime_ms();
        const uint32_t age = (uint32_t)now - s.last_frame_ms;
        add_last_ok(rs485, s.last_frame_ms != 0, now - (int64_t)age);
        return;
    }

    if (strcmp(role, "master") == 0) {
        const macon_master::BusStats s = macon_master::get_bus_stats();
        cJSON_AddNumberToObject(rs485, "polls_ok", s.poll.ok);
        cJSON_AddNumberToObject(rs485, "polls_no_response", s.poll.no_response);
        cJSON_AddNumberToObject(rs485, "polls_transport_error", s.poll.transport_error);
        cJSON_AddNumberToObject(rs485, "checksum_errors", s.poll.checksum_errors);
        cJSON_AddNumberToObject(rs485, "consecutive_failures", s.poll.consecutive_failures);
        cJSON_AddNumberToObject(rs485, "writes_ok", s.writes_ok);
        cJSON_AddNumberToObject(rs485, "writes_failed", s.writes_failed);
        add_last_ok(rs485, s.has_last_ok, s.last_ok_uptime_ms);
    }
}

}  // namespace

cJSON* device_diagnostics_create(void)
{
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) return nullptr;

    cJSON_AddNumberToObject(root, "uptime_ms", (double)uptime_ms());
    add_system(root);
    add_memory(root);
    add_wifi(root);

    cJSON* time_obj = cJSON_AddObjectToObject(root, "time");
    if (time_obj != nullptr) {
        cJSON_AddBoolToObject(time_obj, "synced", time_mgr_is_synced());
    }

    cJSON* ota = cJSON_AddObjectToObject(root, "ota");
    if (ota != nullptr) {
        cJSON_AddBoolToObject(ota, "busy", ota_mgr_is_busy());
        cJSON_AddBoolToObject(ota, "pending_verify", ota_mgr_is_pending_verify());
    }

    add_rs485(root);
    return root;
}
