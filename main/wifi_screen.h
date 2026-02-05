/*
 * Arctic Heat Pump Controller
 * WiFi Settings Screen
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
} wifi_network_info_t;

/**
 * @brief Callback when user wants to connect to a network
 * @param ssid The network SSID
 * @param password The password entered by user (may be empty for open networks)
 */
typedef void (*wifi_connect_cb_t)(const char* ssid, const char* password);

/**
 * @brief Callback when user wants to scan for networks
 */
typedef void (*wifi_scan_cb_t)(void);

/**
 * @brief Callback when user wants to disconnect
 */
typedef void (*wifi_disconnect_cb_t)(void);

/**
 * @brief Callback when user closes the WiFi screen
 */
typedef void (*wifi_close_cb_t)(void);

/**
 * @brief Configuration for WiFi screen
 */
typedef struct {
    wifi_connect_cb_t on_connect;
    wifi_scan_cb_t on_scan;
    wifi_disconnect_cb_t on_disconnect;
    wifi_close_cb_t on_close;
} wifi_screen_config_t;

/**
 * @brief Create and show the WiFi settings screen
 * @param config Configuration with callbacks
 */
void wifi_screen_create(const wifi_screen_config_t* config);

/**
 * @brief Close and destroy the WiFi screen
 */
void wifi_screen_close(void);

/**
 * @brief Check if WiFi screen is currently shown
 * @return true if visible
 */
bool wifi_screen_is_visible(void);

/**
 * @brief Update the network list with scan results
 * @param networks Array of network info
 * @param count Number of networks in array
 */
void wifi_screen_update_networks(const wifi_network_info_t* networks, uint8_t count);

/**
 * @brief Show scanning indicator
 * @param scanning true to show "Scanning...", false to hide
 */
void wifi_screen_set_scanning(bool scanning);

/**
 * @brief Update connection status display
 * @param connected true if connected
 * @param ssid Current network SSID (or NULL if not connected)
 * @param ip_addr IP address string (or NULL if not connected)
 */
void wifi_screen_set_connection_status(bool connected, const char* ssid, const char* ip_addr);

/**
 * @brief Show an error message
 * @param message Error message to display
 */
void wifi_screen_show_error(const char* message);

#ifdef __cplusplus
}
#endif
