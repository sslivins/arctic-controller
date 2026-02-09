/*
 * Arctic Heat Pump Controller
 * Heat Pump Error Details Screen
 * 
 * Full-screen display of active errors with descriptions and severity
 */
#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback when screen is closed
 */
typedef void (*heatpump_errors_close_cb_t)(void);

/**
 * @brief Show the error details screen
 * @param on_close Callback when screen is closed
 */
void heatpump_errors_show(heatpump_errors_close_cb_t on_close);

/**
 * @brief Hide/close the error details screen
 */
void heatpump_errors_hide(void);

/**
 * @brief Check if error details screen is visible
 */
bool heatpump_errors_is_shown(void);

#ifdef __cplusplus
}
#endif
