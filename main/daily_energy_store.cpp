/*
 * Arctic Heat Pump Controller
 * Reads today's energy total as saved by firmware older than the energy ledger.
 */

#include "daily_energy_store.h"

#include <nvs.h>

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
