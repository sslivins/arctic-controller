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

/**
 * @brief Callback for async update check
 * @param update_available True if a newer version is available
 * @param new_version Version string of the new release (or empty)
 */
typedef void (*firmware_update_check_cb_t)(bool update_available, const char* new_version);

/**
 * @brief Check for firmware updates in the background
 * @param callback Called when check completes (may be called from LVGL context)
 */
void firmware_screen_check_for_updates_async(firmware_update_check_cb_t callback);

#ifdef __cplusplus
}
#endif
