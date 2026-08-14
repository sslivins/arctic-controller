/*
 * Arctic Heat Pump Controller
 * REST API Server with mDNS, Web Interface, and Authentication
 */
#include "api_server.h"
#include "settings/settings_display_screen.h"
#include "app_preferences.h"
#include "i18n/i18n.h"
#include "wifi_manager.h"
#include "time_manager.h"
#include "ota_manager.h"
#include "auth_manager.h"
#include "ha_integration.h"
#include "setup_pairing.h"
#include "ha_websocket.h"
#include "heatpump_controller.h"
#include "heatpump_types.h"
#include "macon_master.h"
#include "macon_state.h"  // arctic::setpoint_limits / SetpointKind
#include "advanced_params.h"  // advanced_param_write() AP guardrail
#include "heatpump_errors.h"
#include "event_log.h"
#include "factory_reset.h"
#include "boot_stats.h"
#include "log_buffer.h"
#include "app_preferences.h"
#include "test_endpoints.h"
#include "tls_manager.h"
#include "png_uncompressed.h"
#include <esp_http_server.h>
#include <esp_https_server.h>
#include <esp_log.h>
#include <mdns.h>
#include <cJSON.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <lvgl.h>
#include <bsp/m5stack_tab5.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

static const char* TAG = "api_server";

// Base hostname for mDNS. The effective hostname appends the last two bytes
// of the WiFi station MAC (e.g. "arctic-3f2a") so multiple controllers on the
// same network get unique, stable .local names instead of colliding on
// "arctic.local".
static const char* HOSTNAME_BASE = "arctic";
static char hostname[32] = "arctic";  // Actual hostname (may have suffix)

// HTTP/HTTPS server handle (only one is active per boot)
static httpd_handle_t server = NULL;      // HTTP (port 80) — always running
static httpd_handle_t server_ssl = NULL;  // HTTPS (port 443) — when TLS certs present
static httpd_handle_t server_integration = NULL;  // HA HTTPS/WSS (port 8443)
#ifdef CONFIG_TEST_ENDPOINTS
static httpd_handle_t websocket_test_server = NULL;  // WS feasibility (port 81)
#endif

// Embedded web files (from EMBED_FILES) - gzip compressed
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_index_html_gz_end");

// Session cookie name
static const char* SESSION_COOKIE_NAME = "arctic_session";

// Home Assistant commands are serialized and retained only after an
// acknowledged write. This bounds memory and makes a retry with the same
// command ID safe even while another request is in flight.
static constexpr size_t HA_COMMAND_ID_MAX = 64;
static constexpr size_t HA_COMMAND_OPERATION_MAX = 16;
static constexpr size_t HA_COMMAND_PAYLOAD_MAX = 96;
static constexpr size_t HA_COMMAND_CACHE_SIZE = 32;
struct HaCommandRecord {
    bool accepted;
    char command_id[HA_COMMAND_ID_MAX + 1];
    char operation[HA_COMMAND_OPERATION_MAX + 1];
    char payload[HA_COMMAND_PAYLOAD_MAX + 1];
};
static HaCommandRecord s_ha_command_cache[HA_COMMAND_CACHE_SIZE] = {};
static size_t s_ha_command_next = 0;
static SemaphoreHandle_t s_ha_command_mutex = nullptr;

// ============================================================================
// Forward Declarations
// ============================================================================

// Web handlers
static esp_err_t web_root_handler(httpd_req_t* req);
static esp_err_t http_https_required_handler(httpd_req_t* req,
                                             httpd_err_code_t err);
static esp_err_t web_login_handler(httpd_req_t* req);
static esp_err_t web_logout_handler(httpd_req_t* req);
static esp_err_t favicon_handler(httpd_req_t* req);

// API handlers
static esp_err_t health_get_handler(httpd_req_t* req);
static esp_err_t status_get_handler(httpd_req_t* req);
static esp_err_t time_get_handler(httpd_req_t* req);
static esp_err_t time_config_handler(httpd_req_t* req);
static esp_err_t time_sync_handler(httpd_req_t* req);
static esp_err_t wifi_get_handler(httpd_req_t* req);
static esp_err_t wifi_scan_post_handler(httpd_req_t* req);
static esp_err_t wifi_networks_get_handler(httpd_req_t* req);
static esp_err_t wifi_connect_post_handler(httpd_req_t* req);
static esp_err_t wifi_disconnect_post_handler(httpd_req_t* req);
static esp_err_t info_get_handler(httpd_req_t* req);
static esp_err_t ha_pair_post_handler(httpd_req_t* req);
static esp_err_t ha_capabilities_get_handler(httpd_req_t* req);
static esp_err_t ha_state_get_handler(httpd_req_t* req);
static esp_err_t ha_power_put_handler(httpd_req_t* req);
static esp_err_t ha_mode_put_handler(httpd_req_t* req);
static esp_err_t ha_setpoint_put_handler(httpd_req_t* req);
static esp_err_t ota_status_get_handler(httpd_req_t* req);
static esp_err_t ota_update_post_handler(httpd_req_t* req);
static esp_err_t ota_upload_post_handler(httpd_req_t* req);
static esp_err_t ota_reboot_post_handler(httpd_req_t* req);
static esp_err_t ota_releases_get_handler(httpd_req_t* req);
static esp_err_t ota_github_update_post_handler(httpd_req_t* req);
static esp_err_t auth_config_get_handler(httpd_req_t* req);
static esp_err_t auth_config_post_handler(httpd_req_t* req);
static esp_err_t auth_status_get_handler(httpd_req_t* req);
static esp_err_t auth_credentials_post_handler(httpd_req_t* req);
static esp_err_t auth_apikey_get_handler(httpd_req_t* req);
static esp_err_t auth_apikey_regenerate_handler(httpd_req_t* req);
static esp_err_t heatpump_status_handler(httpd_req_t* req);
static esp_err_t heatpump_raw_handler(httpd_req_t* req);
static esp_err_t heatpump_windows_handler(httpd_req_t* req);
static esp_err_t heatpump_advanced_get_handler(httpd_req_t* req);
static esp_err_t heatpump_advanced_single_get_handler(httpd_req_t* req);
static esp_err_t heatpump_advanced_put_handler(httpd_req_t* req);
static esp_err_t heatpump_power_put_handler(httpd_req_t* req);
static esp_err_t heatpump_mode_put_handler(httpd_req_t* req);
static esp_err_t heatpump_setpoints_put_handler(httpd_req_t* req);
static esp_err_t heatpump_errors_get_handler(httpd_req_t* req);
static esp_err_t heatpump_errors_clear_handler(httpd_req_t* req);
static esp_err_t heatpump_demo_patch_handler(httpd_req_t* req);
static esp_err_t heatpump_diagnostic_get_handler(httpd_req_t* req);
static esp_err_t events_get_handler(httpd_req_t* req);
static esp_err_t events_clear_handler(httpd_req_t* req);
static esp_err_t brownout_clear_handler(httpd_req_t* req);
static esp_err_t display_brightness_get_handler(httpd_req_t* req);
static esp_err_t display_brightness_put_handler(httpd_req_t* req);
static esp_err_t preferences_get_handler(httpd_req_t* req);
static esp_err_t preferences_patch_handler(httpd_req_t* req);
static esp_err_t factory_reset_post_handler(httpd_req_t* req);
static esp_err_t logs_get_handler(httpd_req_t* req);
static esp_err_t logs_clear_handler(httpd_req_t* req);
static esp_err_t screenshot_get_handler(httpd_req_t* req);

// TLS management handlers
static esp_err_t tls_status_get_handler(httpd_req_t* req);
static esp_err_t tls_cert_post_handler(httpd_req_t* req);
static esp_err_t tls_cert_delete_handler(httpd_req_t* req);

// Home Assistant management handlers (web UI)
static esp_err_t ha_manage_status_get_handler(httpd_req_t* req);
static esp_err_t ha_manage_pair_post_handler(httpd_req_t* req);
static esp_err_t ha_manage_pair_cancel_handler(httpd_req_t* req);
static esp_err_t ha_manage_revoke_post_handler(httpd_req_t* req);

// ============================================================================
// Authentication Helpers
// ============================================================================

static bool get_session_from_cookie(httpd_req_t* req, char* token_out)
{
    char cookie_buf[256] = {0};
    size_t cookie_len = sizeof(cookie_buf);
    
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie_buf, cookie_len) != ESP_OK) {
        return false;
    }
    
    // Look for arctic_session=...
    char* session_start = strstr(cookie_buf, SESSION_COOKIE_NAME);
    if (session_start == NULL) return false;
    
    session_start += strlen(SESSION_COOKIE_NAME) + 1;  // Skip "arctic_session="
    
    // Extract token until ; or end
    int i = 0;
    while (session_start[i] != '\0' && session_start[i] != ';' && i < AUTH_SESSION_TOKEN_LEN) {
        token_out[i] = session_start[i];
        i++;
    }
    token_out[i] = '\0';
    
    return i > 0;
}

static bool get_api_key_from_header(httpd_req_t* req, char* key_out)
{
    char key_buf[64] = {0};
    size_t key_len = sizeof(key_buf);
    
    if (httpd_req_get_hdr_value_str(req, "X-API-Key", key_buf, key_len) != ESP_OK) {
        return false;
    }
    
    strncpy(key_out, key_buf, AUTH_API_KEY_LEN);
    key_out[AUTH_API_KEY_LEN] = '\0';
    return true;
}

static bool check_web_auth(httpd_req_t* req)
{
    char token[AUTH_SESSION_TOKEN_LEN + 1] = {0};
    if (get_session_from_cookie(req, token)) {
        if (auth_mgr_validate_session(token)) {
            return true;
        }
    }
    
    return false;
}

static bool check_api_auth(httpd_req_t* req)
{
    if (auth_mgr_credentials_change_required()) {
        return false;
    }
    
    // First check for API key in header (programmatic access)
    char key[AUTH_API_KEY_LEN + 1] = {0};
    if (get_api_key_from_header(req, key)) {
        return auth_mgr_validate_api_key(key);
    }
    
    // Also accept valid session cookies (web UI access when logged in)
    char token[AUTH_SESSION_TOKEN_LEN + 1] = {0};
    if (get_session_from_cookie(req, token)) {
        if (auth_mgr_validate_session(token)) {
            return true;
        }
    }
    
    return false;
}

static bool check_integration_auth(
    httpd_req_t* req, uint32_t* generation_out = nullptr)
{
    constexpr const char* BEARER_PREFIX = "Bearer ";
    constexpr size_t BEARER_PREFIX_LEN = 7;
    char authorization[BEARER_PREFIX_LEN + AUTH_INTEGRATION_TOKEN_LEN + 1] = {};
    const size_t header_len =
        httpd_req_get_hdr_value_len(req, "Authorization");
    if (header_len != sizeof(authorization) - 1 ||
        httpd_req_get_hdr_value_str(
            req, "Authorization", authorization,
            sizeof(authorization)) != ESP_OK ||
        strncmp(authorization, BEARER_PREFIX, BEARER_PREFIX_LEN) != 0) {
        return false;
    }

    return auth_mgr_validate_integration_token_with_generation(
        authorization + BEARER_PREFIX_LEN, generation_out);
}

static void set_session_cookie(httpd_req_t* req, const char* token)
{
    char cookie[160];
    if (tls_mgr_is_https_active()) {
        snprintf(cookie, sizeof(cookie),
                 "%s=%s; Path=/; Max-Age=%d; HttpOnly; Secure; SameSite=Strict",
                 SESSION_COOKIE_NAME, token, AUTH_SESSION_LIFETIME_SEC);
    } else {
        snprintf(cookie, sizeof(cookie),
                 "%s=%s; Path=/; Max-Age=%d; HttpOnly; SameSite=Strict",
                 SESSION_COOKIE_NAME, token, AUTH_SESSION_LIFETIME_SEC);
    }
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
}

static void clear_session_cookie(httpd_req_t* req)
{
    char cookie[160];
    if (tls_mgr_is_https_active()) {
        snprintf(cookie, sizeof(cookie),
                 "%s=; Path=/; Max-Age=0; HttpOnly; Secure; SameSite=Strict",
                 SESSION_COOKIE_NAME);
    } else {
        snprintf(cookie, sizeof(cookie),
                 "%s=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict",
                 SESSION_COOKIE_NAME);
    }
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
}

// ============================================================================
// mDNS
// ============================================================================

bool api_server_init_mdns(void)
{
    ESP_LOGI(TAG, "Initializing mDNS...");
    
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return false;
    }
    
    // Build a per-device hostname by appending the last two bytes of the
    // WiFi station MAC as four lowercase hex digits (e.g. "arctic-3f2a").
    // This avoids "arctic.local" collisions when several controllers share a
    // network. We use wifi_mgr_get_mac_addr() so the suffix is derived from
    // the actual C6 station MAC that the device advertises (the same value
    // reported by GET /api/wifi) rather than the P4 host MAC. api_server_init_mdns()
    // runs after the station has an IP, so this MAC is available here. If the
    // read fails for any reason, fall back to the base name.
    uint8_t mac[6] = {0};
    if (wifi_mgr_get_mac_addr(mac)) {
        snprintf(hostname, sizeof(hostname), "%s-%02x%02x",
                 HOSTNAME_BASE, mac[4], mac[5]);
    } else {
        ESP_LOGW(TAG, "wifi_mgr_get_mac_addr failed; using base hostname");
        snprintf(hostname, sizeof(hostname), "%s", HOSTNAME_BASE);
    }
    
    // Set hostname
    err = mdns_hostname_set(hostname);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS hostname set failed: %s", esp_err_to_name(err));
        return false;
    }
    
    // Set instance name
    err = mdns_instance_name_set("Arctic Heat Pump Controller");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS instance name set failed: %s", esp_err_to_name(err));
    }
    
    // Always advertise HTTP on port 80
    err = mdns_service_add(hostname, "_http", "_tcp", 80, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS _http service add failed: %s", esp_err_to_name(err));
    }
    
    // Also advertise HTTPS on port 443 when TLS certs are present
    if (tls_mgr_has_certs()) {
        err = mdns_service_add(hostname, "_https", "_tcp", 443, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "mDNS _https service add failed: %s", esp_err_to_name(err));
        }
    }
    
    // Add service TXT records
    const char* svc_type = "_http";
    mdns_txt_item_t serviceTxtData[] = {
        {"version", "1.0"},
        {"device", "arctic-controller"}
    };
    err = mdns_service_txt_set(svc_type, "_tcp", serviceTxtData, 2);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS service TXT set failed: %s", esp_err_to_name(err));
    }
    
    ESP_LOGI(TAG, "mDNS initialized: %s.local", hostname);
    return true;
}

const char* api_server_get_hostname(void)
{
    return hostname;
}

// ============================================================================
// HTTP Server
// ============================================================================

