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
    API_START_ERR_BAD_CERT,     // the administrator certificate/key will not load
    API_START_ERR_UNRESPONSIVE, // running, but not answering the self-probe
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
 * @return true if the server carrying the web UI and API is running (443
 *         in HTTPS mode, 80 in HTTP mode). Port 80 in HTTPS mode and the
 *         Home Assistant server (8443) are secondary: if they fail the start
 *         still succeeds and api_server_ensure_running() repairs them later.
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
 * @brief The server carrying the web UI and API is fully started (routes
 *        included): 443 in HTTPS mode, 80 in HTTP mode.
 */
bool api_server_is_running(void);

/**
 * @brief Running AND every secondary server is up: port 80 (health notice)
 *        in HTTPS mode, and the Home Assistant server (8443) whenever a
 *        device identity exists to serve it.
 */
bool api_server_is_healthy(void);

/**
 * @brief Start the servers if the web UI/API server is not running, or
 *        restart only the missing secondary servers (8443, port 80 in HTTPS
 *        mode) without touching the running web UI/API server.
 *
 * No-op when already healthy or after api_server_shutdown().
 * @return true if the web UI/API server is running afterwards
 */
bool api_server_ensure_running(void);

/**
 * @brief Connect to the running web UI/API server over loopback and check
 *        that GET /api/health answers 200 (TLS handshake included in HTTPS
 *        mode). Blocks for up to ~16 s (8 s each to connect and to read);
 *        must not be called from an httpd task.
 *
 * @return true if the server answered, or if it is not running (nothing to
 *         probe -- api_server_is_running() covers that case)
 */
bool api_server_probe(void);

/**
 * @brief Stop serving HTTPS for the rest of this boot and fall back to HTTP
 *        on port 80, recording @p cause. Stops the servers when HTTPS is
 *        currently running; the caller restarts them with
 *        api_server_ensure_running().
 */
void api_server_disable_https(api_start_error_t cause);

/** @brief True while the web UI/API is served over HTTPS (port 443). */
bool api_server_https_active(void);

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
 * @brief Test hooks (CONFIG_TEST_ENDPOINTS builds only). Make the next
 *        @p count self-probes fail, the next @p count certificate checks
 *        reject the stored pair, or the next @p count Home Assistant (8443)
 *        or port 80 (HTTPS mode) starts fail.
 */
void api_server_test_fail_probes(int count);
void api_server_test_fail_cert_checks(int count);
void api_server_test_fail_integration_starts(int count);
void api_server_test_fail_http_starts(int count);

/** @brief Test hook: undo api_server_disable_https() (servers keep running). */
void api_server_test_reenable_https(void);

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
