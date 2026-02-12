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
 * @return Temperature in current unit (rounded)
 */
int16_t app_prefs_convert_temp(int16_t celsius);

/**
 * @brief Convert Celsius to current unit (float for editing)
 * @param celsius Temperature in Celsius
 * @return Temperature in current unit as float
 */
float app_prefs_convert_temp_f(int16_t celsius);

/**
 * @brief Convert temperature differential/offset to current unit
 *        For offsets, no +32 is applied (e.g., a 5°C difference = 9°F difference)
 * @param celsius_diff Temperature difference in Celsius
 * @return Temperature difference in current unit (rounded)
 */
int16_t app_prefs_convert_temp_diff(int16_t celsius_diff);

/**
 * @brief Convert temperature differential/offset to current unit (float)
 * @param celsius_diff Temperature difference in Celsius
 * @return Temperature difference in current unit as float
 */
float app_prefs_convert_temp_diff_f(int16_t celsius_diff);

/**
 * @brief Convert from current unit to Celsius
 * @param temp Temperature in current unit
 * @return Temperature in Celsius
 */
int16_t app_prefs_temp_to_celsius(int16_t temp);

/**
 * @brief Convert from current unit to Celsius (from float)
 * @param temp Temperature in current unit as float
 * @return Temperature in Celsius (rounded)
 */
int16_t app_prefs_temp_to_celsius_from_f(float temp);

/**
 * @brief Convert temperature differential from current unit to Celsius
 * @param diff Temperature difference in current unit
 * @return Temperature difference in Celsius
 */
int16_t app_prefs_temp_diff_to_celsius(int16_t diff);

/**
 * @brief Convert temperature differential from current unit to Celsius (from float)
 * @param diff Temperature difference in current unit as float
 * @return Temperature difference in Celsius (rounded)
 */
int16_t app_prefs_temp_diff_to_celsius_from_f(float diff);

/**
 * @brief Get temperature unit suffix string
 * @return "°C" or "°F"
 */
const char* app_prefs_temp_unit_str(void);

/**
 * @brief Get temperature differential unit suffix string
 * @return "°C" or "Δ°F"
 */
const char* app_prefs_temp_diff_unit_str(void);