bool api_server_start(void)
{
    if (server != NULL) {
        ESP_LOGW(TAG, "Server already running");
        return true;
    }
    
    // Common httpd config values
    if (!arctic::ha::init()) {
        ESP_LOGE(TAG, "Failed to initialize Home Assistant state foundation");
        return false;
    }

    const int uri_handlers = 111;
    const int stack_size   = 16384;  // Default task stack
    const int max_headers  = 16;
    const int recv_timeout = 10;     // seconds
    
    esp_err_t ret;

    if (s_ha_command_mutex == nullptr) {
        s_ha_command_mutex = xSemaphoreCreateMutex();
        if (s_ha_command_mutex == nullptr) {
            ESP_LOGE(TAG, "Failed to initialize HA command mutex");
            return false;
        }
    }
    
    // ---- Always start HTTP on port 80 ----
    // This ensures CI, local development, and OTA always work regardless of TLS state.
    {
        ESP_LOGI(TAG, "Starting HTTP server on port 80...");
        
        httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
        http_config.lru_purge_enable   = true;
        http_config.uri_match_fn       = httpd_uri_match_wildcard;
        http_config.max_uri_handlers   = uri_handlers;
        http_config.stack_size         = stack_size;
        http_config.max_resp_headers   = max_headers;
        http_config.recv_wait_timeout  = recv_timeout;
        // Port 80 only serves essential health/OTA bootstrap routes when
        // mandatory HTTPS is active, so two concurrent clients are sufficient.
        http_config.max_open_sockets   = 2;
#ifdef CONFIG_TEST_ENDPOINTS
        http_config.send_wait_timeout  = 1;
#endif
        
        ret = httpd_start(&server, &http_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
            return false;
        }
    }
    
    // ---- Always start HTTPS using either the administrator certificate or
    // the persistent device identity generated on first boot. ----
    if (tls_mgr_has_certs() || tls_mgr_has_identity()) {
        size_t cert_len = 0, key_len = 0;
        const bool administrator_cert = tls_mgr_has_certs();
        const uint8_t* cert = administrator_cert
            ? tls_mgr_get_cert(&cert_len)
            : tls_mgr_get_identity_cert(&cert_len);
        const uint8_t* key = administrator_cert
            ? tls_mgr_get_key(&key_len)
            : tls_mgr_get_identity_key(&key_len);
        
        ESP_LOGI(TAG, "Starting HTTPS server on port 443...");
        
        httpd_ssl_config_t ssl_config = HTTPD_SSL_CONFIG_DEFAULT();
        ssl_config.httpd.lru_purge_enable   = true;
        ssl_config.httpd.uri_match_fn       = httpd_uri_match_wildcard;
        ssl_config.httpd.max_uri_handlers   = uri_handlers;
        ssl_config.httpd.stack_size         = stack_size;
        ssl_config.httpd.max_resp_headers   = max_headers;
        ssl_config.httpd.recv_wait_timeout  = recv_timeout;
        ssl_config.httpd.max_open_sockets   = 4;      // TLS buffers in PSRAM via EXTERNAL_MEM_ALLOC
#ifdef CONFIG_TEST_ENDPOINTS
        ssl_config.httpd.send_wait_timeout  = 1;
#endif
        ssl_config.servercert    = cert;
        ssl_config.servercert_len = cert_len;
        ssl_config.prvtkey_pem   = key;
        ssl_config.prvtkey_len   = key_len;
        
        ret = httpd_ssl_start(&server_ssl, &ssl_config);
        if (ret == ESP_OK) {
            tls_mgr_set_https_active(true);
            ESP_LOGI(
                TAG, "HTTPS server started on port 443 using %s",
                administrator_cert
                    ? "administrator certificate"
                    : "persistent device identity");
        } else {
            ESP_LOGE(
                TAG, "Failed to start mandatory HTTPS server: %s",
                esp_err_to_name(ret));
            api_server_stop();
            return false;
        }
    } else {
        ESP_LOGE(TAG, "No TLS identity available for mandatory HTTPS");
        api_server_stop();
        return false;
    }

    if (tls_mgr_has_identity()) {
        size_t cert_len = 0;
        size_t key_len = 0;
        const uint8_t* cert = tls_mgr_get_identity_cert(&cert_len);
        const uint8_t* key = tls_mgr_get_identity_key(&key_len);

        httpd_ssl_config_t integration_config = HTTPD_SSL_CONFIG_DEFAULT();
        integration_config.port_secure = 8443;
        integration_config.httpd.ctrl_port = 32771;
        integration_config.httpd.max_uri_handlers = 8;
        integration_config.httpd.max_open_sockets = 5;
        integration_config.httpd.stack_size = 12288;
        // Reject surplus connections instead of evicting established WSS
        // clients. Three WSS slots leave capacity for REST reconciliation
        // while another TLS connection is still closing or handshaking.
        integration_config.httpd.lru_purge_enable = false;
        integration_config.httpd.recv_wait_timeout = 10;
        integration_config.httpd.send_wait_timeout = 1;
        integration_config.servercert = cert;
        integration_config.servercert_len = cert_len;
        integration_config.prvtkey_pem = key;
        integration_config.prvtkey_len = key_len;

        ret = httpd_ssl_start(&server_integration, &integration_config);
        if (ret != ESP_OK) {
            server_integration = NULL;
            ESP_LOGE(TAG,
                     "Failed to start integration HTTPS server: %s "
                     "(HTTP remains available on port 80)",
                     esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "Integration HTTPS server started on port 8443");
        }
    } else {
        ESP_LOGE(TAG, "Integration HTTPS identity is unavailable");
    }
    
    // Helper macro - abort if URI registration fails (catches max_uri_handlers issues)
    // When HTTPS is active, registers on HTTPS only.
    // When no HTTPS, registers on HTTP.
    #define REGISTER_URI(uri_struct) do { \
        httpd_handle_t _target = server_ssl ? server_ssl : server; \
        esp_err_t err = httpd_register_uri_handler(_target, &(uri_struct)); \
        if (err != ESP_OK) { \
            ESP_LOGE(TAG, "FATAL: Failed to register URI '%s': %s", (uri_struct).uri, esp_err_to_name(err)); \
            ESP_LOGE(TAG, "Increase max_uri_handlers in httpd config!"); \
            abort(); \
        } \
    } while(0)
    
    // Register on both HTTP and HTTPS only for explicitly public health checks.
    #define REGISTER_URI_ESSENTIAL(uri_struct) do { \
        esp_err_t err = httpd_register_uri_handler(server, &(uri_struct)); \
        if (err != ESP_OK) { \
            ESP_LOGE(TAG, "FATAL: Failed to register URI '%s': %s", (uri_struct).uri, esp_err_to_name(err)); \
            abort(); \
        } \
        if (server_ssl != NULL) { \
            err = httpd_register_uri_handler(server_ssl, &(uri_struct)); \
            if (err != ESP_OK) { \
                ESP_LOGE(TAG, "FATAL: Failed to register URI '%s' on HTTPS: %s", (uri_struct).uri, esp_err_to_name(err)); \
                abort(); \
            } \
        } \
    } while(0)
    
    // ========================================================================
    // Web Interface
    // ========================================================================
    
    // GET / or /index.html - Web UI
    httpd_uri_t web_root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = web_root_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(web_root_uri);
    
    httpd_uri_t web_index_uri = {
        .uri = "/index.html",
        .method = HTTP_GET,
        .handler = web_root_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(web_index_uri);
    
    // GET /favicon.ico - Return 204 No Content (no favicon)
    httpd_uri_t favicon_uri = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = favicon_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(favicon_uri);
    
    // POST /login - Web login
    httpd_uri_t login_uri = {
        .uri = "/login",
        .method = HTTP_POST,
        .handler = web_login_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(login_uri);
    
    // POST /logout - Web logout
    httpd_uri_t logout_uri = {
        .uri = "/logout",
        .method = HTTP_POST,
        .handler = web_logout_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(logout_uri);
    
    // ========================================================================
    // API Endpoints
    // ========================================================================
    
    // GET /api/health
    httpd_uri_t health_uri = {
        .uri = "/api/health",
        .method = HTTP_GET,
        .handler = health_get_handler,
        .user_ctx = NULL
    };
    REGISTER_URI_ESSENTIAL(health_uri);
    
    // GET /api/status
    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(status_uri);
    
    // GET /api/time
    httpd_uri_t time_uri = {
        .uri = "/api/time",
        .method = HTTP_GET,
        .handler = time_get_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(time_uri);
    
    // GET/POST /api/time/config
    httpd_uri_t time_config_get_uri = {
        .uri = "/api/time/config",
        .method = HTTP_GET,
        .handler = time_config_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(time_config_get_uri);
    
    httpd_uri_t time_config_post_uri = {
        .uri = "/api/time/config",
        .method = HTTP_POST,
        .handler = time_config_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(time_config_post_uri);
    
    // POST /api/time/sync
    httpd_uri_t time_sync_uri = {
        .uri = "/api/time/sync",
        .method = HTTP_POST,
        .handler = time_sync_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(time_sync_uri);
    
    // GET /api/wifi
    httpd_uri_t wifi_uri = {
        .uri = "/api/wifi",
        .method = HTTP_GET,
        .handler = wifi_get_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(wifi_uri);

    httpd_uri_t wifi_scan_uri = {
        .uri = "/api/wifi/scan",
        .method = HTTP_POST,
        .handler = wifi_scan_post_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(wifi_scan_uri);

    httpd_uri_t wifi_networks_uri = {
        .uri = "/api/wifi/networks",
        .method = HTTP_GET,
        .handler = wifi_networks_get_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(wifi_networks_uri);

    httpd_uri_t wifi_connect_uri = {
        .uri = "/api/wifi/connect",
        .method = HTTP_POST,
        .handler = wifi_connect_post_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(wifi_connect_uri);

    httpd_uri_t wifi_disconnect_uri = {
        .uri = "/api/wifi/disconnect",
        .method = HTTP_POST,
        .handler = wifi_disconnect_post_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(wifi_disconnect_uri);
    
    // GET /api/info
    httpd_uri_t info_uri = {
        .uri = "/api/info",
        .method = HTTP_GET,
        .handler = info_get_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(info_uri);
    
    // GET /api/ota/status
    httpd_uri_t ota_status_uri = {
        .uri = "/api/ota/status",
        .method = HTTP_GET,
        .handler = ota_status_get_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(ota_status_uri);
    
    // POST /api/ota/update
    httpd_uri_t ota_update_uri = {
        .uri = "/api/ota/update",
        .method = HTTP_POST,
        .handler = ota_update_post_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(ota_update_uri);
    
    // POST /api/ota/upload
    httpd_uri_t ota_upload_uri = {
        .uri = "/api/ota/upload",
        .method = HTTP_POST,
        .handler = ota_upload_post_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(ota_upload_uri);
    
    // POST /api/ota/reboot
    httpd_uri_t ota_reboot_uri = {
        .uri = "/api/ota/reboot",
        .method = HTTP_POST,
        .handler = ota_reboot_post_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(ota_reboot_uri);
    
    // GET /api/ota/releases - Check GitHub for updates
    httpd_uri_t ota_releases_uri = {
        .uri = "/api/ota/releases",
        .method = HTTP_GET,
        .handler = ota_releases_get_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(ota_releases_uri);
    
    // POST /api/ota/github - Start update from GitHub
    httpd_uri_t ota_github_uri = {
        .uri = "/api/ota/github",
        .method = HTTP_POST,
        .handler = ota_github_update_post_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(ota_github_uri);
    
    // GET /api/auth/config
    httpd_uri_t auth_config_get_uri = {
        .uri = "/api/auth/config",
        .method = HTTP_GET,
        .handler = auth_config_get_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(auth_config_get_uri);
    
    // POST /api/auth/config
    httpd_uri_t auth_config_post_uri = {
        .uri = "/api/auth/config",
        .method = HTTP_POST,
        .handler = auth_config_post_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(auth_config_post_uri);
    
    // GET /api/auth/status - Quick auth status check for web UI
    httpd_uri_t auth_status_uri = {
        .uri = "/api/auth/status",
        .method = HTTP_GET,
        .handler = auth_status_get_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(auth_status_uri);
    
    // POST /api/auth/credentials
    httpd_uri_t auth_credentials_uri = {
        .uri = "/api/auth/credentials",
        .method = HTTP_POST,
        .handler = auth_credentials_post_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(auth_credentials_uri);
    
    // GET /api/auth/apikey
    httpd_uri_t auth_apikey_uri = {
        .uri = "/api/auth/apikey",
        .method = HTTP_GET,
        .handler = auth_apikey_get_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(auth_apikey_uri);
    
    // POST /api/auth/apikey/regenerate
    httpd_uri_t auth_apikey_regen_uri = {
        .uri = "/api/auth/apikey/regenerate",
        .method = HTTP_POST,
        .handler = auth_apikey_regenerate_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(auth_apikey_regen_uri);
    
    // GET /api/heatpump/status
    httpd_uri_t heatpump_uri = {
        .uri = "/api/heatpump/status",
        .method = HTTP_GET,
        .handler = heatpump_status_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(heatpump_uri);
    
    // GET /api/heatpump/raw - debug raw register cache dump
    httpd_uri_t heatpump_raw_uri = {
        .uri = "/api/heatpump/raw",
        .method = HTTP_GET,
        .handler = heatpump_raw_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(heatpump_raw_uri);

    // GET /api/heatpump/windows - debug observed Tuya window catalog
    httpd_uri_t heatpump_windows_uri = {
        .uri = "/api/heatpump/windows",
        .method = HTTP_GET,
        .handler = heatpump_windows_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(heatpump_windows_uri);
    
    // GET /api/heatpump/advanced - List all advanced (AP) parameters
    httpd_uri_t heatpump_advanced_uri = {
        .uri = "/api/heatpump/advanced",
        .method = HTTP_GET,
        .handler = heatpump_advanced_get_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(heatpump_advanced_uri);
    
    // GET /api/heatpump/advanced/* - Get single AP parameter (wildcard)
    httpd_uri_t heatpump_advanced_get_uri = {
        .uri = "/api/heatpump/advanced/*",
        .method = HTTP_GET,
        .handler = heatpump_advanced_single_get_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(heatpump_advanced_get_uri);
    
    // PUT /api/heatpump/advanced/* - Set single AP parameter (wildcard)
    httpd_uri_t heatpump_advanced_put_uri = {
        .uri = "/api/heatpump/advanced/*",
        .method = HTTP_PUT,
        .handler = heatpump_advanced_put_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(heatpump_advanced_put_uri);
    
    // PUT /api/heatpump/power - Set power on/off
    httpd_uri_t heatpump_power_uri = {
        .uri = "/api/heatpump/power",
        .method = HTTP_PUT,
        .handler = heatpump_power_put_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(heatpump_power_uri);
    
    // PUT /api/heatpump/mode - Set operating mode
    httpd_uri_t heatpump_mode_uri = {
        .uri = "/api/heatpump/mode",
        .method = HTTP_PUT,
        .handler = heatpump_mode_put_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(heatpump_mode_uri);
    
    // PUT /api/heatpump/setpoints - Set temperature setpoints
    httpd_uri_t heatpump_setpoints_uri = {
        .uri = "/api/heatpump/setpoints",
        .method = HTTP_PUT,
        .handler = heatpump_setpoints_put_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(heatpump_setpoints_uri);
    
    // GET /api/heatpump/errors - Get active errors and history
    httpd_uri_t heatpump_errors_uri = {
        .uri = "/api/heatpump/errors",
        .method = HTTP_GET,
        .handler = heatpump_errors_get_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(heatpump_errors_uri);
    
    // DELETE /api/heatpump/errors/history - Clear error history (keeps active errors)
    httpd_uri_t heatpump_errors_clear_uri = {
        .uri = "/api/heatpump/errors/history",
        .method = HTTP_DELETE,
        .handler = heatpump_errors_clear_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(heatpump_errors_clear_uri);
    
    // GET /api/heatpump/diagnostic - Download diagnostic CSV dump
    httpd_uri_t heatpump_diagnostic_uri = {
        .uri = "/api/heatpump/diagnostic",
        .method = HTTP_GET,
        .handler = heatpump_diagnostic_get_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(heatpump_diagnostic_uri);

    // PATCH /api/heatpump/demo - Write fields in demo mode (for testing)
    httpd_uri_t heatpump_demo_uri = {
        .uri = "/api/heatpump/demo",
        .method = HTTP_PATCH,
        .handler = heatpump_demo_patch_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(heatpump_demo_uri);
    
    // GET /api/events - Get event log
    httpd_uri_t events_get_uri = {
        .uri = "/api/events",
        .method = HTTP_GET,
        .handler = events_get_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(events_get_uri);
    
    // DELETE /api/events - Clear event log
    httpd_uri_t events_clear_uri = {
        .uri = "/api/events",
        .method = HTTP_DELETE,
        .handler = events_clear_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(events_clear_uri);

    // POST /api/brownout/clear - Reset the persistent brownout counter
    httpd_uri_t brownout_clear_uri = {
        .uri = "/api/brownout/clear",
        .method = HTTP_POST,
        .handler = brownout_clear_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(brownout_clear_uri);

    // GET /api/display/brightness - Get current display brightness
    httpd_uri_t display_brightness_uri = {
        .uri = "/api/display/brightness",
        .method = HTTP_GET,
        .handler = display_brightness_get_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(display_brightness_uri);

    httpd_uri_t display_brightness_put_uri = {
        .uri = "/api/display/brightness",
        .method = HTTP_PUT,
        .handler = display_brightness_put_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(display_brightness_put_uri);

    // GET /api/preferences - Get current app preferences
    httpd_uri_t preferences_uri = {
        .uri = "/api/preferences",
        .method = HTTP_GET,
        .handler = preferences_get_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(preferences_uri);

    httpd_uri_t preferences_patch_uri = {
        .uri = "/api/preferences",
        .method = HTTP_PATCH,
        .handler = preferences_patch_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(preferences_patch_uri);

    httpd_uri_t factory_reset_uri = {
        .uri = "/api/factory-reset",
        .method = HTTP_POST,
        .handler = factory_reset_post_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(factory_reset_uri);

    // GET /api/logs - Get log buffer entries
    httpd_uri_t logs_get_uri = {
        .uri = "/api/logs",
        .method = HTTP_GET,
        .handler = logs_get_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(logs_get_uri);

    // DELETE /api/logs - Clear log buffer
    httpd_uri_t logs_clear_uri = {
        .uri = "/api/logs",
        .method = HTTP_DELETE,
        .handler = logs_clear_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(logs_clear_uri);

    // GET /api/screenshot - Capture live screen as uncompressed PNG
    httpd_uri_t screenshot_uri = {
        .uri = "/api/screenshot",
        .method = HTTP_GET,
        .handler = screenshot_get_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(screenshot_uri);

    // ========================================================================
    // TLS Certificate Management
    // ========================================================================
    
    // GET /api/tls/status
    httpd_uri_t tls_status_uri = {
        .uri = "/api/tls/status",
        .method = HTTP_GET,
        .handler = tls_status_get_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(tls_status_uri);

    // POST /api/tls/certificate
    httpd_uri_t tls_cert_post_uri = {
        .uri = "/api/tls/certificate",
        .method = HTTP_POST,
        .handler = tls_cert_post_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(tls_cert_post_uri);

    // DELETE /api/tls/certificate
    httpd_uri_t tls_cert_delete_uri = {
        .uri = "/api/tls/certificate",
        .method = HTTP_DELETE,
        .handler = tls_cert_delete_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(tls_cert_delete_uri);

    // GET /api/ha/status - Home Assistant pairing status for the web UI
    httpd_uri_t ha_manage_status_uri = {
        .uri = "/api/ha/status",
        .method = HTTP_GET,
        .handler = ha_manage_status_get_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(ha_manage_status_uri);

    // POST /api/ha/pair - Open a pairing window (code shown on device screen)
    httpd_uri_t ha_manage_pair_uri = {
        .uri = "/api/ha/pair",
        .method = HTTP_POST,
        .handler = ha_manage_pair_post_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(ha_manage_pair_uri);

    // DELETE /api/ha/pair - Cancel the active pairing window
    httpd_uri_t ha_manage_pair_cancel_uri = {
        .uri = "/api/ha/pair",
        .method = HTTP_DELETE,
        .handler = ha_manage_pair_cancel_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(ha_manage_pair_cancel_uri);

    // POST /api/ha/revoke - Revoke the Home Assistant integration token
    httpd_uri_t ha_manage_revoke_uri = {
        .uri = "/api/ha/revoke",
        .method = HTTP_POST,
        .handler = ha_manage_revoke_post_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(ha_manage_revoke_uri);

#ifdef CONFIG_TEST_ENDPOINTS
    // Test endpoints need HTTP for CI (no TLS on test runner)
    test_endpoints_register(server);
    if (server_ssl != NULL) {
        test_endpoints_register(server_ssl);
    }
    ESP_LOGI(TAG, "Test instrumentation endpoints enabled");

    httpd_config_t websocket_config = HTTPD_DEFAULT_CONFIG();
    websocket_config.server_port = 81;
    websocket_config.ctrl_port = 32770;
    websocket_config.max_uri_handlers = 1;
    websocket_config.max_open_sockets = 2;
    websocket_config.stack_size = 8192;
    websocket_config.lru_purge_enable = true;
    websocket_config.recv_wait_timeout = 10;
    websocket_config.send_wait_timeout = 1;
    ret = httpd_start(&websocket_test_server, &websocket_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start isolated WebSocket test server: %s",
                 esp_err_to_name(ret));
        api_server_stop();
        return false;
    }
    test_endpoints_register_websocket(websocket_test_server);
    ESP_LOGI(TAG, "WebSocket feasibility server started on port 81");
#endif

    if (server_integration != NULL) {
        httpd_uri_t ha_pair_uri = {
            .uri = "/api/v1/pair",
            .method = HTTP_POST,
            .handler = ha_pair_post_handler,
            .user_ctx = NULL
        };
        ret = httpd_register_uri_handler(server_integration, &ha_pair_uri);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register HA pairing: %s",
                     esp_err_to_name(ret));
            api_server_stop();
            return false;
        }

        httpd_uri_t ha_capabilities_uri = {
            .uri = "/api/v1/capabilities",
            .method = HTTP_GET,
            .handler = ha_capabilities_get_handler,
            .user_ctx = NULL
        };
        ret = httpd_register_uri_handler(
            server_integration, &ha_capabilities_uri);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register HA capabilities: %s",
                     esp_err_to_name(ret));
            api_server_stop();
            return false;
        }

        httpd_uri_t ha_state_uri = {
            .uri = "/api/v1/state",
            .method = HTTP_GET,
            .handler = ha_state_get_handler,
            .user_ctx = NULL
        };
        ret = httpd_register_uri_handler(server_integration, &ha_state_uri);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register HA state: %s",
                     esp_err_to_name(ret));
            api_server_stop();
            return false;
        }

        httpd_uri_t ha_events_uri = {
            .uri = "/api/v1/events",
            .method = HTTP_GET,
            .handler = ha_websocket_handler,
            .user_ctx = NULL,
            .is_websocket = true,
            .handle_ws_control_frames = true,
            .supported_subprotocol = NULL,
            .ws_pre_handshake_cb = ha_websocket_pre_handshake,
        };
        ret = httpd_register_uri_handler(
            server_integration, &ha_events_uri);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register HA WebSocket: %s",
                     esp_err_to_name(ret));
            api_server_stop();
            return false;
        }
        if (!ha_websocket_start(server_integration)) {
            ESP_LOGE(TAG, "Home Assistant WebSocket push unavailable");
            httpd_ssl_stop(server_integration);
            server_integration = NULL;
        } else {
            httpd_uri_t ha_power_uri = {
                .uri = "/api/v1/control/power",
                .method = HTTP_PUT,
                .handler = ha_power_put_handler,
                .user_ctx = NULL
            };
            ret = httpd_register_uri_handler(
                server_integration, &ha_power_uri);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to register HA power control: %s",
                         esp_err_to_name(ret));
                api_server_stop();
                return false;
            }

            httpd_uri_t ha_mode_uri = {
                .uri = "/api/v1/control/mode",
                .method = HTTP_PUT,
                .handler = ha_mode_put_handler,
                .user_ctx = NULL
            };
            ret = httpd_register_uri_handler(
                server_integration, &ha_mode_uri);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to register HA mode control: %s",
                         esp_err_to_name(ret));
                api_server_stop();
                return false;
            }

            httpd_uri_t ha_setpoint_uri = {
                .uri = "/api/v1/control/setpoint",
                .method = HTTP_PUT,
                .handler = ha_setpoint_put_handler,
                .user_ctx = NULL
            };
            ret = httpd_register_uri_handler(
                server_integration, &ha_setpoint_uri);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to register HA setpoint control: %s",
                         esp_err_to_name(ret));
                api_server_stop();
                return false;
            }
        }
    }

    if (server_integration != NULL) {
        ESP_LOGI(TAG, "Home Assistant REST foundation enabled");

        mdns_txt_item_t integration_txt[] = {
            {"device", "arctic-controller"},
            {"id", arctic::ha::deviceId()},
            {"version", "1"},
            {"tls", "1"},
            {"push", "1"},
        };
        ret = mdns_service_add(
            "Arctic Home Assistant", "_arctic", "_tcp", 8443,
            integration_txt, sizeof(integration_txt) / sizeof(integration_txt[0]));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "HA mDNS service add failed: %s",
                     esp_err_to_name(ret));
        }
    } else {
        ESP_LOGE(TAG, "Home Assistant REST disabled: no identity server");
    }
    
    #undef REGISTER_URI
    #undef REGISTER_URI_ESSENTIAL

    // With mandatory HTTPS active, port 80 only serves the essential bootstrap
    // routes; any other path (e.g. a browser opening http://<device>) would
    // otherwise hit the bare "Nothing matches the given URI" 404. Register a
    // 404 handler on the HTTP server that returns a clean "use HTTPS" notice
    // instead. It does NOT redirect: mandatory HTTPS means port 80 must stay a
    // dead end so secret-bearing requests are never normalised onto plaintext
    // HTTP (see tests/api/test_ha_security_contract.py).
    if (server != NULL && server_ssl != NULL) {
        esp_err_t rerr = httpd_register_err_handler(
            server, HTTPD_404_NOT_FOUND, http_https_required_handler);
        if (rerr != ESP_OK) {
            ESP_LOGW(TAG, "Failed to register HTTP 404 handler: %s",
                     esp_err_to_name(rerr));
        }
    }

    ESP_LOGI(TAG, "HTTP server started on port 80");
    if (server_ssl != NULL) {
        ESP_LOGI(TAG, "HTTPS server started on port 443");
        ESP_LOGI(TAG, "Web UI: https://%s.local/", hostname);
    } else {
        ESP_LOGI(TAG, "Web UI: http://%s.local/", hostname);
    }
    
    return true;
}

void api_server_stop(void)
{
#ifdef CONFIG_TEST_ENDPOINTS
    if (websocket_test_server != NULL) {
        ESP_LOGI(TAG, "Stopping WebSocket feasibility server...");
        httpd_stop(websocket_test_server);
        websocket_test_server = NULL;
    }
#endif
    if (server_integration != NULL) {
        ha_websocket_stop();
        ESP_LOGI(TAG, "Stopping integration HTTPS server...");
        httpd_ssl_stop(server_integration);
        server_integration = NULL;
    }
    if (server_ssl != NULL) {
        ESP_LOGI(TAG, "Stopping HTTPS server...");
        httpd_ssl_stop(server_ssl);
        server_ssl = NULL;
        tls_mgr_set_https_active(false);
    }
    if (server != NULL) {
        ESP_LOGI(TAG, "Stopping HTTP server...");
        httpd_stop(server);
        server = NULL;
    }
}

bool api_server_is_running(void)
{
    return server != NULL;
}

// ============================================================================
// Helper Functions
// ============================================================================

static void set_json_content_type(httpd_req_t* req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

static void send_json_error(httpd_req_t* req, const char* status, const char* message)
{
    httpd_resp_set_status(req, status);
    set_json_content_type(req);
    
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", message);
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);
}

// ============================================================================
// Web Interface Handlers
// ============================================================================

// 404 handler for the plain-HTTP server (port 80). Because mandatory HTTPS is
// active, all normal routes are registered on the HTTPS server only, so any
// non-essential HTTP request lands here. Return a clean 404 with a short
// plain-text notice pointing at the device's HTTPS URL instead of the default
// esp_http_server "Nothing matches the given URI" body.
//
// This intentionally does NOT redirect (no Location header / 3xx): under
// mandatory HTTPS, port 80 must stay a dead end so clients are never
// encouraged to send secret-bearing requests (X-API-Key / bearer tokens) over
// plaintext HTTP. The message uses the device's own advertised hostname rather
// than reflecting the client-supplied Host header.
static esp_err_t http_https_required_handler(httpd_req_t* req,
                                             httpd_err_code_t err)
{
    (void)err;

    char body[128];
    snprintf(body, sizeof(body),
             "This controller requires a secure connection.\n"
             "Use https://%s.local/\n", hostname);

    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t web_root_handler(httpd_req_t* req)
{
    // Check auth - if enabled and not authenticated, show login form
    if (auth_mgr_web_auth_enabled() && !check_web_auth(req)) {
        // Serve the page anyway - the JavaScript will handle showing login
        // We just won't allow API calls without auth
    }
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    
    size_t html_len = index_html_gz_end - index_html_gz_start;
    httpd_resp_send(req, (const char*)index_html_gz_start, html_len);
    
    return ESP_OK;
}

static esp_err_t web_login_handler(httpd_req_t* req)
{
    set_json_content_type(req);
    
    // Read body
    char content[256];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        send_json_error(req, "400 Bad Request", "No body");
        return ESP_OK;
    }
    content[ret] = '\0';
    
    // Parse JSON
    cJSON* root = cJSON_Parse(content);
    if (!root) {
        send_json_error(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }
    
    cJSON* username_json = cJSON_GetObjectItem(root, "username");
    cJSON* password_json = cJSON_GetObjectItem(root, "password");
    
    if (!username_json || !password_json) {
        cJSON_Delete(root);
        send_json_error(req, "400 Bad Request", "Missing username or password");
        return ESP_OK;
    }
    
    char session_token[AUTH_SESSION_TOKEN_LEN + 1] = {0};
    bool success = auth_mgr_login(
        username_json->valuestring,
        password_json->valuestring,
        session_token
    );
    
    cJSON_Delete(root);
    
    if (!success) {
        send_json_error(req, "401 Unauthorized", "Invalid credentials");
        return ESP_OK;
    }
    
    // Set session cookie
    set_session_cookie(req, session_token);
    
    cJSON* response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    char* json_str = cJSON_PrintUnformatted(response);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(response);
    
    return ESP_OK;
}

static esp_err_t web_logout_handler(httpd_req_t* req)
{
    set_json_content_type(req);
    
    // Get session token from cookie and invalidate it
    char token[AUTH_SESSION_TOKEN_LEN + 1] = {0};
    if (get_session_from_cookie(req, token)) {
        auth_mgr_logout(token);
    }
    
    // Clear the cookie
    clear_session_cookie(req);
    
    cJSON* response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    char* json_str = cJSON_PrintUnformatted(response);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(response);
    
    return ESP_OK;
}

static esp_err_t favicon_handler(httpd_req_t* req)
{
    // Return 204 No Content - no favicon available
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ============================================================================
// API Handlers
// ============================================================================

static esp_err_t health_get_handler(httpd_req_t* req)
{
    set_json_content_type(req);
    
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddNumberToObject(root, "uptime_ms", (double)xTaskGetTickCount() * portTICK_PERIOD_MS);
    
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

static esp_err_t status_get_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    
    set_json_content_type(req);
    
    cJSON* root = cJSON_CreateObject();
    
    // Device info
    cJSON_AddStringToObject(root, "device", "Arctic Heat Pump Controller");
    cJSON_AddStringToObject(root, "hostname", hostname);
    cJSON_AddNumberToObject(root, "uptime_ms", (double)xTaskGetTickCount() * portTICK_PERIOD_MS);
    
    // WiFi status
    cJSON* wifi = cJSON_AddObjectToObject(root, "wifi");
    wifi_mgr_state_t wifi_state = wifi_mgr_get_state();
    const char* wifi_state_str = "unknown";
    switch (wifi_state) {
        case WIFI_MGR_STATE_NOT_INITIALIZED: wifi_state_str = "not_initialized"; break;
        case WIFI_MGR_STATE_DISCONNECTED: wifi_state_str = "disconnected"; break;
        case WIFI_MGR_STATE_CONNECTING: wifi_state_str = "connecting"; break;
        case WIFI_MGR_STATE_CONNECTED: wifi_state_str = "connected"; break;
        case WIFI_MGR_STATE_ERROR: wifi_state_str = "error"; break;
    }
    cJSON_AddStringToObject(wifi, "state", wifi_state_str);
    if (wifi_state == WIFI_MGR_STATE_CONNECTED) {
        cJSON_AddStringToObject(wifi, "ssid", wifi_mgr_get_connected_ssid());
        char ip[16];
        if (wifi_mgr_get_ip_addr(ip, sizeof(ip))) {
            cJSON_AddStringToObject(wifi, "ip", ip);
        }
        cJSON_AddNumberToObject(wifi, "rssi", wifi_mgr_get_rssi());
    }
    
    // Time status
    cJSON* time_obj = cJSON_AddObjectToObject(root, "time");
    char time_str[32];
    time_mgr_get_time_str(time_str, sizeof(time_str), "%H:%M:%S");
    cJSON_AddStringToObject(time_obj, "local", time_str);
    cJSON_AddStringToObject(time_obj, "timezone", time_mgr_get_timezone());
    cJSON_AddBoolToObject(time_obj, "synced", time_mgr_is_synced());
    
    time_t now;
    time(&now);
    cJSON_AddNumberToObject(time_obj, "epoch", (double)now);
    
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

static esp_err_t time_get_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    
    set_json_content_type(req);
    
    cJSON* root = cJSON_CreateObject();
    
    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    
    char time_str[32];
    time_mgr_get_time_str(time_str, sizeof(time_str), "%H:%M:%S");
    cJSON_AddStringToObject(root, "time_24h", time_str);
    
    time_mgr_get_time_str(time_str, sizeof(time_str), "%I:%M:%S %p");
    cJSON_AddStringToObject(root, "time_12h", time_str);
    
    char date_str[32];
    strftime(date_str, sizeof(date_str), "%Y-%m-%d", &timeinfo);
    cJSON_AddStringToObject(root, "date", date_str);
    
    char iso_str[32];
    strftime(iso_str, sizeof(iso_str), "%Y-%m-%dT%H:%M:%S", &timeinfo);
    cJSON_AddStringToObject(root, "iso", iso_str);
    
    cJSON_AddStringToObject(root, "timezone", time_mgr_get_timezone());
    cJSON_AddNumberToObject(root, "epoch", (double)now);
    cJSON_AddBoolToObject(root, "ntp_synced", time_mgr_is_synced());
    
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

static esp_err_t time_config_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    
    set_json_content_type(req);
    
    if (req->method == HTTP_GET) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "timezone", time_mgr_get_timezone());
        cJSON_AddBoolToObject(root, "format_24h", time_mgr_get_24h_format());
        cJSON_AddBoolToObject(root, "synced", time_mgr_is_synced());
        
        char* json_str = cJSON_PrintUnformatted(root);
        httpd_resp_sendstr(req, json_str);
        free(json_str);
        cJSON_Delete(root);
    } else {
        // POST - update config
        char content[256];
        int ret = httpd_req_recv(req, content, sizeof(content) - 1);
        if (ret <= 0) {
            send_json_error(req, "400 Bad Request", "No body");
            return ESP_OK;
        }
        content[ret] = '\0';
        
        cJSON* root = cJSON_Parse(content);
        if (!root) {
            send_json_error(req, "400 Bad Request", "Invalid JSON");
            return ESP_OK;
        }
        
        cJSON* tz_json = cJSON_GetObjectItem(root, "timezone");
        if (tz_json && cJSON_IsString(tz_json)) {
            time_mgr_set_timezone(tz_json->valuestring);
        }
        
        cJSON* format_json = cJSON_GetObjectItem(root, "format_24h");
        if (format_json && cJSON_IsBool(format_json)) {
            time_mgr_set_24h_format(cJSON_IsTrue(format_json));
        }
        
        cJSON_Delete(root);
        
        cJSON* response = cJSON_CreateObject();
        cJSON_AddBoolToObject(response, "success", true);
        char* json_str = cJSON_PrintUnformatted(response);
        httpd_resp_sendstr(req, json_str);
        free(json_str);
        cJSON_Delete(response);
    }
    
    return ESP_OK;
}

static esp_err_t time_sync_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    
    set_json_content_type(req);
    
    time_mgr_force_sync();
    
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "message", "NTP sync initiated");
    
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

static esp_err_t wifi_get_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    
    set_json_content_type(req);
    
    cJSON* root = cJSON_CreateObject();
    
    wifi_mgr_state_t state = wifi_mgr_get_state();
    const char* state_str = "unknown";
    switch (state) {
        case WIFI_MGR_STATE_NOT_INITIALIZED: state_str = "not_initialized"; break;
        case WIFI_MGR_STATE_DISCONNECTED: state_str = "disconnected"; break;
        case WIFI_MGR_STATE_CONNECTING: state_str = "connecting"; break;
        case WIFI_MGR_STATE_CONNECTED: state_str = "connected"; break;
        case WIFI_MGR_STATE_ERROR: state_str = "error"; break;
    }
    cJSON_AddStringToObject(root, "state", state_str);
    cJSON_AddBoolToObject(root, "connected", state == WIFI_MGR_STATE_CONNECTED);
    
    if (state == WIFI_MGR_STATE_CONNECTED) {
        cJSON_AddStringToObject(root, "ssid", wifi_mgr_get_connected_ssid());
        char ip[16];
        if (wifi_mgr_get_ip_addr(ip, sizeof(ip))) {
            cJSON_AddStringToObject(root, "ip", ip);
        }
        cJSON_AddNumberToObject(root, "rssi", wifi_mgr_get_rssi());
        cJSON_AddStringToObject(root, "hostname", hostname);
        
        char url[64];
        snprintf(url, sizeof(url), "%s://%s.local",
                 tls_mgr_is_https_active() ? "https" : "http", hostname);
        cJSON_AddStringToObject(root, "local_url", url);
        
        // Get MAC address
        uint8_t mac[6];
        if (wifi_mgr_get_mac_addr(mac)) {
            char mac_str[18];
            snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            cJSON_AddStringToObject(root, "mac", mac_str);
        }
    }
    
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

static volatile bool s_wifi_scan_in_progress = false;

static void wifi_scan_done_callback(const wifi_mgr_ap_info_t* ap_list, uint16_t count)
{
    (void)ap_list;
    ESP_LOGI(TAG, "WiFi API scan completed with %u network(s)", count);
    s_wifi_scan_in_progress = false;
}

static esp_err_t wifi_scan_post_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    if (s_wifi_scan_in_progress) {
        send_json_error(req, "409 Conflict", "WiFi scan already in progress");
        return ESP_OK;
    }

    s_wifi_scan_in_progress = true;
    if (!wifi_mgr_start_scan(wifi_scan_done_callback)) {
        s_wifi_scan_in_progress = false;
        send_json_error(req, "503 Service Unavailable", "Unable to start WiFi scan");
        return ESP_OK;
    }

    set_json_content_type(req);
    httpd_resp_sendstr(req, "{\"success\":true,\"scanning\":true}");
    return ESP_OK;
}

static esp_err_t wifi_networks_get_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }

    wifi_mgr_ap_info_t networks[32];
    uint16_t count = wifi_mgr_get_scan_results(networks, 32);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "scanning", s_wifi_scan_in_progress);
    cJSON* arr = cJSON_AddArrayToObject(root, "networks");
    for (uint16_t i = 0; i < count; i++) {
        cJSON* network = cJSON_CreateObject();
        cJSON_AddStringToObject(network, "ssid", networks[i].ssid);
        cJSON_AddNumberToObject(network, "rssi", networks[i].rssi);
        cJSON_AddNumberToObject(network, "authmode", networks[i].authmode);
        cJSON_AddItemToArray(arr, network);
    }

    set_json_content_type(req);
    char* json = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

typedef struct {
    char ssid[33];
    char password[65];
} wifi_connect_request_t;

static void wifi_connect_task(void* arg)
{
    wifi_connect_request_t* request = (wifi_connect_request_t*)arg;
    vTaskDelay(pdMS_TO_TICKS(250));
    bool saved = wifi_mgr_save_credentials(request->ssid, request->password);
    bool started = saved && wifi_mgr_connect(request->ssid, request->password, NULL);
    if (!started) {
        ESP_LOGE(TAG, "Deferred WiFi connection to '%s' failed to start", request->ssid);
    }
    free(request);
    vTaskDelete(NULL);
}

static esp_err_t wifi_connect_post_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }

    char body[256];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        send_json_error(req, "400 Bad Request", "Empty request body");
        return ESP_OK;
    }
    body[received] = '\0';

    cJSON* root = cJSON_Parse(body);
    cJSON* ssid = root ? cJSON_GetObjectItem(root, "ssid") : NULL;
    cJSON* password = root ? cJSON_GetObjectItem(root, "password") : NULL;
    if (!cJSON_IsString(ssid) || ssid->valuestring[0] == '\0' ||
        strlen(ssid->valuestring) > 32 ||
        (password && (!cJSON_IsString(password) || strlen(password->valuestring) > 64))) {
        cJSON_Delete(root);
        send_json_error(req, "400 Bad Request", "Invalid SSID or password");
        return ESP_OK;
    }

    wifi_connect_request_t* request =
        (wifi_connect_request_t*)calloc(1, sizeof(wifi_connect_request_t));
    if (!request) {
        cJSON_Delete(root);
        send_json_error(req, "500 Internal Server Error", "Out of memory");
        return ESP_OK;
    }
    strncpy(request->ssid, ssid->valuestring, sizeof(request->ssid) - 1);
    if (password) {
        strncpy(request->password, password->valuestring, sizeof(request->password) - 1);
    }
    cJSON_Delete(root);

    if (xTaskCreate(wifi_connect_task, "wifi_api_connect", 4096, request, 5, NULL) != pdPASS) {
        free(request);
        send_json_error(req, "500 Internal Server Error", "Unable to start WiFi connection");
        return ESP_OK;
    }

    set_json_content_type(req);
    httpd_resp_sendstr(req, "{\"success\":true,\"state\":\"connecting\"}");
    return ESP_OK;
}

static void wifi_disconnect_task(void* arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(250));
    wifi_mgr_disconnect();
    vTaskDelete(NULL);
}

static esp_err_t wifi_disconnect_post_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    if (xTaskCreate(wifi_disconnect_task, "wifi_api_disconnect", 3072, NULL, 5, NULL) != pdPASS) {
        send_json_error(req, "500 Internal Server Error", "Unable to start WiFi disconnect");
        return ESP_OK;
    }
    set_json_content_type(req);
    httpd_resp_sendstr(req, "{\"success\":true,\"state\":\"disconnecting\"}");
    return ESP_OK;
}

static esp_err_t send_integration_document(
    httpd_req_t* req, cJSON* document)
{
    if (document == NULL) {
        send_json_error(
            req, "500 Internal Server Error", "State serialization failed");
        return ESP_OK;
    }

    set_json_content_type(req);
    char* json = cJSON_PrintUnformatted(document);
    cJSON_Delete(document);
    if (json == NULL) {
        send_json_error(
            req, "500 Internal Server Error", "State serialization failed");
        return ESP_OK;
    }
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return ESP_OK;
}

static bool read_integration_body(
        httpd_req_t* req, char* buffer, size_t capacity)
    {
        if (buffer == nullptr || capacity < 2 ||
            req->content_len <= 0 ||
            static_cast<size_t>(req->content_len) >= capacity) {
            return false;
        }

        size_t received = 0;
        while (received < static_cast<size_t>(req->content_len)) {
            const int count = httpd_req_recv(
                req, buffer + received,
                static_cast<size_t>(req->content_len) - received);
            if (count <= 0) {
                return false;
            }
            received += static_cast<size_t>(count);
        }
        buffer[received] = '\0';
        return true;
    }

    static bool integration_object_has_only_keys(
        const cJSON* object, const char* const* keys, size_t key_count)
    {
        if (object == nullptr || !cJSON_IsObject(object)) {
            return false;
        }
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, object) {
            bool known = false;
            for (size_t i = 0; i < key_count; ++i) {
                if (strcmp(item->string, keys[i]) == 0) {
                    known = true;
                    break;
                }
            }
            if (!known) {
                return false;
            }

        }
        return true;
    }

    static bool integration_command_id(
        const cJSON* root, char* command_id)
    {
        cJSON* value = cJSON_GetObjectItemCaseSensitive(root, "command_id");
        if (value == nullptr || !cJSON_IsString(value) ||
            value->valuestring == nullptr) {
            return false;
        }
        const size_t length = strlen(value->valuestring);
        if (length == 0 || length > HA_COMMAND_ID_MAX) {
            return false;
        }
        for (size_t i = 0; i < length; ++i) {
            const unsigned char c =
                static_cast<unsigned char>(value->valuestring[i]);
            if (c < 0x21 || c > 0x7e) {
                return false;
            }
        }
        memcpy(command_id, value->valuestring, length + 1);
        return true;
    }

    enum class HaCommandLookup {
        New,
        Duplicate,
        Conflict,
    };

    static HaCommandLookup ha_command_begin(
        const char* command_id, const char* operation, const char* payload,
        size_t* slot_out)
    {
        if (s_ha_command_mutex == nullptr ||
            xSemaphoreTake(s_ha_command_mutex, portMAX_DELAY) != pdTRUE) {
            return HaCommandLookup::Conflict;
        }

        for (size_t i = 0; i < HA_COMMAND_CACHE_SIZE; ++i) {
            const HaCommandRecord& record = s_ha_command_cache[i];
            if (!record.accepted || strcmp(record.command_id, command_id) != 0) {
                continue;
            }
            if (strcmp(record.operation, operation) == 0 &&
                strcmp(record.payload, payload) == 0) {
                if (slot_out != nullptr) {
                    *slot_out = i;
                }
                return HaCommandLookup::Duplicate;
            }
            xSemaphoreGive(s_ha_command_mutex);
            return HaCommandLookup::Conflict;
        }

        const size_t slot = s_ha_command_next++ % HA_COMMAND_CACHE_SIZE;
        s_ha_command_cache[slot] = {};
        if (slot_out != nullptr) {
            *slot_out = slot;
        }
        return HaCommandLookup::New;
    }

    static void ha_command_finish(
        size_t slot, bool accepted, const char* command_id,
        const char* operation, const char* payload)
    {
        if (s_ha_command_mutex == nullptr) {
            return;
        }
        if (accepted) {
            HaCommandRecord& record = s_ha_command_cache[slot];
            record.accepted = true;
            snprintf(record.command_id, sizeof(record.command_id), "%s", command_id);
            snprintf(record.operation, sizeof(record.operation), "%s", operation);
            snprintf(record.payload, sizeof(record.payload), "%s", payload);
        }
        xSemaphoreGive(s_ha_command_mutex);
    }

    static esp_err_t send_accepted_command(
        httpd_req_t* req, const char* command_id)
    {
        httpd_resp_set_status(req, "202 Accepted");
        set_json_content_type(req);
        cJSON* response = cJSON_CreateObject();
        cJSON_AddBoolToObject(response, "accepted", true);
        cJSON_AddStringToObject(response, "command_id", command_id);
        cJSON_AddStringToObject(
            response, "status", "accepted_waiting_for_reported_state");
        char* json = cJSON_PrintUnformatted(response);
        cJSON_Delete(response);
        if (json == nullptr) {
            send_json_error(
                req, "500 Internal Server Error", "Command response failed");
            return ESP_OK;
        }
        httpd_resp_sendstr(req, json);
        cJSON_free(json);
        return ESP_OK;
    }

    static bool ha_supports_power_or_mode()
    {
        const arctic::HeatPumpState state = arctic::getState();
        return state.connected && arctic::isDemoMode();
    }

    static bool ha_supports_setpoint(const char* kind)
    {
        const arctic::HeatPumpState state = arctic::getState();
        if (!state.connected) {
            return false;
        }
        if (arctic::isDemoMode()) {
            return strcmp(kind, "cooling") == 0 ||
                   strcmp(kind, "heating") == 0 ||
                   strcmp(kind, "hot_water") == 0;
        }
        if (macon_master::is_active()) {
            return strcmp(kind, "cooling") == 0 ||
                   strcmp(kind, "hot_water") == 0;
        }
        return false;
    }

    static const char* ha_control_unavailable_message(
        const char* operation, const char* kind = nullptr)
    {
        const arctic::HeatPumpState state = arctic::getState();
        if (!state.connected) {
            return "Heat pump not connected";
        }
        if (kind != nullptr && strcmp(kind, "heating") == 0) {
            return "Heating setpoint is unsupported by the active Tuya runtime";
        }
        if (strcmp(operation, "power") == 0) {
            return "Power control is unsupported by the active runtime";
        }
        if (strcmp(operation, "mode") == 0) {
            return "Selected-mode control is unsupported by the active runtime";
        }
        return "Setpoint control is unsupported by the active runtime";
    }

static esp_err_t ha_pair_post_handler(httpd_req_t* req)
{
    if (req->content_len <= 0 || req->content_len >= 64) {
        send_json_error(req, "400 Bad Request", "Pairing code required");
        return ESP_OK;
    }

    char body[64];
    int received = 0;
    while (received < req->content_len) {
        const int result = httpd_req_recv(
            req, body + received, req->content_len - received);
        if (result <= 0) {
            send_json_error(req, "400 Bad Request", "Could not read body");
            return ESP_OK;
        }
        received += result;
    }
    body[received] = '\0';

    cJSON* root = cJSON_Parse(body);
    memset(body, 0, sizeof(body));
    if (root == NULL) {
        send_json_error(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }

    cJSON* code_json = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (!cJSON_IsString(code_json) || code_json->valuestring == NULL) {
        cJSON_Delete(root);
        send_json_error(req, "400 Bad Request", "Pairing code required");
        return ESP_OK;
    }

    char code[SETUP_PAIRING_CODE_LEN + 1] = {};
    if (strlen(code_json->valuestring) != SETUP_PAIRING_CODE_LEN) {
        cJSON_Delete(root);
        send_json_error(
            req, "400 Bad Request", "Pairing code must be six digits");
        return ESP_OK;
    }
    memcpy(code, code_json->valuestring, SETUP_PAIRING_CODE_LEN);
    memset(
        code_json->valuestring, 0, strlen(code_json->valuestring));
    cJSON_Delete(root);

    char fingerprint[TLS_SHA256_FINGERPRINT_HEX_LEN + 1] = {};
    if (!tls_mgr_get_identity_fingerprint(fingerprint)) {
        memset(code, 0, sizeof(code));
        send_json_error(
            req, "500 Internal Server Error",
            "Integration TLS identity is unavailable");
        return ESP_OK;
    }

    char token[AUTH_INTEGRATION_TOKEN_LEN + 1] = {};
    const setup_pairing_claim_result_t result =
        setup_pairing_claim(code, token);
    memset(code, 0, sizeof(code));

    if (result != SETUP_PAIRING_CLAIM_OK) {
        memset(token, 0, sizeof(token));
        memset(fingerprint, 0, sizeof(fingerprint));
        switch (result) {
            case SETUP_PAIRING_CLAIM_NOT_OPEN:
                send_json_error(
                    req, "403 Forbidden", "Pairing window is not open");
                break;
            case SETUP_PAIRING_CLAIM_INVALID_CODE:
                send_json_error(
                    req, "401 Unauthorized", "Invalid pairing code");
                break;
            case SETUP_PAIRING_CLAIM_LOCKED:
                send_json_error(
                    req, "429 Too Many Requests",
                    "Pairing window closed after too many attempts");
                break;
            default:
                send_json_error(
                    req, "500 Internal Server Error",
                    "Could not persist integration token");
                break;
        }
        return ESP_OK;
    }

    char response[320];
    const int response_len = snprintf(
        response,
        sizeof(response),
        "{\"protocol_version\":1,\"device_id\":\"%s\","
        "\"sha256_fingerprint\":\"%s\",\"token\":\"%s\"}",
        arctic::ha::deviceId(),
        fingerprint,
        token);
    memset(token, 0, sizeof(token));
    memset(fingerprint, 0, sizeof(fingerprint));
    if (response_len <= 0 || response_len >= (int)sizeof(response)) {
        memset(response, 0, sizeof(response));
        send_json_error(
            req, "500 Internal Server Error",
            "Could not serialize pairing response");
        return ESP_OK;
    }

    set_json_content_type(req);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t send_result =
        httpd_resp_send(req, response, response_len);
    memset(response, 0, sizeof(response));
    if (send_result != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Pairing token rotated but response delivery failed; "
            "physical re-pairing is required");
    }
    return send_result;
}

static esp_err_t ha_capabilities_get_handler(httpd_req_t* req)
{
    if (!check_integration_auth(req)) {
        send_json_error(
            req, "401 Unauthorized", "Integration token required");
        return ESP_OK;
    }
    return send_integration_document(
        req, arctic::ha::createCapabilities());
}

static esp_err_t ha_state_get_handler(httpd_req_t* req)
{
    if (!check_integration_auth(req)) {
        send_json_error(
            req, "401 Unauthorized", "Integration token required");
        return ESP_OK;
    }
    return send_integration_document(req, arctic::ha::createStateSnapshot());
}

static esp_err_t ha_power_put_handler(httpd_req_t* req)
{
    uint32_t generation = 0;
    if (!check_integration_auth(req, &generation)) {
        send_json_error(req, "401 Unauthorized", "Integration token required");
        return ESP_OK;
    }

    char body[160];
    if (!read_integration_body(req, body, sizeof(body))) {
        send_json_error(req, "422 Unprocessable Entity", "Invalid request body");
        return ESP_OK;
    }
    cJSON* root = cJSON_ParseWithLength(body, strlen(body));
    const char* keys[] = {"command_id", "on"};
    cJSON* on = root == nullptr
        ? nullptr
        : cJSON_GetObjectItemCaseSensitive(root, "on");
    char command_id[HA_COMMAND_ID_MAX + 1] = {};
    if (root == nullptr ||
        !integration_object_has_only_keys(root, keys, 2) ||
        !integration_command_id(root, command_id) ||
        on == nullptr || !cJSON_IsBool(on)) {
        cJSON_Delete(root);
        send_json_error(
            req, "422 Unprocessable Entity",
            "Body must contain only command_id and boolean on");
        return ESP_OK;
    }

    const bool power_on = cJSON_IsTrue(on);
    char payload[HA_COMMAND_PAYLOAD_MAX + 1];
    snprintf(payload, sizeof(payload), "on=%u", power_on ? 1U : 0U);
    size_t slot = 0;
    const HaCommandLookup lookup =
        ha_command_begin(command_id, "power", payload, &slot);
    if (lookup == HaCommandLookup::Duplicate) {
        xSemaphoreGive(s_ha_command_mutex);
        cJSON_Delete(root);
        return send_accepted_command(req, command_id);
    }
    if (lookup == HaCommandLookup::Conflict) {
        cJSON_Delete(root);
        send_json_error(
            req, "409 Conflict",
            "command_id was already used with a different command");
        return ESP_OK;
    }

    if (!ha_supports_power_or_mode()) {
        ha_command_finish(slot, false, command_id, "power", payload);
        cJSON_Delete(root);
        send_json_error(
            req, "503 Service Unavailable",
            ha_control_unavailable_message("power"));
        return ESP_OK;
    }
    if (!auth_mgr_begin_control_write(generation)) {
        ha_command_finish(slot, false, command_id, "power", payload);
        cJSON_Delete(root);
        send_json_error(req, "401 Unauthorized", "Integration token rotated");
        return ESP_OK;
    }
    const bool written = arctic::setUnitPower(power_on);
    auth_mgr_end_control_write();
    cJSON_Delete(root);
    if (!written) {
        ha_command_finish(slot, false, command_id, "power", payload);
        send_json_error(
            req, "503 Service Unavailable", "Power command was not accepted");
        return ESP_OK;
    }
    ha_command_finish(slot, true, command_id, "power", payload);
    return send_accepted_command(req, command_id);
}

static esp_err_t ha_mode_put_handler(httpd_req_t* req)
{
    uint32_t generation = 0;
    if (!check_integration_auth(req, &generation)) {
        send_json_error(req, "401 Unauthorized", "Integration token required");
        return ESP_OK;
    }

    char body[192];
    if (!read_integration_body(req, body, sizeof(body))) {
        send_json_error(req, "422 Unprocessable Entity", "Invalid request body");
        return ESP_OK;
    }
    cJSON* root = cJSON_ParseWithLength(body, strlen(body));
    const char* keys[] = {"command_id", "mode"};
    cJSON* mode_value = root == nullptr
        ? nullptr
        : cJSON_GetObjectItemCaseSensitive(root, "mode");
    char command_id[HA_COMMAND_ID_MAX + 1] = {};
    const char* mode = mode_value != nullptr && cJSON_IsString(mode_value)
        ? mode_value->valuestring
        : nullptr;
    const bool valid_mode =
        mode != nullptr &&
        (strcmp(mode, "cooling") == 0 ||
         strcmp(mode, "floor_heating") == 0 ||
         strcmp(mode, "fan_coil_heating") == 0 ||
         strcmp(mode, "hot_water") == 0 ||
         strcmp(mode, "auto") == 0);
    if (root == nullptr ||
        !integration_object_has_only_keys(root, keys, 2) ||
        !integration_command_id(root, command_id) ||
        !valid_mode) {
        cJSON_Delete(root);
        send_json_error(
            req, "422 Unprocessable Entity",
            "Body must contain command_id and an allowlisted mode");
        return ESP_OK;
    }

    char payload[HA_COMMAND_PAYLOAD_MAX + 1];
    snprintf(payload, sizeof(payload), "mode=%s", mode);
    size_t slot = 0;
    const HaCommandLookup lookup =
        ha_command_begin(command_id, "mode", payload, &slot);
    if (lookup == HaCommandLookup::Duplicate) {
        xSemaphoreGive(s_ha_command_mutex);
        cJSON_Delete(root);
        return send_accepted_command(req, command_id);
    }
    if (lookup == HaCommandLookup::Conflict) {
        cJSON_Delete(root);
        send_json_error(
            req, "409 Conflict",
            "command_id was already used with a different command");
        return ESP_OK;
    }

    if (!ha_supports_power_or_mode()) {
        ha_command_finish(slot, false, command_id, "mode", payload);
        cJSON_Delete(root);
        send_json_error(
            req, "503 Service Unavailable",
            ha_control_unavailable_message("mode"));
        return ESP_OK;
    }
    arctic::WorkingMode working_mode = arctic::WorkingMode::COOLING;
    if (strcmp(mode, "floor_heating") == 0) {
        working_mode = arctic::WorkingMode::FLOOR_HEATING;
    } else if (strcmp(mode, "fan_coil_heating") == 0) {
        working_mode = arctic::WorkingMode::FAN_COIL_HEATING;
    } else if (strcmp(mode, "hot_water") == 0) {
        working_mode = arctic::WorkingMode::HOT_WATER;
    } else if (strcmp(mode, "auto") == 0) {
        working_mode = arctic::WorkingMode::AUTO;
    }
    if (!auth_mgr_begin_control_write(generation)) {
        ha_command_finish(slot, false, command_id, "mode", payload);
        cJSON_Delete(root);
        send_json_error(req, "401 Unauthorized", "Integration token rotated");
        return ESP_OK;
    }
    const bool written = arctic::setWorkingMode(working_mode);
    auth_mgr_end_control_write();
    cJSON_Delete(root);
    if (!written) {
        ha_command_finish(slot, false, command_id, "mode", payload);
        send_json_error(
            req, "503 Service Unavailable",
            "Selected-mode command was not accepted");
        return ESP_OK;
    }
    ha_command_finish(slot, true, command_id, "mode", payload);
    return send_accepted_command(req, command_id);
}

static esp_err_t ha_setpoint_put_handler(httpd_req_t* req)
{
    uint32_t generation = 0;
    if (!check_integration_auth(req, &generation)) {
        send_json_error(req, "401 Unauthorized", "Integration token required");
        return ESP_OK;
    }

    char body[192];
    if (!read_integration_body(req, body, sizeof(body))) {
        send_json_error(req, "422 Unprocessable Entity", "Invalid request body");
        return ESP_OK;
    }
    cJSON* root = cJSON_ParseWithLength(body, strlen(body));
    const char* keys[] = {"command_id", "kind", "value"};
    cJSON* kind_value = root == nullptr
        ? nullptr
        : cJSON_GetObjectItemCaseSensitive(root, "kind");
    cJSON* setpoint_value = root == nullptr
        ? nullptr
        : cJSON_GetObjectItemCaseSensitive(root, "value");
    char command_id[HA_COMMAND_ID_MAX + 1] = {};
    const char* kind = kind_value != nullptr && cJSON_IsString(kind_value)
        ? kind_value->valuestring
        : nullptr;
    const bool valid_kind =
        kind != nullptr &&
        (strcmp(kind, "cooling") == 0 ||
         strcmp(kind, "heating") == 0 ||
         strcmp(kind, "hot_water") == 0);
    const bool valid_number =
        setpoint_value != nullptr && cJSON_IsNumber(setpoint_value) &&
        setpoint_value->valuedouble ==
            static_cast<double>(setpoint_value->valueint);
    const int value = valid_number ? setpoint_value->valueint : 0;
    if (root == nullptr ||
        !integration_object_has_only_keys(root, keys, 3) ||
        !integration_command_id(root, command_id) ||
        !valid_kind || !valid_number) {
        cJSON_Delete(root);
        send_json_error(
            req, "422 Unprocessable Entity",
            "Body must contain command_id, kind, and integer value");
        return ESP_OK;
    }

    const arctic::SetpointKind setpoint_kind =
        strcmp(kind, "cooling") == 0
            ? arctic::SetpointKind::Cooling
            : (strcmp(kind, "heating") == 0
                   ? arctic::SetpointKind::Heating
                   : arctic::SetpointKind::HotWater);
    const arctic::SetpointLimits limits =
        arctic::setpoint_limits(setpoint_kind);
    if (value < limits.min_c || value > limits.max_c) {
        cJSON_Delete(root);
        send_json_error(
            req, "422 Unprocessable Entity",
            "Setpoint is outside the advertised inclusive range");
        return ESP_OK;
    }

    char payload[HA_COMMAND_PAYLOAD_MAX + 1];
    snprintf(payload, sizeof(payload), "kind=%s;value=%d", kind, value);
    size_t slot = 0;
    const HaCommandLookup lookup =
        ha_command_begin(command_id, "setpoint", payload, &slot);
    if (lookup == HaCommandLookup::Duplicate) {
        xSemaphoreGive(s_ha_command_mutex);
        cJSON_Delete(root);
        return send_accepted_command(req, command_id);
    }
    if (lookup == HaCommandLookup::Conflict) {
        cJSON_Delete(root);
        send_json_error(
            req, "409 Conflict",
            "command_id was already used with a different command");
        return ESP_OK;
    }

    if (!ha_supports_setpoint(kind)) {
        ha_command_finish(slot, false, command_id, "setpoint", payload);
        cJSON_Delete(root);
        send_json_error(
            req, "503 Service Unavailable",
            ha_control_unavailable_message("setpoint", kind));
        return ESP_OK;
    }
    if (!auth_mgr_begin_control_write(generation)) {
        ha_command_finish(slot, false, command_id, "setpoint", payload);
        cJSON_Delete(root);
        send_json_error(req, "401 Unauthorized", "Integration token rotated");
        return ESP_OK;
    }
    bool written = false;
    if (setpoint_kind == arctic::SetpointKind::Cooling) {
        written = arctic::setCoolingSetpoint(static_cast<int16_t>(value));
    } else if (setpoint_kind == arctic::SetpointKind::HotWater) {
        written = arctic::setHotWaterSetpoint(static_cast<int16_t>(value));
    }
    auth_mgr_end_control_write();
    cJSON_Delete(root);
    if (!written) {
        ha_command_finish(slot, false, command_id, "setpoint", payload);
        send_json_error(
            req, "503 Service Unavailable",
            "Setpoint command was not accepted");
        return ESP_OK;
    }
    ha_command_finish(slot, true, command_id, "setpoint", payload);
    return send_accepted_command(req, command_id);
}

static esp_err_t info_get_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    
    set_json_content_type(req);
    
    cJSON* root = cJSON_CreateObject();
    
    cJSON_AddStringToObject(root, "name", "Arctic Heat Pump Controller");
    cJSON_AddStringToObject(root, "hostname", hostname);
    cJSON_AddStringToObject(root, "platform", "ESP32-P4");
    cJSON_AddStringToObject(root, "wifi_module", "ESP32-C6");
    
    // Get version from app description
    const esp_app_desc_t* app_desc = esp_app_get_description();
    cJSON_AddStringToObject(root, "version", app_desc->version);
    
    // Free heap memory
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_free_heap", (double)esp_get_minimum_free_heap_size());
    
    // Uptime
    cJSON_AddNumberToObject(root, "uptime_ms", (double)xTaskGetTickCount() * portTICK_PERIOD_MS);
    
    // API version
    cJSON_AddStringToObject(root, "api_version", "2.0");
    
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

// ============================================================================
// OTA Handlers
// ============================================================================

static const char* ota_state_to_string(ota_state_t state)
{
    switch (state) {
        case OTA_STATE_IDLE: return "idle";
        case OTA_STATE_DOWNLOADING: return "downloading";
        case OTA_STATE_VERIFYING: return "verifying";
        case OTA_STATE_READY_TO_REBOOT: return "ready_to_reboot";
        case OTA_STATE_FAILED: return "failed";
        default: return "unknown";
    }
}

static esp_err_t ota_status_get_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    
    set_json_content_type(req);
    
    ota_status_t status = ota_mgr_get_status();
    
    cJSON* root = cJSON_CreateObject();
    
    cJSON_AddStringToObject(root, "state", ota_state_to_string(status.state));
    cJSON_AddNumberToObject(root, "progress", status.progress_percent);
    cJSON_AddNumberToObject(root, "bytes_downloaded", (double)status.bytes_downloaded);
    cJSON_AddNumberToObject(root, "total_bytes", (double)status.total_bytes);
    cJSON_AddStringToObject(root, "current_version", status.current_version);
    cJSON_AddBoolToObject(root, "pending_verify", ota_mgr_is_pending_verify());
#ifndef ARCTIC_BUILD_SHA
#define ARCTIC_BUILD_SHA "dev"
#endif
    // Per-build fingerprint (baked at compile time). CI asserts this equals the
    // build it just OTA'd, so a silent rollback to the previous image is caught
    // instead of being masked by tests passing against the rolled-back firmware.
    cJSON_AddStringToObject(root, "build_sha", ARCTIC_BUILD_SHA);
    
    if (status.new_version[0] != '\0') {
        cJSON_AddStringToObject(root, "new_version", status.new_version);
    }
    
    if (status.error_msg[0] != '\0') {
        cJSON_AddStringToObject(root, "error", status.error_msg);
    }
    
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

static esp_err_t ota_update_post_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    
    set_json_content_type(req);
    
    char content[512];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        send_json_error(req, "400 Bad Request", "No body provided");
        return ESP_OK;
    }
    content[ret] = '\0';
    
    cJSON* root = cJSON_Parse(content);
    if (root == NULL) {
        send_json_error(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }
    
    cJSON* url_json = cJSON_GetObjectItem(root, "url");
    if (url_json == NULL || !cJSON_IsString(url_json)) {
        cJSON_Delete(root);
        send_json_error(req, "400 Bad Request", "Missing 'url' field");
        return ESP_OK;
    }
    
    // Check URL is from allowed source
    if (!ota_mgr_is_url_allowed(url_json->valuestring)) {
        cJSON_Delete(root);
        send_json_error(req, "403 Forbidden", "URL not allowed - must be from official GitHub repository");
        return ESP_OK;
    }
    
    if (!ota_mgr_start_update(url_json->valuestring)) {
        cJSON_Delete(root);
        send_json_error(req, "409 Conflict", "OTA update already in progress");
        return ESP_OK;
    }
    
    cJSON_Delete(root);
    
    cJSON* response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "started");
    cJSON_AddStringToObject(response, "message", "OTA update started");
    
    char* json_str = cJSON_PrintUnformatted(response);
    httpd_resp_sendstr(req, json_str);
    
    free(json_str);
    cJSON_Delete(response);
    
    return ESP_OK;
}

static esp_err_t ota_upload_post_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    
    // Try to acquire OTA lock - prevents concurrent updates
    if (!ota_mgr_try_lock_upload()) {
        send_json_error(req, "409 Conflict", "Another OTA update is already in progress");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Receiving firmware upload, content length: %d", req->content_len);
    
    // Get the next OTA partition
    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ota_mgr_unlock_upload();
        send_json_error(req, "500 Internal Server Error", "No OTA partition available");
        return ESP_OK;
    }
    
    esp_ota_handle_t ota_handle;
    // OTA_WITH_SEQUENTIAL_WRITES: do NOT erase the whole partition up front.
    // esp_ota_begin() returns immediately and each esp_ota_write() erases only the
    // sector(s) it is about to write. This bounds the longest CPU/cache-disabled,
    // interrupts-masked window to a single ~4KB sector erase (~50ms) instead of a
    // multi-second whole-partition erase. On the ESP32-P4 (RISC-V) a flash op masks
    // ALL interrupts (MIE cleared) for its whole duration, so a long monolithic erase
    // starves both the USB-CDC console AND the esp_hosted SDIO-RX servicing -> the C6
    // link wedges and the network dies with no reboot. Writes below are <=4KB, so
    // exactly one sector is erased per write. See ota_manager.cpp for the full story.
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        ota_mgr_unlock_upload();
        send_json_error(req, "500 Internal Server Error", "Failed to begin OTA");
        return ESP_OK;
    }
    
    // Receive and write firmware in chunks
    char* buf = (char*)malloc(4096);
    if (buf == NULL) {
        esp_ota_abort(ota_handle);
        ota_mgr_unlock_upload();
        send_json_error(req, "500 Internal Server Error", "Memory allocation failed");
        return ESP_OK;
    }
    
    size_t total_received = 0;
    int received;
    bool success = true;
    bool header_validated = false;
    
    while ((received = httpd_req_recv(req, buf, 4096)) > 0) {
        // Validate ESP32 firmware magic byte in first chunk
        if (!header_validated && received > 0) {
            if ((uint8_t)buf[0] != 0xE9) {
                ESP_LOGE(TAG, "Invalid firmware header: expected 0xE9, got 0x%02X", (uint8_t)buf[0]);
                esp_ota_abort(ota_handle);
                free(buf);
                ota_mgr_unlock_upload();
                send_json_error(req, "400 Bad Request", "Invalid firmware file (not an ESP32 binary)");
                return ESP_OK;
            }
            header_validated = true;
            ESP_LOGI(TAG, "Firmware header validated");
        }
        
        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            success = false;
            break;
        }
        total_received += received;
        
        // Log progress every 100KB
        if (total_received % 102400 == 0) {
            ESP_LOGI(TAG, "Received %lu bytes", (unsigned long)total_received);
        }
    }
    
    free(buf);
    
    if (!success || received < 0) {
        esp_ota_abort(ota_handle);
        ota_mgr_unlock_upload();
        send_json_error(req, "500 Internal Server Error", "Upload failed");
        return ESP_OK;
    }
    
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        ota_mgr_unlock_upload();
        send_json_error(req, "500 Internal Server Error", "OTA validation failed");
        return ESP_OK;
    }
    
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        ota_mgr_unlock_upload();
        send_json_error(req, "500 Internal Server Error", "Failed to set boot partition");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Firmware upload complete: %lu bytes", (unsigned long)total_received);
    
    set_json_content_type(req);
    cJSON* response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddNumberToObject(response, "bytes_received", (double)total_received);
    cJSON_AddStringToObject(response, "message", "Firmware uploaded. Rebooting...");
    
    char* json_str = cJSON_PrintUnformatted(response);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(response);
    
    // Schedule reboot after response is sent
    ESP_LOGI(TAG, "Scheduling reboot...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    
    return ESP_OK;
}

static esp_err_t ota_reboot_post_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    
    set_json_content_type(req);
    httpd_resp_sendstr(req, "{\"status\":\"rebooting\",\"message\":\"Device will reboot now\"}");
    
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    
    return ESP_OK;
}

static esp_err_t ota_releases_get_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    
    set_json_content_type(req);
    
    ota_release_info_t info;
    if (!ota_mgr_check_github_releases(&info)) {
        send_json_error(req, "502 Bad Gateway", "Failed to check GitHub for updates");
        return ESP_OK;
    }
    
    ota_status_t status = ota_mgr_get_status();
    
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "update_available", info.update_available);
    cJSON_AddStringToObject(root, "current_version", status.current_version);
    cJSON_AddStringToObject(root, "latest_version", info.latest_version);
    cJSON_AddStringToObject(root, "published_at", info.published_at);
    
    if (info.download_url[0] != '\0') {
        cJSON_AddBoolToObject(root, "download_ready", true);
    } else {
        cJSON_AddBoolToObject(root, "download_ready", false);
    }
    
    // Truncate release notes for JSON response
    if (info.release_notes[0] != '\0') {
        cJSON_AddStringToObject(root, "release_notes", info.release_notes);
    }
    
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

static esp_err_t ota_github_update_post_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    
    set_json_content_type(req);
    
    const ota_release_info_t* info = ota_mgr_get_release_info();
    if (!info->update_available) {
        send_json_error(req, "400 Bad Request", "No update available - check for updates first");
        return ESP_OK;
    }
    
    if (!ota_mgr_start_github_update()) {
        send_json_error(req, "409 Conflict", "OTA update already in progress or no download URL");
        return ESP_OK;
    }
    
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "started");
    cJSON_AddStringToObject(root, "message", "GitHub update started");
    cJSON_AddStringToObject(root, "version", info->latest_version);
    
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

// ============================================================================
// Auth Handlers
// ============================================================================

static esp_err_t auth_config_get_handler(httpd_req_t* req)
{
    set_json_content_type(req);
    
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "web_auth_enabled", auth_mgr_web_auth_enabled());
    cJSON_AddBoolToObject(root, "api_auth_enabled", auth_mgr_api_auth_enabled());
    cJSON_AddStringToObject(root, "username", auth_mgr_get_username());
    cJSON_AddBoolToObject(
        root, "credentials_change_required",
        auth_mgr_credentials_change_required());
    
    // Check if current request is authenticated
    cJSON_AddBoolToObject(root, "authenticated", check_web_auth(req));
    
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

static esp_err_t auth_status_get_handler(httpd_req_t* req)
{
    // Quick auth status check for web UI - no authentication required
    set_json_content_type(req);
    
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "web_auth_enabled", auth_mgr_web_auth_enabled());
    cJSON_AddBoolToObject(root, "session_valid", check_web_auth(req));
    cJSON_AddBoolToObject(
        root, "credentials_change_required",
        auth_mgr_credentials_change_required());
    
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

static esp_err_t auth_config_post_handler(httpd_req_t* req)
{
    if (!check_web_auth(req)) {
        send_json_error(req, "401 Unauthorized", "Login required");
        return ESP_OK;
    }
    
    set_json_content_type(req);
    
    char content[256];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        send_json_error(req, "400 Bad Request", "No body");
        return ESP_OK;
    }
    content[ret] = '\0';
    
    cJSON* root = cJSON_Parse(content);
    if (!root) {
        send_json_error(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }
    
    cJSON* web_auth = cJSON_GetObjectItem(root, "web_auth_enabled");
    if (web_auth && cJSON_IsBool(web_auth)) {
        if (!cJSON_IsTrue(web_auth)) {
            cJSON_Delete(root);
            send_json_error(
                req, "403 Forbidden",
                "Web authentication cannot be disabled");
            return ESP_OK;
        }
        auth_mgr_set_web_auth_enabled(true);
    }
    
    cJSON* api_auth = cJSON_GetObjectItem(root, "api_auth_enabled");
    bool api_key_generated = false;
    char new_api_key[AUTH_API_KEY_LEN + 1] = {0};
    
    if (api_auth && cJSON_IsBool(api_auth)) {
        if (!cJSON_IsTrue(api_auth)) {
            cJSON_Delete(root);
            send_json_error(
                req, "403 Forbidden",
                "API authentication cannot be disabled");
            return ESP_OK;
        }
        char existing_key[AUTH_API_KEY_LEN + 1];
        if (!auth_mgr_get_api_key(existing_key)) {
            auth_mgr_regenerate_api_key(new_api_key);
            api_key_generated = true;
        }
        auth_mgr_set_api_auth_enabled(true);
    }
    
    cJSON_Delete(root);
    
    cJSON* response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    
    // If we generated a new key, include it in the response
    if (api_key_generated) {
        cJSON_AddStringToObject(response, "api_key", new_api_key);
        cJSON_AddBoolToObject(response, "api_key_generated", true);
    }
    char* json_str = cJSON_PrintUnformatted(response);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(response);
    
    return ESP_OK;
}

static esp_err_t auth_credentials_post_handler(httpd_req_t* req)
{
    if (!check_web_auth(req)) {
        send_json_error(req, "401 Unauthorized", "Login required");
        return ESP_OK;
    }
    
    set_json_content_type(req);
    
    char content[256];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        send_json_error(req, "400 Bad Request", "No body");
        return ESP_OK;
    }
    content[ret] = '\0';
    
    cJSON* root = cJSON_Parse(content);
    if (!root) {
        send_json_error(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }
    
    cJSON* username = cJSON_GetObjectItem(root, "username");
    cJSON* password = cJSON_GetObjectItem(root, "password");
    cJSON* pairing_code = cJSON_GetObjectItem(root, "pairing_code");
    
    const char* u = (username && cJSON_IsString(username)) ? username->valuestring : NULL;
    const char* p = (password && cJSON_IsString(password)) ? password->valuestring : NULL;

    if (u == NULL || u[0] == '\0' ||
        strlen(u) > AUTH_MAX_USERNAME_LEN ||
        p == NULL || strlen(p) < 12 ||
        strlen(p) > AUTH_MAX_PASSWORD_LEN ||
        (strcmp(u, "arctic") == 0 && strcmp(p, "arctic") == 0)) {
        cJSON_Delete(root);
        send_json_error(
            req, "400 Bad Request",
            "A username and a non-default password of at least 12 characters are required");
        return ESP_OK;
    }

    if (auth_mgr_credentials_change_required()) {
        if (!cJSON_IsString(pairing_code) ||
            pairing_code->valuestring == NULL) {
            cJSON_Delete(root);
            send_json_error(
                req, "403 Forbidden",
                "Physical setup code required");
            return ESP_OK;
        }
        char code[SETUP_PAIRING_CODE_LEN + 1] = {};
        if (strlen(pairing_code->valuestring) == SETUP_PAIRING_CODE_LEN) {
            memcpy(code, pairing_code->valuestring, SETUP_PAIRING_CODE_LEN);
        }
        const setup_pairing_claim_result_t authorization =
            setup_pairing_authorize(code);
        memset(code, 0, sizeof(code));
        if (authorization != SETUP_PAIRING_CLAIM_OK) {
            cJSON_Delete(root);
            send_json_error(
                req, "403 Forbidden",
                "Physical setup code was rejected");
            return ESP_OK;
        }
    }
    
    ESP_LOGI(TAG, "Credential update request: username='%s', password=%s", 
             u ? u : "(null)", p ? (p[0] ? "(provided)" : "(empty)") : "(null)");
    
    auth_mgr_set_credentials(u, p);
    
    cJSON_Delete(root);
    
    cJSON* response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    char* json_str = cJSON_PrintUnformatted(response);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(response);
    
    return ESP_OK;
}

static esp_err_t auth_apikey_get_handler(httpd_req_t* req)
{
    if (auth_mgr_credentials_change_required() || !check_web_auth(req)) {
        send_json_error(req, "401 Unauthorized", "Login required");
        return ESP_OK;
    }
    
    set_json_content_type(req);
    
    char api_key[AUTH_API_KEY_LEN + 1];
    auth_mgr_get_api_key(api_key);
    
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "api_key", api_key);
    
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

static esp_err_t auth_apikey_regenerate_handler(httpd_req_t* req)
{
    if (auth_mgr_credentials_change_required() || !check_web_auth(req)) {
        send_json_error(req, "401 Unauthorized", "Login required");
        return ESP_OK;
    }
    
    set_json_content_type(req);
    
    char api_key[AUTH_API_KEY_LEN + 1];
    auth_mgr_regenerate_api_key(api_key);
    
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "api_key", api_key);
    
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

// ============================================================================
// Heat Pump Status
// ============================================================================

static esp_err_t heatpump_status_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    
    set_json_content_type(req);
    
    arctic::HeatPumpState hp = arctic::getState();
    bool demo_mode = arctic::isDemoMode();
    
    cJSON* root = cJSON_CreateObject();
    
    // Connection status
    cJSON_AddBoolToObject(root, "connected", hp.connected);
    cJSON_AddBoolToObject(root, "demo_mode", demo_mode);
    
    // Unified path — demo mode populates hp state at the arctic:: level
    cJSON_AddBoolToObject(root, "unit_on", hp.unit_on);
    cJSON_AddStringToObject(root, "mode", arctic::workingModeToString(hp.working_mode));
    cJSON_AddStringToObject(root, "operation", arctic::heatPumpOperationToString(hp.operation));
    cJSON_AddBoolToObject(root, "defrosting", hp.isDefrosting());
    
    // Components
    cJSON_AddBoolToObject(root, "compressor", hp.isCompressorRunning());
    cJSON_AddBoolToObject(root, "fans", hp.isFanRunning());
    cJSON_AddNumberToObject(root, "fan_speed", hp.getFanSpeedLevel());
    cJSON_AddBoolToObject(root, "pump", hp.isWaterPumpRunning());
    cJSON_AddBoolToObject(root, "aux_heater", hp.isBackupHeaterOn());
    
    // All temperatures
    cJSON* temps = cJSON_AddObjectToObject(root, "temperatures");
    cJSON_AddNumberToObject(temps, "tank", hp.water_tank_temp);
    cJSON_AddNumberToObject(temps, "outlet", hp.outlet_water_temp);
    cJSON_AddNumberToObject(temps, "inlet", hp.inlet_water_temp);
    cJSON_AddNumberToObject(temps, "outdoor", hp.outdoor_ambient_temp);
    cJSON_AddNumberToObject(temps, "discharge", hp.discharge_temp);
    cJSON_AddNumberToObject(temps, "suction", hp.suction_temp);
    cJSON_AddNumberToObject(temps, "outdoor_coil", hp.outdoor_coil_temp);
    cJSON_AddNumberToObject(temps, "indoor_coil", hp.indoor_coil_temp);
    cJSON_AddNumberToObject(temps, "ipm", hp.ipm_temp);
    
    // Setpoints
    cJSON* setpoints = cJSON_AddObjectToObject(root, "setpoints");
    cJSON_AddNumberToObject(setpoints, "cooling", hp.cooling_setpoint);
    cJSON_AddNumberToObject(setpoints, "heating", hp.heating_setpoint);
    cJSON_AddNumberToObject(setpoints, "hot_water", hp.hot_water_setpoint);

    // Setpoint limits (library-owned guardrails; the mainboard enforces none).
    // Consumers clamp/display against these instead of hardcoding their own.
    cJSON* limits = cJSON_AddObjectToObject(root, "setpoint_limits");
    const struct {
        const char* key;
        arctic::SetpointKind kind;
    } kLimitKinds[] = {
        {"cooling", arctic::SetpointKind::Cooling},
        {"heating", arctic::SetpointKind::Heating},
        {"hot_water", arctic::SetpointKind::HotWater},
    };
    for (const auto& lk : kLimitKinds) {
        const arctic::SetpointLimits sl = arctic::setpoint_limits(lk.kind);
        cJSON* o = cJSON_AddObjectToObject(limits, lk.key);
        cJSON_AddNumberToObject(o, "min", sl.min_c);
        cJSON_AddNumberToObject(o, "max", sl.max_c);
    }
    
    // System readings
    cJSON* readings = cJSON_AddObjectToObject(root, "readings");
    cJSON_AddNumberToObject(readings, "compressor_freq", hp.compressor_freq);
    cJSON_AddNumberToObject(readings, "fan_rpm", hp.fan_speed);
    cJSON_AddNumberToObject(readings, "ac_voltage", hp.ac_voltage);
    cJSON_AddNumberToObject(readings, "ac_current", hp.ac_current);
    cJSON_AddNumberToObject(readings, "dc_voltage", hp.getDcVoltageV());
    cJSON_AddNumberToObject(readings, "dc_current", hp.dc_current);
    cJSON_AddNumberToObject(readings, "primary_eev", hp.primary_eev_opening);
    cJSON_AddNumberToObject(readings, "secondary_eev", hp.secondary_eev_opening);
    cJSON_AddNumberToObject(readings, "power_consumption", hp.realtime_power_w);
    // Estimated performance (macon lib; water flow is an assumed input).
    if (hp.cop_valid) {
        cJSON_AddNumberToObject(readings, "heat_out", hp.thermal_w);
        cJSON_AddNumberToObject(readings, "cop", hp.cop_x100 / 100.0);
    } else {
        cJSON_AddNullToObject(readings, "heat_out");
        cJSON_AddNullToObject(readings, "cop");
    }
    
    // Errors
    cJSON_AddBoolToObject(root, "has_error", hp.hasAnyError());
    if (hp.hasAnyError()) {
        char error_buf[256];
        arctic::getErrorDescriptions(error_buf, sizeof(error_buf));
        cJSON_AddStringToObject(root, "error", error_buf);
    } else {
        cJSON_AddNullToObject(root, "error");
    }
    
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

// GET /api/heatpump/raw - Debug: dump the raw fed Tuya register cache as a
// flat JSON array (index 0 == register `base`). Used for calibrating the
// ECO-600 Tuya byte->field mapping against live idle/running snapshots.
static esp_err_t heatpump_raw_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }

    set_json_content_type(req);

    static uint16_t regs[160];
    uint16_t base = 0;
    uint16_t n = arctic::getRawRegisters(regs, sizeof(regs) / sizeof(regs[0]), &base);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "external_feed", arctic::isExternalFeed());
    cJSON_AddNumberToObject(root, "base", base);
    cJSON_AddNumberToObject(root, "count", n);
    cJSON* arr = cJSON_AddArrayToObject(root, "regs");
    for (uint16_t i = 0; i < n; ++i) {
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(regs[i]));
    }

    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);

    return ESP_OK;
}
// Diagnostic: dump the catalog of Tuya response windows observed on the bus,
// including full payloads (with prefix bytes the register feed strips) and any
// windows the codec does not yet map. Used to locate un-decoded register blocks.
static esp_err_t heatpump_windows_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }

    set_json_content_type(req);

    static arctic::ObservedWindow wins[64];
    uint16_t n = arctic::getObservedWindows(wins, sizeof(wins) / sizeof(wins[0]));

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "count", n);
    cJSON* arr = cJSON_AddArrayToObject(root, "windows");
    for (uint16_t i = 0; i < n; ++i) {
        cJSON* w = cJSON_CreateObject();
        cJSON_AddNumberToObject(w, "addr", wins[i].field_a);
        cJSON_AddNumberToObject(w, "len", wins[i].field_b);
        cJSON_AddBoolToObject(w, "known", wins[i].known != 0);
        cJSON_AddNumberToObject(w, "hits", wins[i].hits);
        cJSON_AddNumberToObject(w, "last_ms", wins[i].last_ms);
        cJSON* p = cJSON_AddArrayToObject(w, "payload");
        for (uint8_t j = 0; j < wins[i].payload_len; ++j) {
            cJSON_AddItemToArray(p, cJSON_CreateNumber(wins[i].payload[j]));
        }
        cJSON_AddItemToArray(arr, w);
    }

    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);

    return ESP_OK;
}
// ============================================================================
// Heat Pump Parameters API
// ============================================================================

// Emit the locale-independent enum display options for `p` (if any) as an
// "options" array: [{ wire, label, msg_id, arg_a, arg_b, en }]. The wire code
// is the only value the UI ever PUTs back; label/msg_id/args/en drive display
// and the UI's own localization (macon owns the code<->meaning mapping).
static void add_ap_enum_options(cJSON* obj, const arctic::AdvancedParam* p) {
    const size_t nopt = arctic::advanced_enum_option_count(p->ap);
    if (nopt == 0) return;
    cJSON* opts = cJSON_AddArrayToObject(obj, "options");
    for (size_t i = 0; i < nopt; i++) {
        const arctic::AdvEnumOption* o = arctic::advanced_enum_option_at(p->ap, i);
        if (!o) continue;
        cJSON* io = cJSON_CreateObject();
        cJSON_AddNumberToObject(io, "wire", o->wire);
        cJSON_AddStringToObject(io, "label", o->label ? o->label : "");
        cJSON_AddStringToObject(io, "msg_id", o->msg_id ? o->msg_id : "");
        cJSON_AddNumberToObject(io, "arg_a", o->arg_a);
        cJSON_AddNumberToObject(io, "arg_b", o->arg_b);
        cJSON_AddStringToObject(io, "en", o->en_default ? o->en_default : "");
        cJSON_AddItemToArray(opts, io);
    }
}

static const char* ap_temperature_kind_name(uint8_t ap) {
    switch (arctic::advanced_temperature_kind(ap)) {
        case arctic::AdvancedTemperatureKind::Absolute:
            return "absolute";
        case arctic::AdvancedTemperatureKind::Differential:
            return "differential";
        case arctic::AdvancedTemperatureKind::None:
        default:
            return "none";
    }
}

// Helper to add a single AP (advanced) parameter to a cJSON object, keyed "AP<n>".
static void add_ap_to_json(cJSON* parent, const arctic::AdvancedParam* p, bool read_ok, int16_t value) {
    char key[8];
    snprintf(key, sizeof(key), "AP%u", (unsigned)p->ap);
    const bool reg_known = (p->reg != arctic::ADV_REG_UNKNOWN);
    const bool writable  = reg_known && !p->needs_sim_confirm && !p->read_only && !p->is_trigger;
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "ap", p->ap);
    cJSON_AddStringToObject(obj, "name", p->name ? p->name : "");
    cJSON_AddStringToObject(obj, "detail", p->detail ? p->detail : "");
    cJSON_AddStringToObject(obj, "name_msg_id", p->name_msg_id ? p->name_msg_id : "");
    cJSON_AddStringToObject(obj, "detail_msg_id", p->detail_msg_id ? p->detail_msg_id : "");
    if (read_ok) {
        cJSON_AddNumberToObject(obj, "value", value);
    } else {
        cJSON_AddNullToObject(obj, "value");
    }
    cJSON_AddNumberToObject(obj, "min", arctic::advanced_display_value(p->ap, p->min_val));
    cJSON_AddNumberToObject(obj, "max", arctic::advanced_display_value(p->ap, p->max_val));
    cJSON_AddNumberToObject(obj, "step", arctic::advanced_display_step(p->ap));
    const char* display_unit = arctic::advanced_display_unit(p->ap);
    cJSON_AddStringToObject(obj, "unit", display_unit ? display_unit : "");
    cJSON_AddStringToObject(obj, "temperature_kind", ap_temperature_kind_name(p->ap));
    cJSON_AddStringToObject(obj, "category", p->category ? p->category : "");
    cJSON_AddBoolToObject(obj, "read_only", p->read_only);
    cJSON_AddBoolToObject(obj, "is_trigger", p->is_trigger);
    cJSON_AddBoolToObject(obj, "writable", writable);
    add_ap_enum_options(obj, p);
    cJSON_AddItemToObject(parent, key, obj);
}

// GET /api/heatpump/advanced - List all advanced (AP) parameters (verified regs only)
static esp_err_t heatpump_advanced_get_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    
    set_json_content_type(req);
    
    bool connected = arctic::isConnected();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "connected", connected);
    cJSON_AddBoolToObject(root, "demo_mode", arctic::isDemoMode());
    
    cJSON* params = cJSON_AddObjectToObject(root, "params");
    
    // Iterate by display category (mirrors the Tab5 Control screen ordering),
    // skipping parameters whose register has not been change-and-capture verified.
    const size_t ncat = arctic::advanced_category_count();
    const size_t nparam = arctic::advanced_param_count();
    for (size_t c = 0; c < ncat; c++) {
        const char* cat = arctic::advanced_category_at(c);
        for (size_t i = 0; i < nparam; i++) {
            const arctic::AdvancedParam* p = arctic::advanced_param_at(i);
            if (!p || !cat) continue;
            if (strcmp(p->category, cat) != 0) continue;
            if (p->reg == arctic::ADV_REG_UNKNOWN) continue;  // hide unverified
            int16_t value = 0;
            bool read_ok = advanced_param_read(p->ap, &value);
            add_ap_to_json(params, p, read_ok, value);
        }
    }
    
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

// GET /api/heatpump/advanced/:ap - Get single AP parameter (accepts "AP13" or "13")
static esp_err_t heatpump_advanced_single_get_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    
    // Extract AP id from URI (after /api/heatpump/advanced/)
    const char* uri = req->uri;
    const char* id = uri + strlen("/api/heatpump/advanced/");
    
    if (!id || strlen(id) == 0) {
        send_json_error(req, "404 Not Found", "AP number required");
        return ESP_OK;
    }
    if (id[0] == 'A' || id[0] == 'a') id++;  // tolerate "AP13" prefix
    if (id[0] == 'P' || id[0] == 'p') id++;
    
    char* endptr;
    long ap_long = strtol(id, &endptr, 10);
    if (endptr == id || ap_long < 0 || ap_long > 255) {
        send_json_error(req, "404 Not Found", "Invalid advanced parameter");
        return ESP_OK;
    }
    
    const arctic::AdvancedParam* p = arctic::advanced_param_lookup((uint8_t)ap_long);
    if (!p || p->reg == arctic::ADV_REG_UNKNOWN) {
        send_json_error(req, "404 Not Found", "Unknown advanced parameter");
        return ESP_OK;
    }
    
    set_json_content_type(req);
    
    int16_t value = 0;
    bool read_ok = advanced_param_read(p->ap, &value);
    
    cJSON* root = cJSON_CreateObject();
    char key[8];
    snprintf(key, sizeof(key), "AP%u", (unsigned)p->ap);
    const bool writable = (p->reg != arctic::ADV_REG_UNKNOWN) && !p->needs_sim_confirm &&
                          !p->read_only && !p->is_trigger;
    cJSON_AddNumberToObject(root, "ap", p->ap);
    cJSON_AddStringToObject(root, "key", key);
    cJSON_AddStringToObject(root, "name", p->name ? p->name : "");
    cJSON_AddStringToObject(root, "detail", p->detail ? p->detail : "");
    cJSON_AddStringToObject(root, "name_msg_id", p->name_msg_id ? p->name_msg_id : "");
    cJSON_AddStringToObject(root, "detail_msg_id", p->detail_msg_id ? p->detail_msg_id : "");
    cJSON_AddStringToObject(root, "category", p->category ? p->category : "");
    cJSON_AddNumberToObject(root, "min", arctic::advanced_display_value(p->ap, p->min_val));
    cJSON_AddNumberToObject(root, "max", arctic::advanced_display_value(p->ap, p->max_val));
    cJSON_AddNumberToObject(root, "step", arctic::advanced_display_step(p->ap));
    const char* display_unit = arctic::advanced_display_unit(p->ap);
    cJSON_AddStringToObject(root, "unit", display_unit ? display_unit : "");
    cJSON_AddStringToObject(root, "temperature_kind", ap_temperature_kind_name(p->ap));
    cJSON_AddBoolToObject(root, "read_only", p->read_only);
    cJSON_AddBoolToObject(root, "is_trigger", p->is_trigger);
    cJSON_AddBoolToObject(root, "writable", writable);
    add_ap_enum_options(root, p);
    if (read_ok) {
        cJSON_AddNumberToObject(root, "value", value);
    } else {
        cJSON_AddNullToObject(root, "value");
    }
    cJSON_AddBoolToObject(root, "connected", arctic::isConnected());
    cJSON_AddBoolToObject(root, "demo_mode", arctic::isDemoMode());
    
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

// PUT /api/heatpump/advanced/:ap - Set single AP parameter (accepts "AP13" or "13")
// Body: just the integer value (e.g., "25" or "-5")
static esp_err_t heatpump_advanced_put_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    
    // Check connection (demo mode reports connected via getState)
    if (!arctic::isConnected()) {
        send_json_error(req, "503 Service Unavailable", "Heat pump not connected");
        return ESP_OK;
    }
    
    // Extract AP id from URI
    const char* uri = req->uri;
    const char* id = uri + strlen("/api/heatpump/advanced/");
    
    if (!id || strlen(id) == 0) {
        send_json_error(req, "400 Bad Request", "AP number required");
        return ESP_OK;
    }
    if (id[0] == 'A' || id[0] == 'a') id++;  // tolerate "AP13" prefix
    if (id[0] == 'P' || id[0] == 'p') id++;
    
    char* ap_endptr;
    long ap_long = strtol(id, &ap_endptr, 10);
    if (ap_endptr == id || ap_long < 0 || ap_long > 255) {
        send_json_error(req, "400 Bad Request", "Invalid AP number");
        return ESP_OK;
    }
    uint8_t ap = (uint8_t)ap_long;
    
    const arctic::AdvancedParam* p = arctic::advanced_param_lookup(ap);
    if (!p || p->reg == arctic::ADV_REG_UNKNOWN) {
        send_json_error(req, "404 Not Found", "Unknown advanced parameter");
        return ESP_OK;
    }
    
    // Read request body - expect plain integer
    char body[32];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        send_json_error(req, "400 Bad Request", "Empty request body");
        return ESP_OK;
    }
    body[received] = '\0';
    
    // Parse integer value (trim whitespace)
    char* endptr;
    long val_long = strtol(body, &endptr, 10);
    while (*endptr == ' ' || *endptr == '\t' || *endptr == '\n' || *endptr == '\r') endptr++;
    if (endptr == body || *endptr != '\0') {
        send_json_error(req, "400 Bad Request", "Invalid integer value");
        return ESP_OK;
    }
    
    int16_t value = (int16_t)val_long;
    
    // Write through the arctic-macon guardrail (validates range/enum, confirmed
    // register, and write-lock) so we never blind-write an unverified/locked reg.
    bool bus_ok = false;
    arctic::AdvWriteResult r = advanced_param_write(ap, value, &bus_ok);
    if (r != arctic::AdvWriteResult::OK) {
        send_json_error(req, "400 Bad Request", arctic::adv_write_result_name(r));
        return ESP_OK;
    }
    
    set_json_content_type(req);
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", bus_ok);
    cJSON_AddNumberToObject(resp, "ap", ap);
    cJSON_AddNumberToObject(resp, "value", value);
    cJSON_AddBoolToObject(resp, "demo_mode", arctic::isDemoMode());
    
    if (!bus_ok) {
        cJSON_AddStringToObject(resp, "error", "Write failed");
    }
    
    char* json_str = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(resp);
    
    return ESP_OK;
}

// PUT /api/heatpump/power - Set power on/off
// Body: { "on": true/false }
static esp_err_t heatpump_power_put_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    
    // Check connection (demo mode reports connected via getState)
    if (!arctic::isConnected()) {
        send_json_error(req, "503 Service Unavailable", "Heat pump not connected");
        return ESP_OK;
    }
    
    // Read request body
    char body[64];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        send_json_error(req, "400 Bad Request", "Empty request body");
        return ESP_OK;
    }
    body[received] = '\0';
    
    // Parse JSON
    cJSON* root = cJSON_Parse(body);
    if (!root) {
        send_json_error(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }
    
    cJSON* on_val = cJSON_GetObjectItem(root, "on");
    if (!on_val || !cJSON_IsBool(on_val)) {
        cJSON_Delete(root);
        send_json_error(req, "400 Bad Request", "Missing or invalid 'on' field (boolean required)");
        return ESP_OK;
    }
    
    bool power_on = cJSON_IsTrue(on_val);
    cJSON_Delete(root);
    
    bool success = arctic::setUnitPower(power_on);
    
    set_json_content_type(req);
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", success);
    cJSON_AddBoolToObject(resp, "on", power_on);
    cJSON_AddBoolToObject(resp, "demo_mode", arctic::isDemoMode());
    
    char* json_str = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(resp);
    
    return ESP_OK;
}

// PUT /api/heatpump/mode - Set operating mode
// Body: { "mode": "cooling" | "floor_heating" | "fan_coil_heating" | "hot_water" | "auto" }
static esp_err_t heatpump_mode_put_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    
    // Check connection (demo mode reports connected via getState)
    if (!arctic::isConnected()) {
        send_json_error(req, "503 Service Unavailable", "Heat pump not connected");
        return ESP_OK;
    }
    
    // Read request body
    char body[64];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        send_json_error(req, "400 Bad Request", "Empty request body");
        return ESP_OK;
    }
    body[received] = '\0';
    
    // Parse JSON
    cJSON* root = cJSON_Parse(body);
    if (!root) {
        send_json_error(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }
    
    cJSON* mode_val = cJSON_GetObjectItem(root, "mode");
    if (!mode_val || !cJSON_IsString(mode_val)) {
        cJSON_Delete(root);
        send_json_error(req, "400 Bad Request", "Missing or invalid 'mode' field (string required)");
        return ESP_OK;
    }
    
    const char* mode_str = mode_val->valuestring;
    arctic::WorkingMode mode;
    
    if (strcmp(mode_str, "cooling") == 0) {
        mode = arctic::WorkingMode::COOLING;
    } else if (strcmp(mode_str, "floor_heating") == 0) {
        mode = arctic::WorkingMode::FLOOR_HEATING;
    } else if (strcmp(mode_str, "fan_coil_heating") == 0) {
        mode = arctic::WorkingMode::FAN_COIL_HEATING;
    } else if (strcmp(mode_str, "hot_water") == 0) {
        mode = arctic::WorkingMode::HOT_WATER;
    } else if (strcmp(mode_str, "auto") == 0) {
        mode = arctic::WorkingMode::AUTO;
    } else {
        cJSON_Delete(root);
        send_json_error(req, "400 Bad Request", 
            "Invalid mode. Valid: cooling, floor_heating, fan_coil_heating, hot_water, auto");
        return ESP_OK;
    }
    
    cJSON_Delete(root);
    
    bool success = arctic::setWorkingMode(mode);
    
    set_json_content_type(req);
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", success);
    cJSON_AddStringToObject(resp, "mode", arctic::workingModeToString(mode));
    cJSON_AddBoolToObject(resp, "demo_mode", arctic::isDemoMode());
    
    char* json_str = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(resp);
    
    return ESP_OK;
}

// PUT /api/heatpump/setpoints - Set temperature setpoints
// Body: { "cooling": 18, "heating": 45, "hot_water": 50 }
// All fields optional - only provided fields are updated
static esp_err_t heatpump_setpoints_put_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    
    // Check connection (demo mode reports connected via getState)
    if (!arctic::isConnected()) {
        send_json_error(req, "503 Service Unavailable", "Heat pump not connected");
        return ESP_OK;
    }
    
    // Read request body
    char body[128];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        send_json_error(req, "400 Bad Request", "Empty request body");
        return ESP_OK;
    }
    body[received] = '\0';
    
    // Parse JSON
    cJSON* root = cJSON_Parse(body);
    if (!root) {
        send_json_error(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }
    
    bool any_set = false;
    bool all_success = true;
    
    cJSON* cooling = cJSON_GetObjectItem(root, "cooling");
    cJSON* heating = cJSON_GetObjectItem(root, "heating");
    cJSON* hot_water = cJSON_GetObjectItem(root, "hot_water");
    
    int cooling_val = -999, heating_val = -999, hot_water_val = -999;
    
    if (cooling && cJSON_IsNumber(cooling)) {
        cooling_val = cooling->valueint;
        any_set = true;
        if (!arctic::setCoolingSetpoint((int16_t)cooling_val)) {
            all_success = false;
        }
    }
    
    if (heating && cJSON_IsNumber(heating)) {
        heating_val = heating->valueint;
        any_set = true;
        if (!arctic::setHeatingSetpoint((int16_t)heating_val)) {
            all_success = false;
        }
    }
    
    if (hot_water && cJSON_IsNumber(hot_water)) {
        hot_water_val = hot_water->valueint;
        any_set = true;
        if (!arctic::setHotWaterSetpoint((int16_t)hot_water_val)) {
            all_success = false;
        }
    }
    
    cJSON_Delete(root);
    
    if (!any_set) {
        send_json_error(req, "400 Bad Request", 
            "At least one setpoint required: cooling, heating, or hot_water");
        return ESP_OK;
    }
    
    set_json_content_type(req);
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", all_success);
    
    cJSON* setpoints = cJSON_AddObjectToObject(resp, "setpoints");
    if (cooling_val != -999) cJSON_AddNumberToObject(setpoints, "cooling", cooling_val);
    if (heating_val != -999) cJSON_AddNumberToObject(setpoints, "heating", heating_val);
    if (hot_water_val != -999) cJSON_AddNumberToObject(setpoints, "hot_water", hot_water_val);
    
    cJSON_AddBoolToObject(resp, "demo_mode", arctic::isDemoMode());
    
    char* json_str = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(resp);
    
    return ESP_OK;
}

// DELETE /api/heatpump/errors/history - Clear error history (keeps active errors)
static esp_err_t heatpump_errors_clear_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    
    set_json_content_type(req);
    arctic::clearErrorHistory();
    
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "message", "Error history cleared");
    
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

// ============================================================================
// GET /api/heatpump/diagnostic - Download complete system diagnostic as CSV
// ============================================================================
static esp_err_t heatpump_diagnostic_get_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }

    // Get current timestamp for filename
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    char filename[128];
    snprintf(filename, sizeof(filename), "attachment; filename=\"arctic-diagnostic-%04d%02d%02d-%02d%02d%02d.csv\"",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", filename);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    // Line buffer for CSV rows
    char line[256];

    // UTF-8 BOM so Excel interprets the file correctly (degree signs, accents, etc.)
    httpd_resp_send_chunk(req, "\xEF\xBB\xBF", 3);

    // CSV header
    httpd_resp_sendstr_chunk(req, "Category,Name,P-Code,Register Address,Value,Unit\r\n");

    arctic::HeatPumpState hp = arctic::getState();
    bool demo = arctic::isDemoMode();

    // --- System State ---
    snprintf(line, sizeof(line), "System,Connected,,,\"%s\",\r\n", hp.connected ? "Yes" : "No");
    httpd_resp_sendstr_chunk(req, line);
    snprintf(line, sizeof(line), "System,Demo Mode,,,\"%s\",\r\n", demo ? "Yes" : "No");
    httpd_resp_sendstr_chunk(req, line);
    snprintf(line, sizeof(line), "System,Unit Power,,2000,\"%s\",\r\n", hp.unit_on ? "ON" : "OFF");
    httpd_resp_sendstr_chunk(req, line);
    snprintf(line, sizeof(line), "System,Working Mode,,2001,\"%s\",\r\n", arctic::workingModeToString(hp.working_mode));
    httpd_resp_sendstr_chunk(req, line);

    // --- Temperatures (stored as tenths of °C) ---
    #define DIAG_TEMP(name_str, field, addr) \
        snprintf(line, sizeof(line), "Temperature,%s,,%d,%.1f,°C\r\n", name_str, addr, (field) / 10.0f); \
        httpd_resp_sendstr_chunk(req, line);

    DIAG_TEMP("Water Tank",       hp.water_tank_temp,       2100)
    DIAG_TEMP("Outlet Water",     hp.outlet_water_temp,     2102)
    DIAG_TEMP("Inlet Water",      hp.inlet_water_temp,      2103)
    DIAG_TEMP("Discharge",        hp.discharge_temp,        2104)
    DIAG_TEMP("Suction",          hp.suction_temp,          2105)
    DIAG_TEMP("Outdoor Coil",     hp.outdoor_coil_temp,     2107)
    DIAG_TEMP("Indoor Coil",      hp.indoor_coil_temp,      2108)
    DIAG_TEMP("Outdoor Ambient",  hp.outdoor_ambient_temp,  2110)
    DIAG_TEMP("IPM Module",       hp.ipm_temp,              2114)
    #undef DIAG_TEMP

    // --- Setpoints ---
    snprintf(line, sizeof(line), "Setpoint,Cooling,,2002,%d,°C\r\n", hp.cooling_setpoint);
    httpd_resp_sendstr_chunk(req, line);
    snprintf(line, sizeof(line), "Setpoint,Heating,,2003,%d,°C\r\n", hp.heating_setpoint);
    httpd_resp_sendstr_chunk(req, line);
    snprintf(line, sizeof(line), "Setpoint,Hot Water,,2004,%d,°C\r\n", hp.hot_water_setpoint);
    httpd_resp_sendstr_chunk(req, line);

    // --- System Readings ---
    snprintf(line, sizeof(line), "Reading,Compressor Frequency,,2118,%u,Hz\r\n", hp.compressor_freq);
    httpd_resp_sendstr_chunk(req, line);
    snprintf(line, sizeof(line), "Reading,Fan Speed,,2119,%u,RPM\r\n", hp.fan_speed);
    httpd_resp_sendstr_chunk(req, line);
    snprintf(line, sizeof(line), "Reading,AC Voltage,,2120,%u,V\r\n", hp.ac_voltage);
    httpd_resp_sendstr_chunk(req, line);
    snprintf(line, sizeof(line), "Reading,AC Current,,2121,%.1f,A\r\n", hp.ac_current / 10.0f);
    httpd_resp_sendstr_chunk(req, line);
    snprintf(line, sizeof(line), "Reading,DC Voltage,,2122,%.1f,V\r\n", hp.getDcVoltageV());
    httpd_resp_sendstr_chunk(req, line);
    snprintf(line, sizeof(line), "Reading,DC Current,,2123,%.1f,A\r\n", hp.dc_current / 10.0f);
    httpd_resp_sendstr_chunk(req, line);
    snprintf(line, sizeof(line), "Reading,Primary EEV Opening,,2124,%u,steps\r\n", hp.primary_eev_opening);
    httpd_resp_sendstr_chunk(req, line);
    snprintf(line, sizeof(line), "Reading,Secondary EEV Opening,,2125,%u,steps\r\n", hp.secondary_eev_opening);
    httpd_resp_sendstr_chunk(req, line);
    snprintf(line, sizeof(line), "Reading,Power Consumption,,2114,%lu,W\r\n", (unsigned long)hp.realtime_power_w);
    httpd_resp_sendstr_chunk(req, line);
    if (hp.cop_valid) {
        snprintf(line, sizeof(line), "Reading,Heat Output (est),,,%ld,W\r\n", (long)hp.thermal_w);
        httpd_resp_sendstr_chunk(req, line);
        snprintf(line, sizeof(line), "Reading,COP (est),,,%u.%02u,\r\n", hp.cop_x100 / 100, hp.cop_x100 % 100);
        httpd_resp_sendstr_chunk(req, line);
    }

    // --- Component Status (register 2135) ---
    #define DIAG_STATUS1(name_str, mask) \
        snprintf(line, sizeof(line), "Status,%s,,2135,\"%s\",\r\n", name_str, (hp.status1 & arctic::status1::mask) ? "ON" : "OFF"); \
        httpd_resp_sendstr_chunk(req, line);

    DIAG_STATUS1("Unit",            UNIT_ON)
    DIAG_STATUS1("Compressor",      COMPRESSOR)
    DIAG_STATUS1("Fan High",        FAN_HIGH)
    DIAG_STATUS1("Fan Medium",      FAN_MED)
    DIAG_STATUS1("Fan Low",         FAN_LOW)
    DIAG_STATUS1("Water Pump",      WATER_PUMP)
    DIAG_STATUS1("Four-Way Valve",  FOUR_WAY_VALVE)
    DIAG_STATUS1("Backup Heater",   BACKUP_HEATER)
    DIAG_STATUS1("Water Flow Switch", WATER_FLOW_SW)
    DIAG_STATUS1("High Press Switch", HIGH_PRESS_SW)
    DIAG_STATUS1("Low Press Switch",  LOW_PRESS_SW)
    DIAG_STATUS1("Emergency Switch",  EMERGENCY_SW)
    DIAG_STATUS1("AC Online",       AC_ONLINE)
    DIAG_STATUS1("Mode Switch",     MODE_SWITCH)
    DIAG_STATUS1("3-Way Valve 1",   THREE_WAY_V1)
    DIAG_STATUS1("3-Way Valve 2",   THREE_WAY_V2)
    #undef DIAG_STATUS1

    // --- Component Status (register 2136) ---
    #define DIAG_STATUS2(name_str, mask) \
        snprintf(line, sizeof(line), "Status,%s,,2136,\"%s\",\r\n", name_str, (hp.status2 & arctic::status2::mask) ? "ON" : "OFF"); \
        httpd_resp_sendstr_chunk(req, line);

    DIAG_STATUS2("Solenoid Valve",    SOLENOID_VALVE)
    DIAG_STATUS2("Unloading Valve",   UNLOADING_VALVE)
    DIAG_STATUS2("Oil Return Valve",  OIL_RETURN_VALVE)
    DIAG_STATUS2("Defrosting",        DEFROSTING)
    DIAG_STATUS2("Refrigerant Recovery", REFRIG_RECOVERY)
    DIAG_STATUS2("Oil Return",        OIL_RETURN)
    DIAG_STATUS2("Wired Controller",  WIRED_CTRL_CONN)
    DIAG_STATUS2("Energy Saving",     ENERGY_SAVING)
    DIAG_STATUS2("Antifreeze Level 1", ANTIFREEZE_1)
    DIAG_STATUS2("Antifreeze Level 2", ANTIFREEZE_2)
    DIAG_STATUS2("Sterilization",     STERILIZATION)
    #undef DIAG_STATUS2

    // --- Raw status/error register values ---
    snprintf(line, sizeof(line), "Register,Status 1 (raw),,2135,0x%04X,\r\n", hp.status1);
    httpd_resp_sendstr_chunk(req, line);
    snprintf(line, sizeof(line), "Register,Status 2 (raw),,2136,0x%04X,\r\n", hp.status2);
    httpd_resp_sendstr_chunk(req, line);
    snprintf(line, sizeof(line), "Register,Error 1 (raw),,2137,0x%04X,\r\n", hp.error1);
    httpd_resp_sendstr_chunk(req, line);
    snprintf(line, sizeof(line), "Register,Error 2 (raw),,2138,0x%04X,\r\n", hp.error2);
    httpd_resp_sendstr_chunk(req, line);

    // --- Active Errors (from ErrorDef arrays with Arctic codes) ---
    int err_count = 0;
    const arctic::ErrorDef* err1_defs = arctic::getError1Definitions(&err_count);
    for (int i = 0; i < err_count; i++) {
        if (hp.error1 & err1_defs[i].mask) {
            snprintf(line, sizeof(line), "Error,\"%s\",%s,2137,ACTIVE,\r\n",
                     err1_defs[i].description, err1_defs[i].code);
            httpd_resp_sendstr_chunk(req, line);
        }
    }
    err1_defs = arctic::getError2Definitions(&err_count);
    for (int i = 0; i < err_count; i++) {
        if (hp.error2 & err1_defs[i].mask) {
            snprintf(line, sizeof(line), "Error,\"%s\",%s,2138,ACTIVE,\r\n",
                     err1_defs[i].description, err1_defs[i].code);
            httpd_resp_sendstr_chunk(req, line);
        }
    }

    // --- Advanced (AP) Parameters (technician settings) ---
    {
        const size_t nparam = arctic::advanced_param_count();
        for (size_t i = 0; i < nparam; i++) {
            const arctic::AdvancedParam* p = arctic::advanced_param_at(i);
            if (!p || p->reg == arctic::ADV_REG_UNKNOWN) continue;
            int16_t value = 0;
            bool read_ok = advanced_param_read(p->ap, &value);
            const char* unit = arctic::advanced_display_unit(p->ap);
            if (!unit) unit = "";
            if (read_ok) {
                snprintf(line, sizeof(line), "Parameter,\"%s\",AP%u,%u,%d,%s\r\n",
                         p->name, (unsigned)p->ap, p->reg, value, unit);
            } else {
                snprintf(line, sizeof(line), "Parameter,\"%s\",AP%u,%u,READ_ERROR,%s\r\n",
                         p->name, (unsigned)p->ap, p->reg, unit);
            }
            httpd_resp_sendstr_chunk(req, line);
        }
    }

    // Terminate chunked response
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// GET /api/heatpump/errors - Get active errors and error history
static esp_err_t heatpump_errors_get_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    
    set_json_content_type(req);
    
    arctic::HeatPumpState hp = arctic::getState();
    
    cJSON* root = cJSON_CreateObject();
    
    // Summary
    cJSON_AddBoolToObject(root, "demo_mode", arctic::isDemoMode());
    cJSON_AddBoolToObject(root, "connected", hp.connected);
    
    // Unified path — active errors come from s_state in both modes
    int error_count = arctic::getActiveErrorCount();
    cJSON_AddBoolToObject(root, "has_errors", error_count > 0);
    cJSON_AddNumberToObject(root, "error_count", error_count);
    cJSON_AddStringToObject(root, "highest_severity", 
        arctic::severityToString(arctic::getHighestSeverity()));
    
    // Active errors array
    char* active_json = arctic::getErrorsAsJson();
    if (active_json) {
        cJSON* active = cJSON_Parse(active_json);
        if (active) {
            cJSON_AddItemToObject(root, "active", active);
        }
        free(active_json);
    }
    
    // Error history array
    char* history_json = arctic::getErrorHistoryAsJson();
    if (history_json) {
        cJSON* history = cJSON_Parse(history_json);
        if (history) {
            cJSON_AddItemToObject(root, "history", history);
        }
        free(history_json);
    }
    
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

// PATCH /api/heatpump/demo - Write read-only fields for testing
// Body: { "field1": value1, "field2": value2, ... }
// Only available when demo mode is enabled
static esp_err_t heatpump_demo_patch_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    
    if (!arctic::isDemoMode()) {
        send_json_error(req, "403 Forbidden", "Demo mode is not enabled");
        return ESP_OK;
    }
    
    // Read request body
    char body[512];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        send_json_error(req, "400 Bad Request", "Empty request body");
        return ESP_OK;
    }
    body[received] = '\0';
    
    // Parse JSON
    cJSON* root = cJSON_Parse(body);
    if (!root) {
        send_json_error(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }
    
    set_json_content_type(req);
    
    cJSON* resp = cJSON_CreateObject();
    cJSON* results = cJSON_AddObjectToObject(resp, "results");
    int success_count = 0;
    int fail_count = 0;
    
    // Iterate all keys in the JSON object
    cJSON* item = NULL;
    cJSON_ArrayForEach(item, root) {
        if (!cJSON_IsNumber(item)) {
            cJSON_AddStringToObject(results, item->string, "error: value must be a number");
            fail_count++;
            continue;
        }
        
        int32_t value = (int32_t)item->valuedouble;
        if (arctic::setDemoField(item->string, value)) {
            cJSON_AddStringToObject(results, item->string, "ok");
            success_count++;
        } else {
            cJSON_AddStringToObject(results, item->string, "error: unknown field");
            fail_count++;
        }
    }
    
    cJSON_Delete(root);
    
    cJSON_AddBoolToObject(resp, "success", fail_count == 0 && success_count > 0);
    cJSON_AddNumberToObject(resp, "updated", success_count);
    cJSON_AddNumberToObject(resp, "failed", fail_count);
    
    char* json_str2 = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, json_str2);
    free(json_str2);
    cJSON_Delete(resp);
    
    return ESP_OK;
}

// ============================================================================
// Events API
// ============================================================================

static esp_err_t events_get_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    set_json_content_type(req);
    
    constexpr int kDefaultLimit = 128;
    constexpr int kMaxLimit = 128;
    int offset = 0;
    int limit = kDefaultLimit;
    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[16];
        if (httpd_query_key_value(query, "offset", param, sizeof(param)) == ESP_OK) {
            offset = atoi(param);
            if (offset < 0) offset = 0;
        }
        if (httpd_query_key_value(query, "limit", param, sizeof(param)) == ESP_OK) {
            limit = atoi(param);
            if (limit < 1) limit = 1;
            if (limit > kMaxLimit) limit = kMaxLimit;
        }
    }

    event_entry_t events[kMaxLimit];
    int count = event_log_get(events, limit, offset);
    
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "total", event_log_count());
    cJSON_AddNumberToObject(root, "offset", offset);
    cJSON_AddNumberToObject(root, "count", count);
    cJSON_AddNumberToObject(root, "current_boot_id", event_log_current_boot_id());
    // Durable brownout tracking (survives the reboot a brownout causes).
    cJSON_AddNumberToObject(root, "brownout_count", boot_stats_brownout_count());
    cJSON_AddStringToObject(root, "last_reset_reason",
                            boot_stats_reset_reason_name(boot_stats_last_reset_reason()));
    cJSON* arr = cJSON_AddArrayToObject(root, "events");
    
    for (int i = 0; i < count; i++) {
        cJSON* evt = cJSON_CreateObject();
        cJSON_AddStringToObject(evt, "type", event_type_name(events[i].type));
        cJSON_AddStringToObject(evt, "category",
                                event_category_name(event_type_category(events[i].type)));
        cJSON_AddNumberToObject(evt, "timestamp", events[i].timestamp);
        cJSON_AddNumberToObject(evt, "boot_id", events[i].boot_id);
        cJSON_AddNumberToObject(evt, "uptime_ms", (double)events[i].uptime_ms);
        cJSON_AddNumberToObject(evt, "payload", events[i].payload);
        cJSON_AddItemToArray(arr, evt);
    }
    
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

