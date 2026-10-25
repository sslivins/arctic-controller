/*
 * Arctic Heat Pump Controller
 * REST API Server with mDNS and optional TLS
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Why the most recent server start failed (or degraded).
typedef enum {
    API_START_ERR_NONE = 0,
    API_START_ERR_NO_MEMORY,    // a server task stack could not be allocated
    API_START_ERR_NO_IDENTITY,  // no TLS certificate/identity for HTTPS
    API_START_ERR_FAILED,       // any other startup failure
    API_START_ERR_INJECTED,     // test-only injected failure
} api_start_error_t;

/**
 * @brief Initialize mDNS service
 * 
 * Registers the device on the network as "arctic-<xxxx>.local", where
 * <xxxx> is the last two bytes of the WiFi station MAC (lowercase hex).
 * The MAC suffix keeps the name unique when multiple controllers share a
 * network. Idempotent: after the first success later calls do nothing
 * (mDNS follows IP changes on its own).
 * 
 * @return true on success
 */
bool api_server_init_mdns(void);

/**
 * @brief Start the REST API server
 * 
 * Should be called after WiFi is connected. Serialized with stop/ensure by an
 * internal lock; a partially started server is torn down and restarted.
 * 
 * @return true if the mandatory servers (HTTP 80 + HTTPS 443) are running
 */
bool api_server_start(void);

/**
 * @brief Stop the REST API server
 */
void api_server_stop(void);

/**
 * @brief Stop the servers for good (OTA flashing / imminent reboot).
 *
 * Latches a flag so api_server_ensure_running() never restarts them.
 */
void api_server_shutdown(void);

/** @brief true once api_server_shutdown() has been called. */
bool api_server_is_shut_down(void);

/**
 * @brief Check if the mandatory servers are fully started (routes included)
 */
bool api_server_is_running(void);

/**
 * @brief Mandatory servers running AND the Home Assistant server (8443) is
 *        up whenever a device identity exists to serve it.
 */
bool api_server_is_healthy(void);

/**
 * @brief Restart the servers if they are not healthy.
 *
 * No-op when already healthy or after api_server_shutdown().
 * @return true if healthy afterwards
 */
bool api_server_ensure_running(void);

/** @brief Cause of the most recent failed or degraded start. */
api_start_error_t api_server_last_start_error(void);

/**
 * @brief Test hook (CONFIG_TEST_ENDPOINTS builds only): make the next
 *        @p count starts fail as if out of memory.
 */
void api_server_test_fail_starts(int count);

/**
 * @brief Test hook (CONFIG_TEST_ENDPOINTS builds only): make the next
 *        @p count HTTPS (port 443) starts fail, so the servers fall back to
 *        HTTP mode while an administrator certificate is installed.
 */
void api_server_test_fail_https_starts(int count);

/**
 * @brief True when an administrator certificate is installed but HTTPS failed
 *        to start, so the web UI and API are being served over HTTP instead.
 */
bool api_server_https_failed(void);

/** @brief The event_web_server_cause_t value for a start error. */
uint32_t api_server_start_error_event_cause(api_start_error_t err);

/**
 * @brief Get the device hostname for mDNS
 * 
 * @return hostname (e.g., "arctic-3f2a")
 */
const char* api_server_get_hostname(void);

#ifdef __cplusplus
}
#endif
