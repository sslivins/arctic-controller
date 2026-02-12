/*
 * Arctic Heat Pump Controller
 * Heat Pump System Readings Screen
 * 
 * Full-screen display of system readings (pressures, voltages, currents, etc.)
 */
#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback when screen is closed
 */
typedef void (*heatpump_system_close_cb_t)(void);

/**
 * @brief Show the system readings screen
 * @param on_close Callback when screen is closed
 */
void heatpump_system_show(heatpump_system_close_cb_t on_close);

/**
 * @brief Hide/close the system readings screen
 */
void heatpump_system_hide(void);

/**
 * @brief Check if system readings screen is visible
 */
bool heatpump_system_is_shown(void);

#ifdef __cplusplus
}
#endif
