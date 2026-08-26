/*
 * Arctic Heat Pump Controller
 * Settings - Web Interface Screen (iOS-style full screen)
 *
 * Full-screen screen that shows a QR code and the plain-text URL for the
 * device's browser dashboard, so a phone can scan (or a user can type) the
 * address to open the web interface.
 */
#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration for the Web Interface screen
 */
typedef struct {
    void (*on_back)(void);  // Called when back button is pressed
} web_screen_config_t;

/**
 * @brief Create and show the Web Interface settings screen
 * @param config Configuration callbacks
 */
void web_screen_create(const web_screen_config_t* config);

/**
 * @brief Close the Web Interface settings screen
 */
void web_screen_close(void);

/**
 * @brief Check if the Web Interface screen is visible
 */
bool web_screen_is_visible(void);

#ifdef __cplusplus
}
#endif
