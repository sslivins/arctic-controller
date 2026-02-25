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
#include "modbus/arctic_heatpump.h"
#include "modbus/arctic_registers.h"
#include "heatpump_params.h"
#include "heatpump_errors.h"
#include "event_log.h"
#include "log_buffer.h"
#include "app_preferences.h"
#include "test_endpoints.h"
#include "mcp_server.h"
#include "png_uncompressed.h"
#include <esp_http_server.h>
#include <esp_log.h>
#include <mdns.h>
#include <cJSON.h>
#include <string.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <lvgl.h>
#include <bsp/m5stack_tab5.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

static const char* TAG = "api_server";

// Base hostname for mDNS (arctic.local, arctic-2.local, etc.)
static const char* HOSTNAME_BASE = "arctic";
static char hostname[32] = "arctic";  // Actual hostname (may have suffix)

// HTTP server handle
static httpd_handle_t server = NULL;

// Embedded web files (from EMBED_FILES) - gzip compressed
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_index_html_gz_end");

// Session cookie name
static const char* SESSION_COOKIE_NAME = "arctic_session";

// ============================================================================
// Forward Declarations
// ============================================================================

// Web handlers
static esp_err_t web_root_handler(httpd_req_t* req);
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
static esp_err_t info_get_handler(httpd_req_t* req);
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
static esp_err_t heatpump_control_handler(httpd_req_t* req);
static esp_err_t heatpump_params_get_handler(httpd_req_t* req);
static esp_err_t heatpump_param_get_handler(httpd_req_t* req);
static esp_err_t heatpump_param_put_handler(httpd_req_t* req);
static esp_err_t heatpump_power_put_handler(httpd_req_t* req);
static esp_err_t heatpump_mode_put_handler(httpd_req_t* req);
static esp_err_t heatpump_setpoints_put_handler(httpd_req_t* req);
static esp_err_t heatpump_errors_get_handler(httpd_req_t* req);
static esp_err_t heatpump_errors_clear_handler(httpd_req_t* req);
static esp_err_t heatpump_demo_patch_handler(httpd_req_t* req);
static esp_err_t heatpump_diagnostic_get_handler(httpd_req_t* req);
static esp_err_t events_get_handler(httpd_req_t* req);
static esp_err_t events_clear_handler(httpd_req_t* req);
static esp_err_t display_brightness_get_handler(httpd_req_t* req);
static esp_err_t preferences_get_handler(httpd_req_t* req);
static esp_err_t logs_get_handler(httpd_req_t* req);
static esp_err_t logs_clear_handler(httpd_req_t* req);
static esp_err_t screenshot_get_handler(httpd_req_t* req);

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
    if (!auth_mgr_web_auth_enabled()) {
        return true;  // Auth disabled
    }
    
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
    if (!auth_mgr_api_auth_enabled()) {
        return true;  // API auth disabled
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
    
    // If web auth is disabled, allow access from the web interface
    // (API auth is for external programmatic access, not for blocking the local web UI)
    if (!auth_mgr_web_auth_enabled()) {
        return true;
    }
    
    return false;
}

static void set_session_cookie(httpd_req_t* req, const char* token)
{
    char cookie[128];
    snprintf(cookie, sizeof(cookie), 
             "%s=%s; Path=/; Max-Age=%d; HttpOnly; SameSite=Strict",
             SESSION_COOKIE_NAME, token, AUTH_SESSION_LIFETIME_SEC);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
}