static esp_err_t events_clear_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    set_json_content_type(req);
    
    event_log_clear();
    httpd_resp_sendstr(req, "{\"success\":true}");
    return ESP_OK;
}

// POST /api/brownout/clear - Reset the persistent brownout counter
static esp_err_t brownout_clear_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    set_json_content_type(req);
    
    boot_stats_clear();
    httpd_resp_sendstr(req, "{\"success\":true}");
    return ESP_OK;
}

// ============================================================================
// Display API
// ============================================================================

static esp_err_t display_brightness_get_handler(httpd_req_t* req)
{
    set_json_content_type(req);
    int brightness = display_screen_get_brightness();
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"brightness\":%d}", brightness);
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t display_brightness_put_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }

    char body[96];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        send_json_error(req, "400 Bad Request", "Empty request body");
        return ESP_OK;
    }
    body[received] = '\0';
    cJSON* root = cJSON_Parse(body);
    cJSON* brightness = root ? cJSON_GetObjectItem(root, "brightness") : NULL;
    if (!cJSON_IsNumber(brightness) || brightness->valueint < 5 || brightness->valueint > 100) {
        cJSON_Delete(root);
        send_json_error(req, "400 Bad Request", "brightness must be between 5 and 100");
        return ESP_OK;
    }
    int value = brightness->valueint;
    cJSON_Delete(root);

    if (!display_screen_set_brightness(value)) {
        send_json_error(req, "500 Internal Server Error", "Unable to save brightness");
        return ESP_OK;
    }
    set_json_content_type(req);
    char response[64];
    snprintf(response, sizeof(response), "{\"success\":true,\"brightness\":%d}", value);
    httpd_resp_sendstr(req, response);
    return ESP_OK;
}

