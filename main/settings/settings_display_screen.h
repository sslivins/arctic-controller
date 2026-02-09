/*
 * Arctic Heat Pump Controller
 * Settings - Display Screen (iOS-style full screen)
 * 
 * Full-screen display brightness settings with back navigation.
 */
#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration for Display screen
 */
typedef struct {
    void (*on_back)(void);  // Called when back button is pressed
} display_screen_config_t;

/**
 * @brief Create the Display settings screen
 * @param config Configuration callbacks
 */
void display_screen_create(const display_screen_config_t* config);

/**
 * @brief Close the Display settings screen
 */
void display_screen_close(void);

/**
 * @brief Check if Display screen is visible
 */
bool display_screen_is_visible(void);

#ifdef __cplusplus
}
#endif
