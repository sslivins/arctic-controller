/*
 * Arctic Heat Pump Controller
 * P-Parameter Definitions - Implementation
 */
#include "heatpump_params.h"
#include "app_preferences.h"
#include "modbus/arctic_heatpump.h"
#include <string.h>
#include <ctype.h>
#include <esp_log.h>

static const char* TAG = "hp_params";

// ============================================================================
// Demo Mode Storage
// ============================================================================
static int16_t s_demo_param_values[NUM_HEATPUMP_PARAMS];
static bool s_demo_values_initialized = false;

static void init_demo_values(void) {
    if (s_demo_values_initialized) return;
    
    // Initialize P-parameters to midpoint of range
    for (int i = 0; i < NUM_HEATPUMP_PARAMS; i++) {
        s_demo_param_values[i] = (HEATPUMP_PARAMS[i].min_val + HEATPUMP_PARAMS[i].max_val) / 2;
    }
    
    s_demo_values_initialized = true;
}

// ============================================================================
// Lookup Functions
// ============================================================================

// Case-insensitive string compare
static bool str_eq_nocase(const char* a, const char* b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == *b;
}

const HeatPumpParam* heatpump_param_find_by_key(const char* key) {
    if (!key) return nullptr;
    
    for (int i = 0; i < NUM_HEATPUMP_PARAMS; i++) {
        if (strcmp(HEATPUMP_PARAMS[i].key, key) == 0) {
            return &HEATPUMP_PARAMS[i];
        }
    }
    return nullptr;
}

const HeatPumpParam* heatpump_param_find_by_pcode(const char* pcode) {
    if (!pcode) return nullptr;
    
    for (int i = 0; i < NUM_HEATPUMP_PARAMS; i++) {
        if (str_eq_nocase(HEATPUMP_PARAMS[i].p_code, pcode)) {
            return &HEATPUMP_PARAMS[i];
        }
    }
    return nullptr;
}

const HeatPumpParam* heatpump_param_find(const char* id) {
    if (!id) return nullptr;
    
    // Try key first (exact match)
    const HeatPumpParam* param = heatpump_param_find_by_key(id);
    if (param) return param;
    
    // Try p_code (case-insensitive)
    return heatpump_param_find_by_pcode(id);
}

const char* param_unit_to_string(ParamUnit unit) {
    switch (unit) {
        case ParamUnit::STEPS:          return "steps";
        case ParamUnit::MINUTES:        return "min";
        case ParamUnit::SECONDS:        return "sec";
        case ParamUnit::TEMP_ABSOLUTE:  return "°C";
        case ParamUnit::TEMP_OFFSET:    return "°C";
        case ParamUnit::NONE:
        default:                        return "";
    }
}

int heatpump_param_get_index(const HeatPumpParam* param) {
    if (!param) return -1;
    
    // Calculate index from pointer arithmetic
    ptrdiff_t idx = param - HEATPUMP_PARAMS;
    if (idx >= 0 && idx < NUM_HEATPUMP_PARAMS) {
        return (int)idx;
    }
    return -1;
}

// ============================================================================
// Value Read/Write (handles demo mode)
// ============================================================================

int16_t heatpump_param_read_by_index(int idx, bool* success) {
    if (idx < 0 || idx >= NUM_HEATPUMP_PARAMS) {
        if (success) *success = false;
        return 0;
    }
    
    // Demo mode - return simulated value
    if (app_prefs_is_demo_mode()) {
        init_demo_values();
        if (success) *success = true;
        return s_demo_param_values[idx];
    }
    
    // Real mode - try to read from heat pump
    arctic::HeatPumpState hp = arctic::getState();
    if (!hp.connected) {
        if (success) *success = false;
        return 0;
    }
    
    uint16_t val = 0;
    if (arctic::readRegister(HEATPUMP_PARAMS[idx].reg_addr, &val)) {
        if (success) *success = true;
        return (int16_t)val;
    }
    
    if (success) *success = false;
    return 0;
}

int16_t heatpump_param_read(const HeatPumpParam* param, bool* success) {
    return heatpump_param_read_by_index(heatpump_param_get_index(param), success);
}

bool heatpump_param_write_by_index(int idx, int16_t value) {
    if (idx < 0 || idx >= NUM_HEATPUMP_PARAMS) return false;
    
    // Demo mode - store in memory
    if (app_prefs_is_demo_mode()) {
        init_demo_values();
        ESP_LOGI(TAG, "[DEMO] Storing %s = %d (in-memory)", HEATPUMP_PARAMS[idx].name, value);
        s_demo_param_values[idx] = value;
        return true;
    }
    
    // Real mode - check connection
    arctic::HeatPumpState hp = arctic::getState();
    if (!hp.connected) {
        ESP_LOGW(TAG, "Cannot write %s: not connected", HEATPUMP_PARAMS[idx].name);
        return false;
    }
    
    // Write to register
    if (arctic::writeRegister(HEATPUMP_PARAMS[idx].reg_addr, (uint16_t)value)) {
        ESP_LOGI(TAG, "Wrote %s = %d", HEATPUMP_PARAMS[idx].name, value);
        return true;
    }
    
    ESP_LOGE(TAG, "Failed to write %s", HEATPUMP_PARAMS[idx].name);
    return false;
}

bool heatpump_param_write(const HeatPumpParam* param, int16_t value) {
    return heatpump_param_write_by_index(heatpump_param_get_index(param), value);
}