static const char* language_code(language_t language)
{
    switch (language) {
        case LANG_FRENCH: return "fr";
        case LANG_SPANISH: return "es";
        case LANG_ENGLISH:
        default: return "en";
    }
}

static cJSON* create_preferences_json(void)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "demo_mode", app_prefs_is_demo_mode());
    cJSON_AddStringToObject(root, "temp_unit",
        app_prefs_get_temp_unit() == TEMP_UNIT_FAHRENHEIT ? "fahrenheit" : "celsius");
    cJSON_AddNumberToObject(root, "brightness", display_screen_get_brightness());
    cJSON_AddStringToObject(root, "language",
        i18n_get_language_name(i18n_get_language()));
    cJSON_AddStringToObject(root, "language_code", language_code(i18n_get_language()));
    cJSON_AddBoolToObject(root, "format_24h", time_mgr_get_24h_format());
    cJSON_AddStringToObject(root, "timezone", time_mgr_get_timezone());
    return root;
}

static esp_err_t preferences_get_handler(httpd_req_t* req)
{
    set_json_content_type(req);
    cJSON* root = create_preferences_json();
    char* json = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t preferences_patch_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }

    char body[256];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        send_json_error(req, "400 Bad Request", "Empty request body");
        return ESP_OK;
    }
    body[received] = '\0';
    cJSON* root = cJSON_Parse(body);
    if (!root) {
        send_json_error(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }

    cJSON* demo_mode = cJSON_GetObjectItem(root, "demo_mode");
    cJSON* temp_unit = cJSON_GetObjectItem(root, "temp_unit");
    cJSON* language = cJSON_GetObjectItem(root, "language");
    bool any = demo_mode || temp_unit || language;
    if ((demo_mode && !cJSON_IsBool(demo_mode)) ||
        (temp_unit && (!cJSON_IsString(temp_unit) ||
            (strcmp(temp_unit->valuestring, "celsius") != 0 &&
             strcmp(temp_unit->valuestring, "fahrenheit") != 0))) ||
        (language && (!cJSON_IsString(language) ||
            (strcmp(language->valuestring, "en") != 0 &&
             strcmp(language->valuestring, "fr") != 0 &&
             strcmp(language->valuestring, "es") != 0)))) {
        cJSON_Delete(root);
        send_json_error(req, "400 Bad Request", "Invalid preference value");
        return ESP_OK;
    }
    if (!any) {
        cJSON_Delete(root);
        send_json_error(req, "400 Bad Request", "No supported preferences supplied");
        return ESP_OK;
    }

    bool reboot_required = false;
    if (demo_mode) {
        bool enabled = cJSON_IsTrue(demo_mode);
        reboot_required = enabled != app_prefs_is_demo_mode();
        app_prefs_set_demo_mode(enabled);
    }
    if (temp_unit) {
        app_prefs_set_temp_unit(
            strcmp(temp_unit->valuestring, "fahrenheit") == 0
                ? TEMP_UNIT_FAHRENHEIT : TEMP_UNIT_CELSIUS);
    }
    if (language) {
        language_t value = LANG_ENGLISH;
        if (strcmp(language->valuestring, "fr") == 0) value = LANG_FRENCH;
        if (strcmp(language->valuestring, "es") == 0) value = LANG_SPANISH;
        i18n_set_language(value);
    }
    cJSON_Delete(root);

    cJSON* response = create_preferences_json();
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddBoolToObject(response, "reboot_required", reboot_required);
    set_json_content_type(req);
    char* json = cJSON_PrintUnformatted(response);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(response);
    return ESP_OK;
}

