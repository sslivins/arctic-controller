/*
 * Arctic Heat Pump Controller
 * Boot / reset statistics implementation
 */

#include "boot_stats.h"
#include <nvs.h>
#include <esp_log.h>

static const char* TAG = "boot_stats";

#define NVS_NAMESPACE   "boot_stats"
#define NVS_KEY_BO_CNT  "bo_count"
#define NVS_KEY_PANIC_STREAK "panic_strk"

// Consecutive crash reboots at/above this count trip SAFE MODE for the boot.
#define SAFE_MODE_PANIC_THRESHOLD 3

static struct {
    bool initialized;
    uint32_t brownout_count;
    uint32_t panic_streak;
    bool safe_mode;
    esp_reset_reason_t reason;
} s = { false, 0, 0, false, ESP_RST_UNKNOWN };

static bool is_crash_reason(esp_reset_reason_t reason) {
    return reason == ESP_RST_PANIC || reason == ESP_RST_TASK_WDT ||
           reason == ESP_RST_INT_WDT || reason == ESP_RST_WDT;
}

void boot_stats_init(esp_reset_reason_t reason) {
    if (s.initialized) return;
    s.reason = reason;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        uint32_t cnt = 0;
        nvs_get_u32(nvs, NVS_KEY_BO_CNT, &cnt);   // leaves cnt=0 if key absent
        if (reason == ESP_RST_BROWNOUT) {
            cnt++;
            nvs_set_u32(nvs, NVS_KEY_BO_CNT, cnt);
            nvs_commit(nvs);
            ESP_LOGW(TAG, "BROWNOUT reset detected - persistent count now %lu",
                     (unsigned long)cnt);
        }
        s.brownout_count = cnt;

        // Consecutive crash-reboot streak: increment on a crash-induced reset,
        // leave untouched otherwise. It is cleared to zero only by
        // boot_stats_note_healthy() once the device proves healthy this boot.
        uint32_t streak = 0;
        nvs_get_u32(nvs, NVS_KEY_PANIC_STREAK, &streak);
        if (is_crash_reason(reason)) {
            streak++;
            nvs_set_u32(nvs, NVS_KEY_PANIC_STREAK, streak);
            nvs_commit(nvs);
            ESP_LOGW(TAG, "Crash reboot (%s) - consecutive crash streak now %lu",
                     boot_stats_reset_reason_name(reason), (unsigned long)streak);
        }
        s.panic_streak = streak;

        nvs_close(nvs);
    } else {
        ESP_LOGW(TAG, "Failed to open NVS (%s); boot stats unavailable",
                 esp_err_to_name(err));
    }

    s.safe_mode = (s.panic_streak >= SAFE_MODE_PANIC_THRESHOLD);
    if (s.safe_mode) {
        ESP_LOGE(TAG, "SAFE MODE: %lu consecutive crash reboots >= threshold %d; "
                 "optional subsystems (e.g. demo mode) disabled this boot",
                 (unsigned long)s.panic_streak, SAFE_MODE_PANIC_THRESHOLD);
    }
    s.initialized = true;
}

uint32_t boot_stats_brownout_count(void) { return s.brownout_count; }

uint32_t boot_stats_panic_streak(void) { return s.panic_streak; }

bool boot_stats_in_safe_mode(void) { return s.safe_mode; }

bool boot_stats_note_healthy(void) {
    if (s.panic_streak == 0) return true;  // nothing persisted to clear
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "note_healthy: nvs_open failed (%s); will retry", esp_err_to_name(err));
        return false;  // leave RAM streak intact so a later call retries
    }
    err = nvs_set_u32(nvs, NVS_KEY_PANIC_STREAK, 0);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "note_healthy: persist failed (%s); will retry", esp_err_to_name(err));
        return false;  // do NOT zero RAM: the persisted value must stay
                       // authoritative until it is actually cleared
    }
    ESP_LOGI(TAG, "Device healthy - cleared crash streak (was %lu)",
             (unsigned long)s.panic_streak);
    s.panic_streak = 0;
    return true;
}

esp_reset_reason_t boot_stats_last_reset_reason(void) { return s.reason; }

const char* boot_stats_reset_reason_name(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:   return "POWER_ON";
        case ESP_RST_EXT:       return "EXTERNAL";
        case ESP_RST_SW:        return "SOFTWARE";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INTERRUPT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "OTHER_WDT";
        case ESP_RST_DEEPSLEEP: return "DEEP_SLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

void boot_stats_clear(void) {
    s.brownout_count = 0;
    s.panic_streak = 0;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u32(nvs, NVS_KEY_BO_CNT, 0);
        nvs_set_u32(nvs, NVS_KEY_PANIC_STREAK, 0);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "Boot stats counters cleared");
}
