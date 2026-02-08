/*
 * Arctic Heat Pump Controller
 * Heat Pump Detail Screen - Shows all sensor readings
 */
#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback when detail screen is closed
 */
typedef void (*heatpump_detail_close_cb_t)(void);

/**
 * @brief Show the heat pump detail screen
 * @param on_close Callback when screen is closed (optional)
 */
void heatpump_detail_show(heatpump_detail_close_cb_t on_close);

/**
 * @brief Hide/delete the detail screen
 */
void heatpump_detail_hide(void);

/**
 * @brief Check if detail screen is currently shown
 */
bool heatpump_detail_is_shown(void);

#ifdef __cplusplus
}
#endif