static esp_err_t factory_reset_post_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }

    char body[96];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        send_json_error(req, "400 Bad Request", "Confirmation required");
        return ESP_OK;
    }
    body[received] = '\0';
    cJSON* root = cJSON_Parse(body);
    cJSON* confirm = root ? cJSON_GetObjectItem(root, "confirm") : NULL;
    bool confirmed = cJSON_IsString(confirm) &&
                     strcmp(confirm->valuestring, "factory-reset") == 0;
    cJSON_Delete(root);
    if (!confirmed) {
        send_json_error(req, "400 Bad Request", "Set confirm to factory-reset");
        return ESP_OK;
    }
    if (!factory_reset_start()) {
        send_json_error(req, "500 Internal Server Error", "Unable to start factory reset");
        return ESP_OK;
    }

    set_json_content_type(req);
    httpd_resp_sendstr(req, "{\"success\":true,\"restarting\":true}");
    return ESP_OK;
}

// ============================================================================
// Logs API
// ============================================================================

static esp_err_t logs_get_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    set_json_content_type(req);

    // Parse query parameters: ?since=<seq>&level=<E|W|I|D|V>&limit=<n>
    uint32_t since_seq = 0;
    esp_log_level_t min_level = ESP_LOG_VERBOSE;  // Default: all levels
    int limit = LOG_BUFFER_MAX_ENTRIES;

    char query[128] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[32];

        if (httpd_query_key_value(query, "since", param, sizeof(param)) == ESP_OK) {
            since_seq = (uint32_t)atoi(param);
        }
        if (httpd_query_key_value(query, "limit", param, sizeof(param)) == ESP_OK) {
            int l = atoi(param);
            if (l > 0 && l < limit) limit = l;
        }
        if (httpd_query_key_value(query, "level", param, sizeof(param)) == ESP_OK) {
            switch (param[0]) {
                case 'E': case 'e': min_level = ESP_LOG_ERROR;   break;
                case 'W': case 'w': min_level = ESP_LOG_WARN;    break;
                case 'I': case 'i': min_level = ESP_LOG_INFO;    break;
                case 'D': case 'd': min_level = ESP_LOG_DEBUG;   break;
                case 'V': case 'v': min_level = ESP_LOG_VERBOSE; break;
            }
        }
    }

    // Allocate entries on heap (each entry is ~220 bytes, 256 entries = ~56 KB)
    log_entry_t* entries = (log_entry_t*)malloc(limit * sizeof(log_entry_t));
    if (!entries) {
        send_json_error(req, "500 Internal Server Error", "Out of memory");
        return ESP_OK;
    }

    int count = log_buffer_get(entries, limit, since_seq, min_level);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "total", log_buffer_count());
    cJSON_AddNumberToObject(root, "latest_seq", log_buffer_get_latest_seq());
    cJSON* arr = cJSON_AddArrayToObject(root, "entries");

    for (int i = 0; i < count; i++) {
        cJSON* entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "seq", entries[i].seq);
        cJSON_AddNumberToObject(entry, "uptime_ms", entries[i].uptime_ms);
        cJSON_AddStringToObject(entry, "level", log_level_char(entries[i].level));
        cJSON_AddStringToObject(entry, "tag", entries[i].tag);
        cJSON_AddStringToObject(entry, "message", entries[i].message);
        cJSON_AddItemToArray(arr, entry);
    }

    free(entries);

    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);

    return ESP_OK;
}

