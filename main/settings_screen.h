/*
 * Arctic Heat Pump Controller
 * Settings Screen
 */
#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback when user closes the settings screen
 */
typedef void (*settings_close_cb_t)(void);

/**
 * @brief Configuration for settings screen
 */
typedef struct {
    settings_close_cb_t on_close;
} settings_screen_config_t;

/**
 * @brief Create and show the settings screen
 * @param config Configuration with callbacks
 */
void settings_screen_create(const settings_screen_config_t* config);

/**
 * @brief Close and destroy the settings screen
 */
void settings_screen_close(void);

/**
 * @brief Check if settings screen is currently shown
 * @return true if visible
 */
bool settings_screen_is_visible(void);

#ifdef __cplusplus
}
#endif
