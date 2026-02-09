/*
 * Arctic Heat Pump Controller
 * Settings - Firmware Screen (iOS-style full screen)
 * 
 * Full-screen firmware/OTA update with back navigation.
 */
#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration for Firmware screen
 */
typedef struct {
    void (*on_back)(void);  // Called when back button is pressed
} firmware_screen_config_t;

/**
 * @brief Create the Firmware settings screen
 * @param config Configuration callbacks
 */
void firmware_screen_create(const firmware_screen_config_t* config);

/**
 * @brief Close the Firmware settings screen
 */
void firmware_screen_close(void);

/**
 * @brief Check if Firmware screen is visible
 */
bool firmware_screen_is_visible(void);

#ifdef __cplusplus
}
#endif
