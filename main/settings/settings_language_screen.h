/*
 * Arctic Heat Pump Controller
 * Settings - Language Screen (iOS-style full screen)
 * 
 * Full-screen language selection with back navigation.
 */
#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration for Language screen
 */
typedef struct {
    void (*on_back)(void);  // Called when back button is pressed
} language_screen_config_t;

/**
 * @brief Create the Language settings screen
 * @param config Configuration callbacks
 */
void language_screen_create(const language_screen_config_t* config);

/**
 * @brief Close the Language settings screen
 */
void language_screen_close(void);

/**
 * @brief Check if Language screen is visible
 */
bool language_screen_is_visible(void);

#ifdef __cplusplus
}
#endif