static esp_err_t logs_clear_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    set_json_content_type(req);

    log_buffer_clear();
    httpd_resp_sendstr(req, "{\"success\":true}");
    return ESP_OK;
}

// ============================================================================
// Screenshot
// ============================================================================

/**
 * Write callback that forwards PNG data to the HTTP chunked response.
 */
static esp_err_t png_http_write(void *ctx, const void *buf, size_t len)
{
    return httpd_resp_send_chunk((httpd_req_t *)ctx, (const char *)buf, len);
}

static esp_err_t screenshot_get_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Screenshot requested");

    // Get screen dimensions under LVGL lock
    bsp_display_lock(0);
    lv_obj_t* screen = lv_screen_active();
    lv_obj_update_layout(screen);
    int32_t w = lv_obj_get_width(screen);
    int32_t h = lv_obj_get_height(screen);
    bsp_display_unlock();

    // Allocate pixel buffer in PSRAM (720x1280x3 ≈ 2.7 MB)
    uint32_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_RGB888);
    uint32_t buf_size = stride * h;
    void* pixel_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!pixel_buf) {
        ESP_LOGE(TAG, "Failed to allocate snapshot buffer (%lu bytes)", (unsigned long)buf_size);
        send_json_error(req, "500 Internal Server Error", "Out of memory");
        return ESP_OK;
    }

    // Init draw buffer with our PSRAM-backed memory
    lv_draw_buf_t snapshot;
    lv_draw_buf_init(&snapshot, w, h, LV_COLOR_FORMAT_RGB888, stride, pixel_buf, buf_size);

    // Capture screen under LVGL lock
    bsp_display_lock(0);
    screen = lv_screen_active();
    lv_result_t snap_res = lv_snapshot_take_to_draw_buf(screen, LV_COLOR_FORMAT_RGB888, &snapshot);
    bsp_display_unlock();

    if (snap_res != LV_RESULT_OK) {
        ESP_LOGE(TAG, "Snapshot capture failed");
        heap_caps_free(pixel_buf);
        send_json_error(req, "500 Internal Server Error", "Snapshot capture failed");
        return ESP_OK;
    }

    // LVGL RGB888 stores B,G,R — swap to R,G,B and pack rows (remove stride padding)
    uint8_t* dst = (uint8_t*)pixel_buf;
    const uint8_t* src_row = (const uint8_t*)pixel_buf;
    for (int32_t y = 0; y < h; y++) {
        const uint8_t* s = src_row;
        for (int32_t x = 0; x < w; x++) {
            uint8_t b = s[0], g = s[1], r = s[2];
            dst[0] = r;
            dst[1] = g;
            dst[2] = b;
            dst += 3;
            s += 3;
        }
        src_row += stride;
    }

    // Stream-encode as uncompressed PNG directly to HTTP response
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=\"screenshot.png\"");

    int64_t t0 = esp_timer_get_time();
    esp_err_t ret = png_encode_uncompressed_rgb888(
        (const uint8_t*)pixel_buf, w, h, png_http_write, req);
    int64_t encode_ms = (esp_timer_get_time() - t0) / 1000;

    heap_caps_free(pixel_buf);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PNG streaming failed: %s", esp_err_to_name(ret));
        // Can't send error JSON — headers already sent as image/png.
        // The incomplete chunked response will cause a client-side error.
        return ret;
    }

    // Finalize chunked transfer
    httpd_resp_send_chunk(req, NULL, 0);

    ESP_LOGI(TAG, "Screenshot streamed: %ldx%ld in %ld ms (uncompressed PNG)",
             (long)w, (long)h, (long)encode_ms);
    return ESP_OK;
}

