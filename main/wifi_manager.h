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
 * @brief Get current IPv4 address/netmask in network byte order.
 * @return true if connected and both values are available.
 */
bool wifi_mgr_get_ip_info(uint32_t* ip_out, uint32_t* netmask_out);

/**
 * @brief Get current connection signal strength (RSSI)
 * @return RSSI in dBm (typically -30 to -90), 0 if not connected
 */
int8_t wifi_mgr_get_rssi(void);

/**
 * @brief One access point (BSS) of the configured network.
 */
typedef struct {
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
} wifi_mgr_bss_t;

/**
 * @brief The access point the station is associated with.
 * @return false when not connected or the C6 can't report it.
 */
bool wifi_mgr_get_current_bss(wifi_mgr_bss_t* out);

/**
 * @brief Connect to one specific access point of `ssid` (used to roam).
 *        Later reconnects through wifi_mgr_connect() are not pinned.
 */
bool wifi_mgr_connect_bss(const char* ssid, const char* password,
                          const uint8_t bssid[6], uint8_t channel);

/**
 * @brief Blocking all-channel scan for the access points of `ssid`, kept
 *        out of the user-facing scan results.
 * @return number of entries written to `out`, or -1 if the scan failed.
 */
int wifi_mgr_scan_bss(const char* ssid, wifi_mgr_bss_t* out, int max_out,
                      uint32_t timeout_ms);

/**
 * @brief DHCP hostname: the controller's friendly name made hostname-safe,
 *        or "Macon-Heat-Pump-Controller-XXXX" (see dhcp_hostname.h).
 */
const char* wifi_mgr_get_hostname(void);

/**
 * @brief Re-derive the DHCP hostname after the friendly name changes. The
 *        router sees it at the next DHCP renewal or reconnect.
 */
void wifi_mgr_refresh_hostname(void);

/**
 * @brief Number of STA disconnect events this boot (includes failed
 *        reconnect attempts).
 */
uint32_t wifi_mgr_get_disconnect_count(void);

/**
 * @brief wifi_err_reason_t of the most recent disconnect this boot (0 = none).
 */
uint16_t wifi_mgr_get_last_disconnect_reason(void);

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

/**
 * @brief Ask the API service task to (re)start the web/API servers if they
 *        are not healthy. Non-blocking; safe from event handlers.
 */
void wifi_mgr_request_api_start(void);

/**
 * @brief Run the same (re)start synchronously, waiting up to @p timeout_ms.
 *
 * Used by the WiFi supervisor's safety net. All starts are executed by the
 * single API service task, so callers never race each other.
 * @return true if the web UI/API server is running afterwards (a missing
 *         secondary server -- 8443, or port 80 in HTTPS mode -- is retried
 *         but does not make this false)
 */
bool wifi_mgr_api_ensure_running(uint32_t timeout_ms);

/**
 * @brief Stop the web/API servers and start them again, on the API service
 *        task, waiting up to @p timeout_ms. With @p disable_https, HTTPS is
 *        turned off for the rest of this boot first (see
 *        api_server_disable_https), recording @p cause.
 * @return true if the web UI/API server is running afterwards
 */
bool wifi_mgr_api_restart(bool disable_https, int cause, uint32_t timeout_ms);

/**
 * @brief Run api_server_probe() on the API service task, waiting up to
 *        @p timeout_ms. A probe that does not finish in time (the task is
 *        stuck, e.g. in a hung server stop) counts as a failure.
 */
bool wifi_mgr_api_probe(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
