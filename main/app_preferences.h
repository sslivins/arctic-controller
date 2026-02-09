/*
 * Arctic Heat Pump Controller
 * Application Preferences
 * 
 * Global settings stored in NVS:
 * - Demo mode enable
 * - Temperature units (Celsius/Fahrenheit)
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

// Temperature unit options
typedef enum {
    TEMP_UNIT_CELSIUS = 0,
    TEMP_UNIT_FAHRENHEIT = 1
} temp_unit_t;

/**
 * @brief Initialize app preferences from NVS
 *        Call this early in startup
 */
void app_prefs_init(void);

/**
 * @brief Check if demo mode is enabled
 * @return true if demo mode is forced on
 */
bool app_prefs_is_demo_mode(void);

/**
 * @brief Set demo mode enable state
 * @param enabled true to force demo mode
 */
void app_prefs_set_demo_mode(bool enabled);

/**
 * @brief Get current temperature unit setting
 * @return TEMP_UNIT_CELSIUS or TEMP_UNIT_FAHRENHEIT
 */
temp_unit_t app_prefs_get_temp_unit(void);

/**
 * @brief Set temperature unit
 * @param unit TEMP_UNIT_CELSIUS or TEMP_UNIT_FAHRENHEIT
 */
void app_prefs_set_temp_unit(temp_unit_t unit);

/**
 * @brief Convert Celsius to current unit
 * @param celsius Temperature in Celsius
 * @return Temperature in current unit
 */
int16_t app_prefs_convert_temp(int16_t celsius);

/**
 * @brief Convert from current unit to Celsius
 * @param temp Temperature in current unit
 * @return Temperature in Celsius
 */
int16_t app_prefs_temp_to_celsius(int16_t temp);

/**
 * @brief Get temperature unit suffix string
 * @return "°C" or "°F"
 */
const char* app_prefs_temp_unit_str(void);