// ============================================================================
// Home Assistant Management Handlers (web UI)
//
// These endpoints let the authenticated web UI manage Home Assistant pairing
// with full parity to the on-device Settings -> Home Assistant screen. They sit
// behind check_api_auth like every other /api/* settings call, so they inherit
// the fail-closed-when-unsecured behaviour: a factory-fresh controller that has
// not yet had its credentials set will reject them until it is secured (which
// itself requires physical presence).
//
// Showing the pairing code to an authenticated web user is not a privilege
// escalation: that user is already a full administrator (they can change every
// setting, revoke pairing, or drive the controller directly), and the Home
// Assistant integration token is strictly weaker than the access they already
// hold. Taking ownership of the controller ("Secure this controller") remains
// physical-presence-only and is unaffected by these endpoints.
// ============================================================================

// Build the exact name Home Assistant displays for this controller. Must stay
// in sync with the hass-macon integration (custom_components/macon/
// config_flow.py) and the on-device screen (settings_home_assistant_screen.cpp):
// "Macon Heat Pump Controller <LAST4>" where <LAST4> is the upper-cased last 4
// characters of the device_id.
static void ha_manage_build_device_name(char* out, size_t out_len)
{
    const char* device_id = arctic::ha::deviceId();
    const size_t id_len = strlen(device_id);
    char last4[5] = {};
    const char* suffix = id_len >= 4 ? device_id + (id_len - 4) : device_id;
    for (size_t i = 0; i < 4 && suffix[i] != '\0'; ++i) {
        last4[i] = (char)toupper((unsigned char)suffix[i]);
    }
    snprintf(out, out_len, "Macon Heat Pump Controller %s", last4);
}

