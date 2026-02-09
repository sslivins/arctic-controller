/*
 * Arctic Heat Pump Controller
 * Application Preferences Implementation
 */

#include "app_preferences.h"
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_log.h>
#include <math.h>

static const char* TAG = "app_prefs";

// NVS namespace and keys
#define NVS_NAMESPACE    "app_prefs"
#define NVS_KEY_DEMO     "demo_mode"
#define NVS_KEY_TEMP_UNIT "temp_unit"

// Current state (cached from NVS)
static struct {
    bool initialized = false;
    bool demo_mode = false;
    temp_unit_t temp_unit = TEMP_UNIT_CELSIUS;
} s_prefs;

void app_prefs_init(void) {
    if (s_prefs.initialized) return;
    
    ESP_LOGI(TAG, "Loading app preferences from NVS");
    
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        uint8_t val = 0;
        
        if (nvs_get_u8(nvs, NVS_KEY_DEMO, &val) == ESP_OK) {
            s_prefs.demo_mode = (val != 0);
        }
        
        if (nvs_get_u8(nvs, NVS_KEY_TEMP_UNIT, &val) == ESP_OK) {
            s_prefs.temp_unit = (val == 1) ? TEMP_UNIT_FAHRENHEIT : TEMP_UNIT_CELSIUS;
        }
        
        nvs_close(nvs);
    } else {
        ESP_LOGW(TAG, "No saved preferences found, using defaults");
    }
    
    s_prefs.initialized = true;
    ESP_LOGI(TAG, "Preferences: demo_mode=%d, temp_unit=%s", 
             s_prefs.demo_mode, 
             s_prefs.temp_unit == TEMP_UNIT_CELSIUS ? "C" : "F");
}

bool app_prefs_is_demo_mode(void) {
    return s_prefs.demo_mode;
}

void app_prefs_set_demo_mode(bool enabled) {
    if (s_prefs.demo_mode == enabled) return;
    
    s_prefs.demo_mode = enabled;
    
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        nvs_set_u8(nvs, NVS_KEY_DEMO, enabled ? 1 : 0);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Saved demo_mode = %d", enabled);
    } else {
        ESP_LOGE(TAG, "Failed to save demo_mode: %s", esp_err_to_name(err));
    }
}

temp_unit_t app_prefs_get_temp_unit(void) {
    return s_prefs.temp_unit;
}

void app_prefs_set_temp_unit(temp_unit_t unit) {
    if (s_prefs.temp_unit == unit) return;
    
    s_prefs.temp_unit = unit;
    
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        nvs_set_u8(nvs, NVS_KEY_TEMP_UNIT, (uint8_t)unit);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Saved temp_unit = %s", unit == TEMP_UNIT_CELSIUS ? "C" : "F");
    } else {
        ESP_LOGE(TAG, "Failed to save temp_unit: %s", esp_err_to_name(err));
    }
}

int16_t app_prefs_convert_temp(int16_t celsius) {
    if (s_prefs.temp_unit == TEMP_UNIT_FAHRENHEIT) {
        return (int16_t)roundf((celsius * 9.0f / 5.0f) + 32.0f);
    }
    return celsius;
}

float app_prefs_convert_temp_f(int16_t celsius) {
    if (s_prefs.temp_unit == TEMP_UNIT_FAHRENHEIT) {
        return (celsius * 9.0f / 5.0f) + 32.0f;
    }
    return (float)celsius;
}

int16_t app_prefs_convert_temp_diff(int16_t celsius_diff) {
    if (s_prefs.temp_unit == TEMP_UNIT_FAHRENHEIT) {
        // No +32 for differentials
        return (int16_t)roundf(celsius_diff * 9.0f / 5.0f);
    }
    return celsius_diff;
}

float app_prefs_convert_temp_diff_f(int16_t celsius_diff) {
    if (s_prefs.temp_unit == TEMP_UNIT_FAHRENHEIT) {
        return celsius_diff * 9.0f / 5.0f;
    }
    return (float)celsius_diff;
}

int16_t app_prefs_temp_to_celsius(int16_t temp) {
    if (s_prefs.temp_unit == TEMP_UNIT_FAHRENHEIT) {
        return (int16_t)roundf((temp - 32.0f) * 5.0f / 9.0f);
    }
    return temp;
}

int16_t app_prefs_temp_to_celsius_from_f(float temp) {
    if (s_prefs.temp_unit == TEMP_UNIT_FAHRENHEIT) {
        return (int16_t)roundf((temp - 32.0f) * 5.0f / 9.0f);
    }
    return (int16_t)roundf(temp);
}

int16_t app_prefs_temp_diff_to_celsius(int16_t diff) {
    if (s_prefs.temp_unit == TEMP_UNIT_FAHRENHEIT) {
        return (int16_t)roundf(diff * 5.0f / 9.0f);
    }
    return diff;
}

int16_t app_prefs_temp_diff_to_celsius_from_f(float diff) {
    if (s_prefs.temp_unit == TEMP_UNIT_FAHRENHEIT) {
        return (int16_t)roundf(diff * 5.0f / 9.0f);
    }
    return (int16_t)roundf(diff);
}

const char* app_prefs_temp_unit_str(void) {
    return s_prefs.temp_unit == TEMP_UNIT_FAHRENHEIT ? "°F" : "°C";
}

const char* app_prefs_temp_diff_unit_str(void) {
    return s_prefs.temp_unit == TEMP_UNIT_FAHRENHEIT ? "Δ°F" : "°C";
}
