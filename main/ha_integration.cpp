/*
 * Home Assistant integration identity and versioned state serialization.
 */
#include "ha_integration.h"

#include "heatpump_controller.h"
#include "heatpump_errors.h"
#include "macon_state.h"
#include "macon_master.h"
#include "wifi_manager.h"

#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <string.h>

namespace arctic::ha {
namespace {

constexpr size_t SHA256_LEN = 32;
constexpr size_t DEVICE_ID_LEN = 20;
constexpr size_t BOOT_ID_LEN = 33;

const char* TAG = "ha_integration";
char s_device_id[DEVICE_ID_LEN] = {};
char s_boot_id[BOOT_ID_LEN] = {};
uint8_t s_state_hash[SHA256_LEN] = {};
uint64_t s_revision = 0;
bool s_has_state_hash = false;
bool s_initialized = false;
SemaphoreHandle_t s_revision_mutex = nullptr;

void bytesToHex(const uint8_t* bytes, size_t count, char* output)
{
    static constexpr char HEX[] = "0123456789abcdef";
    for (size_t i = 0; i < count; ++i) {
        output[i * 2] = HEX[bytes[i] >> 4];
        output[i * 2 + 1] = HEX[bytes[i] & 0x0f];
    }
    output[count * 2] = '\0';
}

void addSetpointLimits(cJSON* parent)
{
    const struct {
        const char* key;
        SetpointKind kind;
    } limit_kinds[] = {
        {"cooling", SetpointKind::Cooling},
        {"heating", SetpointKind::Heating},
        {"hot_water", SetpointKind::HotWater},
    };

    for (const auto& item : limit_kinds) {
        const SetpointLimits limits = setpoint_limits(item.kind);
        cJSON* value = cJSON_AddObjectToObject(parent, item.key);
        cJSON_AddNumberToObject(value, "min", limits.min_c);
        cJSON_AddNumberToObject(value, "max", limits.max_c);
    }
}

cJSON* createStateObject(const HeatPumpState& hp)
{
    cJSON* state = cJSON_CreateObject();
    if (state == nullptr) {
        return nullptr;
    }

    cJSON_AddBoolToObject(state, "connected", hp.connected);
    cJSON_AddBoolToObject(state, "unit_on", hp.unit_on);
    cJSON_AddStringToObject(state, "mode", workingModeToString(hp.working_mode));
    cJSON_AddStringToObject(state, "operation",
                            heatPumpOperationToString(hp.operation));
    cJSON_AddBoolToObject(state, "defrosting", hp.isDefrosting());

    cJSON* components = cJSON_AddObjectToObject(state, "components");
    cJSON_AddBoolToObject(components, "compressor", hp.isCompressorRunning());
    cJSON_AddBoolToObject(components, "fan", hp.isFanRunning());
    cJSON_AddNumberToObject(components, "fan_level", hp.getFanSpeedLevel());
    cJSON_AddBoolToObject(components, "water_pump", hp.isWaterPumpRunning());
    cJSON_AddBoolToObject(components, "backup_heater", hp.isBackupHeaterOn());
    cJSON_AddBoolToObject(
        components, "reversing_valve_request",
        (hp.status1 & status1::FOUR_WAY_VALVE) != 0);

    cJSON* temperatures = cJSON_AddObjectToObject(state, "temperatures_c");
    cJSON_AddNumberToObject(temperatures, "tank", hp.water_tank_temp);
    cJSON_AddNumberToObject(temperatures, "outlet", hp.outlet_water_temp);
    cJSON_AddNumberToObject(temperatures, "inlet", hp.inlet_water_temp);
    cJSON_AddNumberToObject(
        temperatures, "outdoor_ambient", hp.outdoor_ambient_temp);
    cJSON_AddNumberToObject(temperatures, "discharge", hp.discharge_temp);
    cJSON_AddNumberToObject(temperatures, "suction", hp.suction_temp);
    cJSON_AddNumberToObject(
        temperatures, "outdoor_coil", hp.outdoor_coil_temp);
    cJSON_AddNumberToObject(
        temperatures, "indoor_coil", hp.indoor_coil_temp);
    cJSON_AddNumberToObject(temperatures, "ipm", hp.ipm_temp);

    cJSON* setpoints = cJSON_AddObjectToObject(state, "setpoints_c");
    cJSON_AddNumberToObject(setpoints, "cooling", hp.cooling_setpoint);
    cJSON_AddNumberToObject(setpoints, "heating", hp.heating_setpoint);
    cJSON_AddNumberToObject(setpoints, "hot_water", hp.hot_water_setpoint);

    cJSON* readings = cJSON_AddObjectToObject(state, "readings");
    cJSON_AddNumberToObject(
        readings, "compressor_frequency_hz", hp.compressor_freq);
    cJSON_AddNumberToObject(readings, "fan_rpm", hp.fan_speed);
    cJSON_AddNumberToObject(readings, "power_w", hp.realtime_power_w);
    cJSON_AddNumberToObject(readings, "thermal_w", hp.thermal_w);
    if (hp.cop_valid) {
        cJSON_AddNumberToObject(readings, "cop", hp.cop_x100 / 100.0);
    } else {
        cJSON_AddNullToObject(readings, "cop");
    }

    cJSON* error = cJSON_AddObjectToObject(state, "error");
    cJSON_AddBoolToObject(error, "active", hp.hasAnyError());
    if (hp.hasAnyError()) {
        // Surface the primary (highest-severity) active fault so the HA
        // integration can expose a stable code + severity. The full active
        // list and suggested resolutions remain a device-local convenience
        // (see /api/heatpump/errors).
        ActiveError actives[16];
        int active_count = getActiveErrors(actives, 16);
        const ActiveError* primary = nullptr;
        for (int i = 0; i < active_count; ++i) {
            if (primary == nullptr ||
                actives[i].severity > primary->severity) {
                primary = &actives[i];
            }
        }
        if (primary != nullptr) {
            cJSON_AddStringToObject(error, "code", primary->code);
            cJSON_AddStringToObject(error, "name", primary->name);
            cJSON_AddStringToObject(
                error, "description", primary->description);
            cJSON_AddStringToObject(
                error, "severity", severityToString(primary->severity));
        } else {
            // Error bits set but unmapped — fall back to concatenated text.
            char descriptions[256];
            getErrorDescriptions(descriptions, sizeof(descriptions));
            cJSON_AddNullToObject(error, "code");
            cJSON_AddNullToObject(error, "name");
            cJSON_AddStringToObject(error, "description", descriptions);
            cJSON_AddNullToObject(error, "severity");
        }
    } else {
        cJSON_AddNullToObject(error, "code");
        cJSON_AddNullToObject(error, "name");
        cJSON_AddNullToObject(error, "description");
        cJSON_AddNullToObject(error, "severity");
    }

    return state;
}

bool calculateHash(const cJSON* value, uint8_t* hash)
{
    char* serialized = cJSON_PrintUnformatted(value);
    if (serialized == nullptr) {
        return false;
    }

    mbedtls_sha256(
        reinterpret_cast<const unsigned char*>(serialized),
        strlen(serialized), hash, 0);
    cJSON_free(serialized);
    return true;
}

bool assignRevision(const cJSON* state, uint64_t* revision)
{
    uint8_t hash[SHA256_LEN];
    if (!calculateHash(state, hash)) {
        return false;
    }

    if (xSemaphoreTake(s_revision_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out acquiring revision mutex");
        return false;
    }

    if (!s_has_state_hash ||
        memcmp(hash, s_state_hash, sizeof(s_state_hash)) != 0) {
        memcpy(s_state_hash, hash, sizeof(s_state_hash));
        s_has_state_hash = true;
        ++s_revision;
    }
    *revision = s_revision;
    xSemaphoreGive(s_revision_mutex);
    return true;
}

}  // namespace

bool init()
{
    if (s_initialized) {
        return true;
    }

    uint8_t mac[6];
    if (!wifi_mgr_get_mac_addr(mac)) {
        ESP_LOGE(TAG, "Unable to derive stable device identity");
        return false;
    }
    snprintf(
        s_device_id, sizeof(s_device_id),
        "arctic-%02x%02x%02x%02x%02x%02x",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    uint8_t boot_random[16];
    esp_fill_random(boot_random, sizeof(boot_random));
    bytesToHex(boot_random, sizeof(boot_random), s_boot_id);

    s_revision_mutex = xSemaphoreCreateMutex();
    if (s_revision_mutex == nullptr) {
        ESP_LOGE(TAG, "Unable to create revision mutex");
        return false;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Initialized device_id=%s", s_device_id);
    return true;
}

const char* deviceId()
{
    return s_device_id;
}

const char* bootId()
{
    return s_boot_id;
}

cJSON* createCapabilities()
{
    if (!s_initialized) {
        return nullptr;
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return nullptr;
    }

    const esp_app_desc_t* app = esp_app_get_description();
    const HeatPumpState hp = getState();
    const bool connected = hp.connected;
    const bool demo_controls = connected && isDemoMode();
    const bool active_master_controls =
        connected && macon_master::is_active();
    const bool cooling_setpoint =
        demo_controls || active_master_controls;
    const bool hot_water_setpoint =
        demo_controls || active_master_controls;
    const bool heating_setpoint = demo_controls;

    cJSON_AddNumberToObject(root, "protocol_version", PROTOCOL_VERSION);
    cJSON_AddStringToObject(root, "device_id", s_device_id);
    cJSON_AddStringToObject(root, "model", "Arctic Heat Pump Controller");
    cJSON_AddStringToObject(root, "firmware_version", app->version);

    cJSON* transports = cJSON_AddObjectToObject(root, "transports");
    cJSON_AddBoolToObject(transports, "rest", true);
    cJSON_AddBoolToObject(transports, "websocket", true);

    cJSON* capabilities = cJSON_AddObjectToObject(root, "capabilities");
    cJSON_AddBoolToObject(capabilities, "read_state", true);
    cJSON_AddBoolToObject(capabilities, "control_power", demo_controls);
    cJSON_AddBoolToObject(capabilities, "control_mode", demo_controls);
    cJSON_AddBoolToObject(
        capabilities, "control_setpoints",
        cooling_setpoint || hot_water_setpoint);
    cJSON_AddBoolToObject(capabilities, "advanced_parameters", false);
    cJSON_AddBoolToObject(capabilities, "raw_registers", false);

    cJSON* modes = cJSON_AddArrayToObject(capabilities, "supported_modes");
    if (demo_controls) {
        cJSON_AddItemToArray(modes, cJSON_CreateString("cooling"));
        cJSON_AddItemToArray(modes, cJSON_CreateString("floor_heating"));
        cJSON_AddItemToArray(
            modes, cJSON_CreateString("fan_coil_heating"));
        cJSON_AddItemToArray(modes, cJSON_CreateString("hot_water"));
        cJSON_AddItemToArray(modes, cJSON_CreateString("auto"));
    }

    cJSON* setpoint_controls =
        cJSON_AddObjectToObject(capabilities, "setpoint_controls");
    cJSON_AddBoolToObject(setpoint_controls, "cooling", cooling_setpoint);
    // Heating setpoint writes are available only in the synthetic demo
    // adapter; the live Tuya mapping is unverified and unsupported.
    cJSON_AddBoolToObject(setpoint_controls, "heating", heating_setpoint);
    cJSON_AddBoolToObject(
        setpoint_controls, "hot_water", hot_water_setpoint);

    cJSON* limits = cJSON_AddObjectToObject(root, "setpoint_limits_c");
    addSetpointLimits(limits);
    return root;
}

cJSON* createStateSnapshot()
{
    if (!s_initialized) {
        return nullptr;
    }

    const HeatPumpState hp = getState();
    cJSON* state = createStateObject(hp);
    if (state == nullptr) {
        return nullptr;
    }

    uint64_t revision = 0;
    if (!assignRevision(state, &revision)) {
        cJSON_Delete(state);
        return nullptr;
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        cJSON_Delete(state);
        return nullptr;
    }

    cJSON_AddNumberToObject(root, "protocol_version", PROTOCOL_VERSION);
    cJSON_AddStringToObject(root, "device_id", s_device_id);
    cJSON_AddStringToObject(root, "boot_id", s_boot_id);
    cJSON_AddNumberToObject(root, "revision", static_cast<double>(revision));
    cJSON_AddNumberToObject(
        root, "captured_at_ms",
        static_cast<double>(esp_timer_get_time() / 1000));
    cJSON_AddItemToObject(root, "state", state);
    return root;
}

}  // namespace arctic::ha
