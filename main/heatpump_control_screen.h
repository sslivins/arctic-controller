/*
 * Arctic Heat Pump Controller
 * Heat Pump Control Screen - Modify settings
 */
#pragma once

#include <lvgl.h>

// Callback when screen is closed
typedef void (*heatpump_control_close_cb_t)(void);

// Show the control screen (full-screen overlay)
void heatpump_control_show(heatpump_control_close_cb_t on_close = nullptr);

// Hide and clean up the control screen
void heatpump_control_hide(void);

// Check if currently shown
bool heatpump_control_is_shown(void);
