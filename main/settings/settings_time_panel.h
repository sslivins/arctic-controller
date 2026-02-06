/*
 * Arctic Heat Pump Controller
 * Settings Screen - Time Panel Header
 */
#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the time settings panel
 * @param parent Parent content area
 */
void time_panel_create(lv_obj_t* parent);

/**
 * @brief Delete/cleanup time panel resources
 */
void time_panel_delete(void);

/**
 * @brief Show the time panel
 */
void time_panel_show(void);

/**
 * @brief Hide the time panel
 */
void time_panel_hide(void);

/**
 * @brief Check if using 24-hour format
 * @return true if 24-hour format, false for 12-hour
 */
bool time_panel_get_24h_format(void);

#ifdef __cplusplus
}
#endif
