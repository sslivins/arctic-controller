/*
 * Arctic Heat Pump Controller
 * Time Settings Screen
 */
#pragma once

#include <lvgl.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback when user closes the time settings screen
 */
typedef void (*time_screen_close_cb_t)(void);

/**
 * @brief Configuration for time settings screen
 */
typedef struct {
    time_screen_close_cb_t on_close;    // Close callback (optional)
} time_screen_config_t;

/**
 * @brief Create the time settings screen
 * @param config Screen configuration
 * @return Created screen container or NULL on error
 */
lv_obj_t* time_screen_create(const time_screen_config_t* config);

/**
 * @brief Delete the time settings screen
 */
void time_screen_delete(void);

/**
 * @brief Check if screen is currently shown
 */
bool time_screen_is_shown(void);

/**
 * @brief Get whether 24-hour format is enabled
 * @return true for 24h, false for 12h
 */
bool time_screen_get_24h_format(void);

#ifdef __cplusplus
}
#endif
