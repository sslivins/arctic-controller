/*
 * Arctic Heat Pump Controller
 * Settings - WiFi Screen (iOS-style full screen)
 * 
 * Full-screen WiFi configuration with back navigation.
 */
#pragma once

#include <lvgl.h>
#include "../settings_screen.h"  // For settings_wifi_network_t

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration for WiFi screen
 */
typedef struct {
    void (*on_wifi_scan)(void);
    void (*on_wifi_connect)(const char* ssid, const char* password);
    void (*on_wifi_disconnect)(void);
    void (*on_back)(void);  // Called when back button is pressed
    bool use_fade;          // Use fade animation instead of slide (for status bar entry)
} wifi_screen_config_t;

/**
 * @brief Create the WiFi settings screen
 * @param config Configuration callbacks
 */
void wifi_screen_create(const wifi_screen_config_t* config);

/**
 * @brief Close the WiFi settings screen
 */
void wifi_screen_close(void);

/**
 * @brief Check if WiFi screen is visible
 */
bool wifi_screen_is_visible(void);

/**
 * @brief Update connection status display
 * @param is_connected Whether WiFi is connected
 * @param ssid Connected SSID (or NULL)
 * @param ip IP address (or NULL)
 */
void wifi_screen_update_connection(bool is_connected, const char* ssid, const char* ip);

/**
 * @brief Update available networks list
 * @param networks Array of network info
 * @param count Number of networks
 */
void wifi_screen_update_networks(const settings_wifi_network_t* networks, uint8_t count);

/**
 * @brief Set scanning state (shows loading indicator)
 * @param scanning True if scan in progress
 */
void wifi_screen_set_scanning(bool scanning);

/**
 * @brief Trigger a WiFi scan
 */
void wifi_screen_trigger_scan(void);

#ifdef __cplusplus
}
#endif
