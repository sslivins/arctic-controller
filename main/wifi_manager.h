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

uint16_t wifi_mgr_get_scan_results(wifi_mgr_ap_info_t* out, uint16_t max_out);

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

/**
 * @brief Get current connection signal strength (RSSI)
 * @return RSSI in dBm (typically -30 to -90), 0 if not connected
 */
int8_t wifi_mgr_get_rssi(void);

/**
 * @brief Save WiFi credentials to NVS
 * @param ssid Network SSID
 * @param password Network password
 * @return true on success
 */
bool wifi_mgr_save_credentials(const char* ssid, const char* password);

/**
 * @brief Load saved WiFi credentials from NVS
 * @param ssid Buffer to store SSID (min 33 bytes)
 * @param ssid_len SSID buffer length
 * @param password Buffer to store password (min 65 bytes)
 * @param password_len Password buffer length
 * @return true if credentials were found
 */
bool wifi_mgr_load_credentials(char* ssid, size_t ssid_len, char* password, size_t password_len);

/**
 * @brief Check if saved credentials exist
 * @return true if credentials are saved
 */
bool wifi_mgr_has_saved_credentials(void);

/**
 * @brief Clear saved WiFi credentials from NVS
 */
void wifi_mgr_clear_credentials(void);

/**
 * @brief Get MAC address
 * @param mac Buffer to store MAC (6 bytes)
 * @return true if MAC available
 */
bool wifi_mgr_get_mac_addr(uint8_t* mac);

/**
 * @brief Get the ESP32-C6 co-processor's ESP-Hosted firmware version.
 *
 * The version is read once from the co-processor over the ESP-Hosted RPC link
 * during wifi_mgr_init(). Useful for confirming host/slave ESP-Hosted version
 * compatibility.
 *
 * @param major Out: major version (may be NULL)
 * @param minor Out: minor version (may be NULL)
 * @param patch Out: patch version (may be NULL)
 * @return true if a version was successfully read, false otherwise
 */
bool wifi_mgr_get_coprocessor_version(uint32_t* major, uint32_t* minor, uint32_t* patch);

#ifdef __cplusplus
}
#endif
