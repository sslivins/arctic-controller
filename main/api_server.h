/*
 * Arctic Heat Pump Controller
 * REST API Server with mDNS and optional TLS
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize mDNS service
 * 
 * Registers the device as "arctic.local" on the network
 * 
 * @return true on success
 */
bool api_server_init_mdns(void);

/**
 * @brief Start the REST API server
 * 
 * Should be called after WiFi is connected.
 * Starts HTTPS (port 443) if TLS certs are provisioned,
 * otherwise falls back to HTTP (port 80).
 * 
 * @return true on success
 */
bool api_server_start(void);

/**
 * @brief Stop the REST API server
 */
void api_server_stop(void);

/**
 * @brief Check if server is running
 * 
 * @return true if server is running
 */
bool api_server_is_running(void);

/**
 * @brief Get the device hostname for mDNS
 * 
 * @return hostname (e.g., "arctic")
 */
const char* api_server_get_hostname(void);

#ifdef __cplusplus
}
#endif
