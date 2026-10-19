/*
 * Arctic Heat Pump Controller
 * WiFi roaming (see wifi_roam.h / wifi_roam_policy.h)
 */
#include "wifi_roam.h"
#include "wifi_roam_policy.h"
#include "wifi_manager.h"
#include "event_log.h"
#include "sdkconfig.h"

#include <atomic>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_timer.h>

static const char* TAG = "wifi_roam";

static const uint32_t SCAN_TIMEOUT_MS = 8000;
static const uint32_t CONNECT_TIMEOUT_MS = 20000;
static const int MAX_BSS = 8;

static WifiRoamPolicy s_policy;
static std::atomic<bool> s_busy{false};
static std::atomic<uint32_t> s_roams{0};

static int64_t now_ms() { return esp_timer_get_time() / 1000; }

static bool wait_connected(uint32_t timeout_ms)
{
    char ip[16];
    for (uint32_t waited = 0; waited < timeout_ms; waited += 200) {
        if (wifi_mgr_get_state() == WIFI_MGR_STATE_CONNECTED &&
            wifi_mgr_get_ip_addr(ip, sizeof(ip))) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return false;
}

static const char* roam(bool any, bool bogus, bool dry)
{
    wifi_mgr_bss_t cur;
    if (!wifi_mgr_get_current_bss(&cur)) {
        return "not connected";
    }
    char ssid[33] = {0};
    char password[65] = {0};
    if (!wifi_mgr_load_credentials(ssid, sizeof(ssid), password, sizeof(password))) {
        return "no saved credentials";
    }

    wifi_mgr_bss_t found[MAX_BSS];
    const int n = wifi_mgr_scan_bss(ssid, found, MAX_BSS, SCAN_TIMEOUT_MS);
    WifiRoamCandidate c[MAX_BSS];
    for (int i = 0; i < n; i++) {
        memcpy(c[i].bssid, found[i].bssid, 6);
        c[i].rssi = found[i].rssi;
        c[i].channel = found[i].channel;
        ESP_LOGI(TAG, "  AP %02x:%02x:%02x:%02x:%02x:%02x ch %u %d dBm%s",
                 c[i].bssid[0], c[i].bssid[1], c[i].bssid[2], c[i].bssid[3],
                 c[i].bssid[4], c[i].bssid[5], (unsigned)c[i].channel, (int)c[i].rssi,
                 memcmp(c[i].bssid, cur.bssid, 6) == 0 ? " (current)" : "");
    }
    const int idx = s_policy.pick(now_ms(), cur.bssid, cur.rssi, c, n > 0 ? n : 0, any);
    if (n < 0) {
        memset(password, 0, sizeof(password));
        return "scan failed";
    }

    WifiRoamCandidate target;
    if (bogus) {
        static const uint8_t NOWHERE[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
        memcpy(target.bssid, NOWHERE, 6);
        target.rssi = cur.rssi;
        target.channel = cur.channel;
    } else if (idx < 0) {
        ESP_LOGI(TAG, "staying on ..%02x:%02x (%d dBm): no access point at least %d dB stronger",
                 cur.bssid[4], cur.bssid[5], (int)cur.rssi, WifiRoamPolicy::MIN_GAIN_DB);
        memset(password, 0, sizeof(password));
        return "no better access point";
    } else {
        target = c[idx];
    }
    if (dry) {
        ESP_LOGI(TAG, "dry run: would roam to ..%02x:%02x (%d dBm, ch %u)", target.bssid[4],
                 target.bssid[5], (int)target.rssi, (unsigned)target.channel);
        memset(password, 0, sizeof(password));
        return "dry run";
    }

    ESP_LOGW(TAG, "roaming from ..%02x:%02x (%d dBm, ch %u) to ..%02x:%02x (%d dBm, ch %u)",
             cur.bssid[4], cur.bssid[5], (int)cur.rssi, (unsigned)cur.channel,
             target.bssid[4], target.bssid[5], (int)target.rssi, (unsigned)target.channel);
    wifi_mgr_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    bool ok = wifi_mgr_connect_bss(ssid, password, target.bssid, target.channel) &&
              wait_connected(CONNECT_TIMEOUT_MS);
    s_policy.on_roam(now_ms());

    if (ok) {
        wifi_mgr_bss_t now_on;
        const int8_t rssi = wifi_mgr_get_current_bss(&now_on) ? now_on.rssi : target.rssi;
        s_roams.fetch_add(1);
        event_log_record(EVENT_WIFI_ROAMED, wifi_roam_pack(cur.rssi, rssi, target.channel));
        ESP_LOGW(TAG, "roamed: now %d dBm on ch %u", (int)rssi, (unsigned)target.channel);
        memset(password, 0, sizeof(password));
        return "roamed";
    }

    // Never leave the controller stuck on an access point that didn't take
    // us: drop the pin and join the strongest one the normal way. If that
    // fails too, the supervisor's reconnect / reboot backstops take over.
    ESP_LOGE(TAG, "roam failed - reconnecting to the strongest access point");
    wifi_mgr_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    wifi_mgr_connect(ssid, password, nullptr);
    memset(password, 0, sizeof(password));
    wait_connected(CONNECT_TIMEOUT_MS);
    return "roam failed; reconnected normally";
}

void wifi_roam_poll(void)
{
    const bool connected = wifi_mgr_get_state() == WIFI_MGR_STATE_CONNECTED;
    const int rssi = connected ? wifi_mgr_get_rssi() : 0;
    if (!s_policy.sample(now_ms(), connected, rssi)) {
        return;
    }
    bool expected = false;
    if (!s_busy.compare_exchange_strong(expected, true)) {
        return;
    }
    ESP_LOGI(TAG, "on %d dBm - checking for a stronger access point", rssi);
    const char* result = roam(false, false, false);
    ESP_LOGI(TAG, "roam check: %s", result);
    s_busy.store(false);
}

uint32_t wifi_roam_count(void)
{
    return s_roams.load();
}

bool wifi_roam_in_progress(void)
{
    return s_busy.load();
}

#ifdef CONFIG_TEST_ENDPOINTS
const char* wifi_roam_test_run(bool any, bool bogus, bool dry)
{
    bool expected = false;
    if (!s_busy.compare_exchange_strong(expected, true)) {
        return "busy";
    }
    const char* result = roam(any, bogus, dry);
    ESP_LOGW(TAG, "test roam (any=%d bogus=%d dry=%d): %s", any, bogus, dry, result);
    s_busy.store(false);
    return result;
}
#endif