static esp_err_t ha_manage_status_get_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }

    const setup_pairing_status_t pairing = setup_pairing_get_status();
    char device_name[64];
    ha_manage_build_device_name(device_name, sizeof(device_name));

    set_json_content_type(req);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "paired", auth_mgr_has_integration_token());
    cJSON_AddStringToObject(root, "device_name", device_name);
    cJSON_AddBoolToObject(root, "pairing_active", pairing.active);
    cJSON_AddNumberToObject(
        root, "remaining_seconds", pairing.remaining_seconds);

    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t ha_manage_pair_post_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }

    char code[SETUP_PAIRING_CODE_LEN + 1] = {};
    if (!setup_pairing_start(code)) {
        send_json_error(
            req, "500 Internal Server Error",
            "Could not open pairing window");
        return ESP_OK;
    }

    const setup_pairing_status_t pairing = setup_pairing_get_status();

    set_json_content_type(req);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "code", code);
    cJSON_AddNumberToObject(
        root, "expires_in_seconds", pairing.remaining_seconds);
    memset(code, 0, sizeof(code));

    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t ha_manage_pair_cancel_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }

    setup_pairing_cancel();

    set_json_content_type(req);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t ha_manage_revoke_post_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }

    const bool revoked = auth_mgr_revoke_integration_token();
    setup_pairing_cancel();

    if (!revoked) {
        send_json_error(
            req, "500 Internal Server Error",
            "Could not revoke Home Assistant credential");
        return ESP_OK;
    }

    set_json_content_type(req);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// ============================================================================
// TLS Certificate Management Handlers
// ============================================================================

static esp_err_t tls_status_get_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }

    set_json_content_type(req);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "has_certs", tls_mgr_has_certs());
    cJSON_AddBoolToObject(root, "https_active", tls_mgr_is_https_active());
    cJSON_AddBoolToObject(root, "auth_ready",
                          auth_mgr_web_auth_enabled() && auth_mgr_api_auth_enabled());
    cJSON_AddBoolToObject(
        root, "integration_identity", tls_mgr_has_identity());

    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);

    return ESP_OK;
}

static esp_err_t tls_cert_post_handler(httpd_req_t* req)
{
    // Always require authentication for cert provisioning
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }

    // Require both auth methods enabled before allowing TLS provisioning
    if (!auth_mgr_web_auth_enabled() || !auth_mgr_api_auth_enabled()) {
        send_json_error(req, "403 Forbidden",
                        "Both web authentication and API key authentication must be enabled before uploading TLS certificates");
        return ESP_OK;
    }

    // If HTTPS is already active, cert changes are only accepted over HTTPS.
    // Since the server runs in either HTTP or HTTPS mode (never both), if
    // HTTPS is NOT active the request arrived over HTTP — block it.
    if (tls_mgr_has_certs() && !tls_mgr_is_https_active()) {
        // Edge case: certs in NVS but server is HTTP (shouldn't normally happen)
        send_json_error(req, "403 Forbidden",
                        "Certs exist but HTTPS is not active. Reboot first.");
        return ESP_OK;
    }

    set_json_content_type(req);

    // Read the full POST body (cert + key as JSON)
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > (TLS_MAX_CERT_LEN + TLS_MAX_KEY_LEN + 256)) {
        send_json_error(req, "400 Bad Request",
                        "Body too large or empty");
        return ESP_OK;
    }

    char* body = (char*)malloc(content_len + 1);
    if (!body) {
        send_json_error(req, "500 Internal Server Error", "Out of memory");
        return ESP_OK;
    }

    int received = 0;
    while (received < content_len) {
        int ret = httpd_req_recv(req, body + received, content_len - received);
        if (ret <= 0) {
            free(body);
            send_json_error(req, "400 Bad Request", "Failed to read body");
            return ESP_OK;
        }
        received += ret;
    }
    body[received] = '\0';

    cJSON* root = cJSON_Parse(body);
    free(body);
    if (!root) {
        send_json_error(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }

    cJSON* cert_json = cJSON_GetObjectItem(root, "cert");
    cJSON* key_json  = cJSON_GetObjectItem(root, "key");

    if (!cJSON_IsString(cert_json) || !cJSON_IsString(key_json)) {
        cJSON_Delete(root);
        send_json_error(req, "400 Bad Request",
                        "Missing 'cert' or 'key' string fields");
        return ESP_OK;
    }

    const char* cert_pem = cert_json->valuestring;
    const char* key_pem  = key_json->valuestring;

    // Basic PEM validation
    if (strstr(cert_pem, "-----BEGIN CERTIFICATE-----") == NULL) {
        cJSON_Delete(root);
        send_json_error(req, "400 Bad Request",
                        "cert does not look like a PEM certificate");
        return ESP_OK;
    }
    if (strstr(key_pem, "-----BEGIN") == NULL ||
        strstr(key_pem, "PRIVATE KEY-----") == NULL) {
        cJSON_Delete(root);
        send_json_error(req, "400 Bad Request",
                        "key does not look like a PEM private key");
        return ESP_OK;
    }

    size_t cert_len = strlen(cert_pem) + 1;  // include null terminator
    size_t key_len  = strlen(key_pem) + 1;

    if (cert_len > TLS_MAX_CERT_LEN) {
        cJSON_Delete(root);
        send_json_error(req, "400 Bad Request", "Certificate too large");
        return ESP_OK;
    }
    if (key_len > TLS_MAX_KEY_LEN) {
        cJSON_Delete(root);
        send_json_error(req, "400 Bad Request", "Private key too large");
        return ESP_OK;
    }

    bool ok = tls_mgr_store_certs(cert_pem, cert_len, key_pem, key_len);
    cJSON_Delete(root);

    if (!ok) {
        send_json_error(req, "500 Internal Server Error",
                        "Failed to store certificates");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "TLS certificates stored (%d + %d bytes). Reboot to activate HTTPS.",
             (int)cert_len, (int)key_len);

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    cJSON_AddStringToObject(resp, "message",
                            "Certificates stored. Reboot to activate HTTPS.");

    char* json_str = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(resp);

    return ESP_OK;
}

static esp_err_t tls_cert_delete_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }

    set_json_content_type(req);

    if (!tls_mgr_has_certs()) {
        send_json_error(req, "404 Not Found", "No certificates provisioned");
        return ESP_OK;
    }

    bool ok = tls_mgr_clear_certs();
    if (!ok) {
        send_json_error(req, "500 Internal Server Error",
                        "Failed to clear certificates");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "TLS certificates cleared. Reboot to revert to HTTP.");

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    cJSON_AddStringToObject(resp, "message",
                            "Certificates cleared. Reboot to revert to HTTP.");

    char* json_str = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(resp);

    return ESP_OK;
}