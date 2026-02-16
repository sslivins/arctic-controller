/*
 * Arctic Heat Pump Controller
 * Heat Pump Status Display Screen
 */
#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the heat pump status display on the main screen
 * @param parent Parent object (main screen)
 * @param y_offset Vertical offset from top (after status bar)
 */
void heatpump_screen_create(lv_obj_t* parent, int y_offset);

/**
 * @brief Update the heat pump display with current state
 * Should be called periodically (e.g., every second)
 */
void heatpump_screen_update(void);

/**
 * @brief Delete the heat pump screen elements
 */
void heatpump_screen_delete(void);

/**
 * @brief Check if screen is currently created
 */
bool heatpump_screen_is_created(void);

/**
 * @brief Show or hide the demo mode banner on the main screen
 * @param visible true to show, false to hide
 */
void heatpump_screen_set_demo_banner(bool visible);

#ifdef __cplusplus
}
#endif
