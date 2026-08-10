/*
 * Arctic Heat Pump Controller
 * Heat Pump Control Screen - Modify settings
 */
#pragma once

#include <lvgl.h>

// Callback when screen is closed
typedef void (*heatpump_control_close_cb_t)(void);

// Build the control tab content into the given panel (used by the tab shell).
void heatpump_control_create_in(lv_obj_t* parent);

// Hide and clean up the control screen
void heatpump_control_hide(void);

// Pause/resume the update timer (tab shell drives this so only the visible
// tab polls).
void heatpump_control_set_active(bool active);

// Close edit dialogs without tearing down the persistent Control tab.
void heatpump_control_dismiss_overlays(void);

// Check if currently shown
bool heatpump_control_is_shown(void);
