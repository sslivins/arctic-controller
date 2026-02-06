/*
 * Arctic Heat Pump Controller
 * Settings Screen - Master-Detail Layout with WiFi and Firmware sections
 */
#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WiFi network information structure
 */
typedef struct {
    char ssid[33];      // Max SSID length is 32 + null terminator
    int8_t rssi;        // Signal strength
    uint8_t authmode;   // Authentication mode (0=open, 1=WEP, 2=WPA, etc.)
} settings_wifi_network_t;

/**
 * @brief Callback when user wants to connect to a network
 * @param ssid The network SSID
 * @param password The password entered by user (may be empty for open networks)
 */
typedef void (*settings_wifi_connect_cb_t)(const char* ssid, const char* password);

/**
 * @brief Callback when user wants to scan for networks
 */
typedef void (*settings_wifi_scan_cb_t)(void);

/**
 * @brief Callback when user wants to disconnect
 */
typedef void (*settings_wifi_disconnect_cb_t)(void);

/**
 * @brief Callback when user closes the settings screen
 */
typedef void (*settings_close_cb_t)(void);

/**
 * @brief Configuration for settings screen
 */
typedef struct {
    settings_close_cb_t on_close;
    settings_wifi_connect_cb_t on_wifi_connect;
    settings_wifi_scan_cb_t on_wifi_scan;
    settings_wifi_disconnect_cb_t on_wifi_disconnect;
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

/**
 * @brief Update the WiFi network list with scan results
 * @param networks Array of network info
 * @param count Number of networks in array
 */
void settings_screen_update_networks(const settings_wifi_network_t* networks, uint8_t count);

/**
 * @brief Show scanning indicator in WiFi section
 * @param scanning true to show "Scanning...", false to hide
 */
void settings_screen_set_scanning(bool scanning);

/**
 * @brief Update WiFi connection status display
 * @param connected true if connected
 * @param ssid Current network SSID (or NULL if not connected)
 * @param ip_addr IP address string (or NULL if not connected)
 */
void settings_screen_set_wifi_status(bool connected, const char* ssid, const char* ip_addr);

/**
 * @brief Show an error message
 * @param message Error message to display
 */
void settings_screen_show_error(const char* message);

/**
 * @brief Callback for update check result
 * @param update_available true if a new version is available
 * @param new_version Version string (only valid if update_available)
 */
typedef void (*update_check_cb_t)(bool update_available, const char* new_version);

/**
 * @brief Start background check for firmware updates
 *        Can be called without settings screen visible
 * @param callback Function to call with result (can be NULL)
 */
void settings_screen_check_for_updates_async(update_check_cb_t callback);

#ifdef __cplusplus
}
#endif
