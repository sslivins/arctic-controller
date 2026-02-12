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
 * @brief Show the event log screen
 * @param on_close Callback when screen is closed
 */
void event_log_screen_show(event_log_screen_close_cb_t on_close);

/**
 * @brief Hide/close the event log screen
 */
void event_log_screen_hide(void);

/**
 * @brief Check if event log screen is visible
 */
bool event_log_screen_is_shown(void);

#ifdef __cplusplus
}
#endif
