/*
 * Arctic Heat Pump Controller
 * Settings Screen - Display Panel Header
 */
#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the display settings panel
 * @param parent Parent content area
 */
void display_panel_create(lv_obj_t* parent);

/**
 * @brief Delete/cleanup display panel resources
 */
void display_panel_delete(void);

/**
 * @brief Show the display panel
 */
void display_panel_show(void);

/**
 * @brief Hide the display panel
 */
void display_panel_hide(void);

/**
 * @brief Get the current brightness value
 * @return Current brightness percentage (0-100)
 */
int display_panel_get_brightness(void);

/**
 * @brief Initialize display brightness from saved settings
 * Called at startup to restore brightness
 */
void display_panel_init_brightness(void);

#ifdef __cplusplus
}
#endif
