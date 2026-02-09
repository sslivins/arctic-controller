/*
 * Arctic Heat Pump Controller
 * Settings - Time Screen (iOS-style full screen)
 * 
 * Full-screen time/timezone configuration with back navigation.
 */
#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration for Time screen
 */
typedef struct {
    void (*on_back)(void);  // Called when back button is pressed
} time_screen_config_t;

/**
 * @brief Create the Time settings screen
 * @param config Configuration callbacks
 */
void time_screen_create(const time_screen_config_t* config);

/**
 * @brief Close the Time settings screen
 */
void time_screen_close(void);

/**
 * @brief Check if Time screen is visible
 */
bool time_screen_is_visible(void);

/**
 * @brief Get the current 24-hour format setting
 * @return true if 24-hour format, false for 12-hour
 */
bool time_screen_get_24h_format(void);

#ifdef __cplusplus
}
#endif
