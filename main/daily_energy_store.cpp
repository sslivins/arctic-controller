/*
 * Arctic Heat Pump Controller
 * NVS storage for today's energy total.
 */

#include "daily_energy_store.h"

#include <esp_log.h>
#include <nvs.h>

static const char* TAG = "energy_store";

#define NVS_NAMESPACE "energy"
#define NVS_KEY_DAY   "day"
#define NVS_KEY_WS    "ws"

bool daily_energy_store_load(int32_t* day_key, uint64_t* watt_seconds) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;
    const bool ok = nvs_get_i32(nvs, NVS_KEY_DAY, day_key) == ESP_OK &&
                    nvs_get_u64(nvs, NVS_KEY_WS, watt_seconds) == ESP_OK;
    nvs_close(nvs);
    return ok;
}

bool daily_energy_store_save(int32_t day_key, uint64_t watt_seconds) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_i32(nvs, NVS_KEY_DAY, day_key);
        if (err == ESP_OK) err = nvs_set_u64(nvs, NVS_KEY_WS, watt_seconds);
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save today's energy: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}
