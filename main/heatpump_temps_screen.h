/*
 * Arctic Heat Pump Controller
 * Heat Pump Temperatures Screen
 * 
 * Full-screen display of all temperature sensors with large, readable fonts.
 */
#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback when screen is closed
 */
typedef void (*heatpump_temps_close_cb_t)(void);

/**
 * @brief Show the temperatures detail screen
 * @param on_close Callback when screen is closed
 */
void heatpump_temps_show(heatpump_temps_close_cb_t on_close);

/**
 * @brief Hide/close the temperatures screen
 */
void heatpump_temps_hide(void);

/**
 * @brief Check if temperatures screen is visible
 */
bool heatpump_temps_is_shown(void);

#ifdef __cplusplus
}
#endif
