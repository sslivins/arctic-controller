/*
 * Arctic Heat Pump Controller
 * Settings Menu - iOS-style settings list
 * 
 * Main settings screen with a list of setting categories.
 * Each row opens a full-screen sub-setting when tapped.
 */
#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration for the settings menu
 */
typedef struct {
    void (*on_close)(void);  // Callback when settings is closed
    
    // WiFi callbacks (passed to WiFi sub-screen)
    void (*on_wifi_scan)(void);
    void (*on_wifi_connect)(const char* ssid, const char* password);
    void (*on_wifi_disconnect)(void);
    
    // Firmware callbacks (passed to Firmware sub-screen)
    void (*on_check_updates)(void);
    void (*on_update_firmware)(const char* url);
} settings_menu_config_t;

/**
 * @brief Create and show the settings menu
 * @param config Optional configuration (can be NULL)
 */
void settings_menu_create(const settings_menu_config_t* config);

/**
 * @brief Close the settings menu and cleanup
 */
void settings_menu_close(void);

/**
 * @brief Check if settings menu is currently visible
 */
bool settings_menu_is_visible(void);

/**
 * @brief Navigate back to the settings menu from a sub-screen
 */
void settings_menu_show(void);

/**
 * @brief Refresh the settings menu for language change
 */
void settings_menu_refresh(void);

/**
 * @brief Update WiFi status display (called from wifi_manager callbacks)
 */
void settings_menu_update_wifi_status(bool connected, const char* ssid);

#ifdef __cplusplus
}
#endif