static void clear_session_cookie(httpd_req_t* req)
{
    char cookie[128];
    snprintf(cookie, sizeof(cookie),
             "%s=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict",
             SESSION_COOKIE_NAME);
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
    
    // Use base hostname directly
    snprintf(hostname, sizeof(hostname), "%s", HOSTNAME_BASE);
    
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
    
    // Add HTTP service
    err = mdns_service_add(hostname, "_http", "_tcp", 80, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS HTTP service add failed: %s", esp_err_to_name(err));
    }
    
    // Add service TXT records
    mdns_txt_item_t serviceTxtData[] = {
        {"version", "1.0"},
        {"device", "arctic-controller"}
    };
    err = mdns_service_txt_set("_http", "_tcp", serviceTxtData, 2);
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
    
    ESP_LOGI(TAG, "Starting HTTP server on port 80...");
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 92;  // 44 api_server + 4 mcp + 36 test_endpoints = 84 needed
    config.stack_size = 16384;     // Default task stack (tree walk is iterative, not recursive)
    config.max_resp_headers = 16;  // More response headers
    config.recv_wait_timeout = 10; // 10 second receive timeout
    config.max_open_sockets = 4;   // Reduced to leave sockets for OTA/API calls
    
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server: %s", esp_err_to_name(ret));
        return false;
    }
    
    // Helper macro - abort if URI registration fails (catches max_uri_handlers issues)
    #define REGISTER_URI(uri_struct) do { \
        esp_err_t err = httpd_register_uri_handler(server, &(uri_struct)); \
        if (err != ESP_OK) { \
            ESP_LOGE(TAG, "FATAL: Failed to register URI '%s': %s", (uri_struct).uri, esp_err_to_name(err)); \
            ESP_LOGE(TAG, "Increase max_uri_handlers in httpd config!"); \
            abort(); \
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
    REGISTER_URI(health_uri);
    
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
    
    // POST /api/heatpump/control
    httpd_uri_t heatpump_control_uri = {
        .uri = "/api/heatpump/control",
        .method = HTTP_POST,
        .handler = heatpump_control_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(heatpump_control_uri);
    
    // GET /api/heatpump/params - List all parameters
    httpd_uri_t heatpump_params_uri = {
        .uri = "/api/heatpump/params",
        .method = HTTP_GET,
        .handler = heatpump_params_get_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(heatpump_params_uri);
    
    // GET /api/heatpump/params/* - Get single parameter (wildcard)
    httpd_uri_t heatpump_param_get_uri = {
        .uri = "/api/heatpump/params/*",
        .method = HTTP_GET,
        .handler = heatpump_param_get_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(heatpump_param_get_uri);
    
    // PUT /api/heatpump/params/* - Set single parameter (wildcard)
    httpd_uri_t heatpump_param_put_uri = {
        .uri = "/api/heatpump/params/*",
        .method = HTTP_PUT,
        .handler = heatpump_param_put_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(heatpump_param_put_uri);
    
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

    // GET /api/display/brightness - Get current display brightness
    httpd_uri_t display_brightness_uri = {
        .uri = "/api/display/brightness",
        .method = HTTP_GET,
        .handler = display_brightness_get_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(display_brightness_uri);

    // GET /api/preferences - Get current app preferences
    httpd_uri_t preferences_uri = {
        .uri = "/api/preferences",
        .method = HTTP_GET,
        .handler = preferences_get_handler,
        .user_ctx = NULL
    };
    REGISTER_URI(preferences_uri);

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

    // MCP (Model Context Protocol) server — always enabled
    mcp_server_register(server);

#ifdef CONFIG_TEST_ENDPOINTS
    test_endpoints_register(server);
    ESP_LOGI(TAG, "Test instrumentation endpoints enabled");
#endif
    
    #undef REGISTER_URI
    
    ESP_LOGI(TAG, "HTTP server started successfully");
    ESP_LOGI(TAG, "Web UI: http://%s.local/", hostname);
    ESP_LOGI(TAG, "MCP endpoint: http://%s.local/mcp", hostname);
    
    return true;
}

void api_server_stop(void)
{
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
        snprintf(url, sizeof(url), "http://%s.local", hostname);
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
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
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
    // Allow unauthenticated access to check if auth is enabled
    set_json_content_type(req);
    
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "web_auth_enabled", auth_mgr_web_auth_enabled());
    cJSON_AddBoolToObject(root, "api_auth_enabled", auth_mgr_api_auth_enabled());
    cJSON_AddStringToObject(root, "username", auth_mgr_get_username());
    
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
    
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

static esp_err_t auth_config_post_handler(httpd_req_t* req)
{
    // Must be authenticated to change config (if auth is enabled)
    if (auth_mgr_web_auth_enabled() && !check_web_auth(req)) {
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
        auth_mgr_set_web_auth_enabled(cJSON_IsTrue(web_auth));
    }
    
    cJSON* api_auth = cJSON_GetObjectItem(root, "api_auth_enabled");
    bool api_key_generated = false;
    char new_api_key[AUTH_API_KEY_LEN + 1] = {0};
    
    if (api_auth && cJSON_IsBool(api_auth)) {
        bool enable_api = cJSON_IsTrue(api_auth);
        
        // If enabling API auth and no key exists, generate one
        if (enable_api) {
            char existing_key[AUTH_API_KEY_LEN + 1];
            if (!auth_mgr_get_api_key(existing_key)) {
                // No key exists, generate one
                auth_mgr_regenerate_api_key(new_api_key);
                api_key_generated = true;
            }
        }
        
        auth_mgr_set_api_auth_enabled(enable_api);
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
    // Must be authenticated to change credentials (if auth is enabled)
    if (auth_mgr_web_auth_enabled() && !check_web_auth(req)) {
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
    
    const char* u = (username && cJSON_IsString(username)) ? username->valuestring : NULL;
    const char* p = (password && cJSON_IsString(password)) ? password->valuestring : NULL;
    
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
    // Must be authenticated to view API key
    if (auth_mgr_web_auth_enabled() && !check_web_auth(req)) {
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
    // Must be authenticated to regenerate API key
    if (auth_mgr_web_auth_enabled() && !check_web_auth(req)) {
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
    
    // System readings
    cJSON* readings = cJSON_AddObjectToObject(root, "readings");
    cJSON_AddNumberToObject(readings, "compressor_freq", hp.compressor_freq);
    cJSON_AddNumberToObject(readings, "fan_rpm", hp.fan_speed);
    cJSON_AddNumberToObject(readings, "ac_voltage", hp.ac_voltage);
    cJSON_AddNumberToObject(readings, "ac_current", hp.ac_current);
    cJSON_AddNumberToObject(readings, "dc_voltage", hp.getDcVoltageV());
    cJSON_AddNumberToObject(readings, "dc_current", hp.dc_current);
    cJSON_AddNumberToObject(readings, "high_pressure", hp.getHighPressureMPa());
    cJSON_AddNumberToObject(readings, "low_pressure", hp.getLowPressureMPa());
    cJSON_AddNumberToObject(readings, "primary_eev", hp.primary_eev_opening);
    cJSON_AddNumberToObject(readings, "secondary_eev", hp.secondary_eev_opening);
    cJSON_AddNumberToObject(readings, "power_consumption", (hp.ac_voltage * hp.ac_current) / 10);
    
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
// POST /api/heatpump/control
// Body: {"command": "power", "value": true}
//       {"command": "mode", "value": "heating"}
//       {"command": "setpoint", "type": "cooling|heating|hot_water", "value": 25}
//       {"command": "register", "address": 2046, "value": -20}  // technician mode
static esp_err_t heatpump_control_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    
    // Check connection
    if (!arctic::isConnected()) {
        send_json_error(req, "503 Service Unavailable", "Heat pump not connected");
        return ESP_OK;
    }
    
    // Read request body
    char body[256];
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
    
    cJSON* cmd = cJSON_GetObjectItem(root, "command");
    if (!cmd || !cJSON_IsString(cmd)) {
        cJSON_Delete(root);
        send_json_error(req, "400 Bad Request", "Missing 'command' field");
        return ESP_OK;
    }
    
    bool success = false;
    const char* command = cmd->valuestring;
    
    if (strcmp(command, "power") == 0) {
        cJSON* value = cJSON_GetObjectItem(root, "value");
        if (value && cJSON_IsBool(value)) {
            success = arctic::setUnitPower(cJSON_IsTrue(value));
        } else {
            cJSON_Delete(root);
            send_json_error(req, "400 Bad Request", "power requires boolean 'value'");
            return ESP_OK;
        }
    }
    else if (strcmp(command, "mode") == 0) {
        cJSON* value = cJSON_GetObjectItem(root, "value");
        if (value && cJSON_IsString(value)) {
            const char* mode_str = value->valuestring;
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
                send_json_error(req, "400 Bad Request", "Invalid mode value");
                return ESP_OK;
            }
            success = arctic::setWorkingMode(mode);
        } else {
            cJSON_Delete(root);
            send_json_error(req, "400 Bad Request", "mode requires string 'value'");
            return ESP_OK;
        }
    }
    else if (strcmp(command, "setpoint") == 0) {
        cJSON* type = cJSON_GetObjectItem(root, "type");
        cJSON* value = cJSON_GetObjectItem(root, "value");
        if (type && cJSON_IsString(type) && value && cJSON_IsNumber(value)) {
            int16_t temp = (int16_t)value->valueint;
            const char* type_str = type->valuestring;
            if (strcmp(type_str, "cooling") == 0) {
                success = arctic::setCoolingSetpoint(temp);
            } else if (strcmp(type_str, "heating") == 0) {
                success = arctic::setHeatingSetpoint(temp);
            } else if (strcmp(type_str, "hot_water") == 0) {
                success = arctic::setHotWaterSetpoint(temp);
            } else {
                cJSON_Delete(root);
                send_json_error(req, "400 Bad Request", "Invalid setpoint type");
                return ESP_OK;
            }
        } else {
            cJSON_Delete(root);
            send_json_error(req, "400 Bad Request", "setpoint requires 'type' and 'value'");
            return ESP_OK;
        }
    }
    else if (strcmp(command, "register") == 0) {
        // Technician mode: direct register write
        cJSON* addr = cJSON_GetObjectItem(root, "address");
        cJSON* value = cJSON_GetObjectItem(root, "value");
        if (addr && cJSON_IsNumber(addr) && value && cJSON_IsNumber(value)) {
            uint16_t address = (uint16_t)addr->valueint;
            uint16_t val = (uint16_t)value->valueint;
            success = arctic::writeRegister(address, val);
        } else {
            cJSON_Delete(root);
            send_json_error(req, "400 Bad Request", "register requires 'address' and 'value'");
            return ESP_OK;
        }
    }
    else {
        cJSON_Delete(root);
        send_json_error(req, "400 Bad Request", "Unknown command");
        return ESP_OK;
    }
    
    cJSON_Delete(root);
    
    // Send response
    set_json_content_type(req);
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", success);
    if (!success) {
        cJSON_AddStringToObject(resp, "error", "Command failed - check connection");
    }
    char* json_str2 = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, json_str2);
    free(json_str2);
    cJSON_Delete(resp);
    
    return ESP_OK;
}

// ============================================================================
// Heat Pump Parameters API
// ============================================================================

// Helper to add a single parameter to a cJSON object
static void add_param_to_json(cJSON* parent, const HeatPumpParam* param, int16_t value) {
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "value", value);
    cJSON_AddStringToObject(obj, "p_code", param->p_code);
    cJSON_AddStringToObject(obj, "name", param->name);
    cJSON_AddStringToObject(obj, "description", param->description);
    cJSON_AddStringToObject(obj, "unit", param_unit_to_string(param->unit_type));
    cJSON_AddNumberToObject(obj, "min", param->min_val);
    cJSON_AddNumberToObject(obj, "max", param->max_val);
    cJSON_AddStringToObject(obj, "category", param->category);
    cJSON_AddItemToObject(parent, param->key, obj);
}

// GET /api/heatpump/params - List all parameters
static esp_err_t heatpump_params_get_handler(httpd_req_t* req)
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
    
    for (int i = 0; i < NUM_HEATPUMP_PARAMS; i++) {
        const HeatPumpParam* param = &HEATPUMP_PARAMS[i];
        
        // Read current value (handles demo mode)
        bool read_ok = false;
        int16_t value = heatpump_param_read_by_index(i, &read_ok);
        
        add_param_to_json(params, param, read_ok ? value : 0);
    }
    
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

// GET /api/heatpump/params/:id - Get single parameter
static esp_err_t heatpump_param_get_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    
    // Extract param ID from URI (after /api/heatpump/params/)
    const char* uri = req->uri;
    const char* id = uri + strlen("/api/heatpump/params/");
    
    if (!id || strlen(id) == 0) {
        send_json_error(req, "400 Bad Request", "Parameter ID required");
        return ESP_OK;
    }
    
    // Find parameter by key or p_code
    const HeatPumpParam* param = heatpump_param_find(id);
    if (!param) {
        send_json_error(req, "404 Not Found", "Unknown parameter");
        return ESP_OK;
    }
    
    set_json_content_type(req);
    
    // Read current value (handles demo mode)
    bool read_ok = false;
    int16_t value = heatpump_param_read(param, &read_ok);
    
    bool connected = arctic::isConnected();
    
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "key", param->key);
    cJSON_AddStringToObject(root, "p_code", param->p_code);
    cJSON_AddStringToObject(root, "name", param->name);
    cJSON_AddStringToObject(root, "description", param->description);
    cJSON_AddStringToObject(root, "unit", param_unit_to_string(param->unit_type));
    cJSON_AddNumberToObject(root, "min", param->min_val);
    cJSON_AddNumberToObject(root, "max", param->max_val);
    cJSON_AddStringToObject(root, "category", param->category);
    
    if (read_ok) {
        cJSON_AddNumberToObject(root, "value", value);
    } else {
        cJSON_AddNullToObject(root, "value");
    }
    cJSON_AddBoolToObject(root, "connected", connected);
    cJSON_AddBoolToObject(root, "demo_mode", arctic::isDemoMode());
    
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

// PUT /api/heatpump/params/:id - Set single parameter
// Body: just the integer value (e.g., "25" or "-5")
static esp_err_t heatpump_param_put_handler(httpd_req_t* req)
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
    
    // Extract param ID from URI
    const char* uri = req->uri;
    const char* id = uri + strlen("/api/heatpump/params/");
    
    if (!id || strlen(id) == 0) {
        send_json_error(req, "400 Bad Request", "Parameter ID required");
        return ESP_OK;
    }
    
    // Find parameter
    const HeatPumpParam* param = heatpump_param_find(id);
    if (!param) {
        send_json_error(req, "404 Not Found", "Unknown parameter");
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
    
    // Check if parsing succeeded (endptr should point to end or whitespace)
    while (*endptr == ' ' || *endptr == '\t' || *endptr == '\n' || *endptr == '\r') endptr++;
    if (endptr == body || *endptr != '\0') {
        send_json_error(req, "400 Bad Request", "Invalid integer value");
        return ESP_OK;
    }
    
    int16_t value = (int16_t)val_long;
    
    // Validate range
    if (value < param->min_val || value > param->max_val) {
        char err_buf[128];
        snprintf(err_buf, sizeof(err_buf), "Value out of range (%d to %d)", 
                 param->min_val, param->max_val);
        send_json_error(req, "400 Bad Request", err_buf);
        return ESP_OK;
    }
    
    // Write value (handles demo mode)
    bool success = heatpump_param_write(param, value);
    
    set_json_content_type(req);
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", success);
    cJSON_AddStringToObject(resp, "key", param->key);
    cJSON_AddStringToObject(resp, "p_code", param->p_code);
    cJSON_AddNumberToObject(resp, "value", value);
    cJSON_AddBoolToObject(resp, "demo_mode", arctic::isDemoMode());
    
    if (!success) {
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
    httpd_resp_sendstr_chunk(req, "Category,Name,P-Code,Modbus Address,Value,Unit\r\n");

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
    snprintf(line, sizeof(line), "Reading,High Pressure,,2126,%.2f,MPa\r\n", hp.getHighPressureMPa());
    httpd_resp_sendstr_chunk(req, line);
    snprintf(line, sizeof(line), "Reading,Low Pressure,,2127,%.2f,MPa\r\n", hp.getLowPressureMPa());
    httpd_resp_sendstr_chunk(req, line);
    snprintf(line, sizeof(line), "Reading,Power Consumption,,,%.0f,W\r\n", (hp.ac_voltage * hp.ac_current) / 10.0f);
    httpd_resp_sendstr_chunk(req, line);

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

    // --- P-Parameters (technician settings) ---
    for (int i = 0; i < NUM_HEATPUMP_PARAMS; i++) {
        const HeatPumpParam* p = &HEATPUMP_PARAMS[i];
        bool read_ok = false;
        int16_t value = heatpump_param_read_by_index(i, &read_ok);
        if (read_ok) {
            snprintf(line, sizeof(line), "Parameter,\"%s\",%s,%u,%d,%s\r\n",
                     p->name, p->p_code, p->reg_addr, value, param_unit_to_string(p->unit_type));
        } else {
            snprintf(line, sizeof(line), "Parameter,\"%s\",%s,%u,READ_ERROR,%s\r\n",
                     p->name, p->p_code, p->reg_addr, param_unit_to_string(p->unit_type));
        }
        httpd_resp_sendstr_chunk(req, line);
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
    
    // Get events (newest first, up to 128)
    event_entry_t events[128];
    int count = event_log_get(events, 128, 0);
    
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "total", event_log_count());
    cJSON* arr = cJSON_AddArrayToObject(root, "events");
    
    for (int i = 0; i < count; i++) {
        cJSON* evt = cJSON_CreateObject();
        cJSON_AddStringToObject(evt, "type", event_type_name(events[i].type));
        cJSON_AddNumberToObject(evt, "timestamp", events[i].timestamp);
        cJSON_AddNumberToObject(evt, "uptime_ms", events[i].uptime_ms);
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

static esp_err_t preferences_get_handler(httpd_req_t* req)
{
    set_json_content_type(req);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "demo_mode", app_prefs_is_demo_mode());
    cJSON_AddStringToObject(root, "temp_unit",
        app_prefs_get_temp_unit() == TEMP_UNIT_FAHRENHEIT ? "fahrenheit" : "celsius");
    cJSON_AddNumberToObject(root, "brightness", display_screen_get_brightness());
    cJSON_AddStringToObject(root, "language",
        i18n_get_language_name(i18n_get_language()));
    cJSON_AddBoolToObject(root, "format_24h", time_mgr_get_24h_format());
    cJSON_AddStringToObject(root, "timezone", time_mgr_get_timezone());

    char* json = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
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