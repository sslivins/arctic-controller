/*
 * Arctic Heat Pump Controller
 * Event Log Screen
 * 
 * Full-screen display of recent operational events
 */
#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback when screen is closed
 */
typedef void (*event_log_screen_close_cb_t)(void);

/**
 * @brief Build the event log (Events) tab content into the given panel.
 *        Used by the tab shell; the panel is persistent.
 * @param parent The tab panel to build into.
 */
void event_log_screen_create_in(lv_obj_t* parent);

/**
 * @brief Hide/close the event log screen
 */
void event_log_screen_hide(void);

/**
 * @brief Pause/resume the update timer (tab shell drives this so only the
 *        visible tab polls). Rebuilds the list when activated.
 */
void event_log_screen_set_active(bool active);

// Close search/filter overlays without tearing down the persistent Events tab.
void event_log_screen_dismiss_overlays(void);

/**
 * @brief Check if event log screen is visible
 */
bool event_log_screen_is_shown(void);

#ifdef __cplusplus
}
#endif
