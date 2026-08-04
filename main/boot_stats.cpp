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

static struct {
    bool initialized;
    uint32_t brownout_count;
    esp_reset_reason_t reason;
} s = { false, 0, ESP_RST_UNKNOWN };

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
        nvs_close(nvs);
    } else {
        ESP_LOGW(TAG, "Failed to open NVS (%s); brownout count unavailable",
                 esp_err_to_name(err));
    }
    s.initialized = true;
}

uint32_t boot_stats_brownout_count(void) { return s.brownout_count; }

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
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u32(nvs, NVS_KEY_BO_CNT, 0);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "Brownout counter cleared");
}
