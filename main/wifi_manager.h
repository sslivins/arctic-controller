/*
 * Arctic Heat Pump Controller
 * WiFi Manager - Handles ESP-Hosted WiFi via ESP32-C6
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WiFi connection state
 */
typedef enum {
    WIFI_MGR_STATE_NOT_INITIALIZED,
    WIFI_MGR_STATE_DISCONNECTED,
    WIFI_MGR_STATE_CONNECTING,
    WIFI_MGR_STATE_CONNECTED,
    WIFI_MGR_STATE_ERROR,
} wifi_mgr_state_t;

/**
 * @brief Scanned network information
 */
typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t authmode;  // 0=open, 1=WEP, 2=WPA, 3=WPA2, 4=WPA3, etc.
} wifi_mgr_ap_info_t;

/**
 * @brief Callback for scan results
 * @param ap_list Array of access points found
 * @param count Number of access points
 */
typedef void (*wifi_mgr_scan_done_cb_t)(const wifi_mgr_ap_info_t* ap_list, uint16_t count);

/**
 * @brief Callback for connection state changes
 * @param state New connection state
 * @param ssid Connected SSID (only valid when state is CONNECTED)
 */
typedef void (*wifi_mgr_state_cb_t)(wifi_mgr_state_t state, const char* ssid);

/**
 * @brief Initialize WiFi manager
 *        Powers on ESP32-C6 and initializes ESP-Hosted
 * @return true on success
 */
bool wifi_mgr_init(void);

/**
 * @brief Deinitialize WiFi manager
 */
void wifi_mgr_deinit(void);

/**
 * @brief Check if WiFi manager is initialized
 * @return true if initialized
 */
bool wifi_mgr_is_initialized(void);

/**
 * @brief Start scanning for WiFi networks
 * @param callback Function to call when scan completes
 * @return true if scan started successfully
 */
bool wifi_mgr_start_scan(wifi_mgr_scan_done_cb_t callback);

/**
 * @brief Connect to a WiFi network
 * @param ssid Network SSID
 * @param password Network password (NULL or empty for open networks)
 * @param state_callback Function to call on state changes (optional)
 * @return true if connection attempt started
 */
bool wifi_mgr_connect(const char* ssid, const char* password, wifi_mgr_state_cb_t state_callback);

/**
 * @brief Disconnect from current network
 */
void wifi_mgr_disconnect(void);

/**
 * @brief Get current connection state
 * @return Current WiFi state
 */
wifi_mgr_state_t wifi_mgr_get_state(void);

/**
 * @brief Get connected network SSID
 * @return SSID string or NULL if not connected
 */
const char* wifi_mgr_get_connected_ssid(void);

/**
 * @brief Get IP address (as string)
 * @param buf Buffer to store IP string
 * @param buf_len Buffer length
 * @return true if IP available
 */
bool wifi_mgr_get_ip_addr(char* buf, size_t buf_len);

#ifdef __cplusplus
}
#endif
