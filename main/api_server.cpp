/*
 * Arctic Heat Pump Controller
 * REST API Server with mDNS, Web Interface, and Authentication
 */
#include "api_server.h"
#include "wifi_manager.h"
#include "time_manager.h"
#include "ota_manager.h"
#include "auth_manager.h"
#include "modbus/arctic_heatpump.h"
#include "modbus/arctic_registers.h"
#include "heatpump_params.h"
#include "heatpump_demo_state.h"
#include "app_preferences.h"
#include <esp_http_server.h>
#include <esp_log.h>
#include <mdns.h>
#include <cJSON.h>
#include <string.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>

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
    config.max_uri_handlers = 40;  // Increased for all endpoints
    config.stack_size = 8192;      // Larger stack for file upload
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
    
    #undef REGISTER_URI
    
    ESP_LOGI(TAG, "HTTP server started successfully (32 URI handlers registered)");
    ESP_LOGI(TAG, "Web UI: http://%s.local/", hostname);
    
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
    bool demo_mode = app_prefs_is_demo_mode();
    
    cJSON* root = cJSON_CreateObject();
    
    // Connection status
    cJSON_AddBoolToObject(root, "connected", hp.connected || demo_mode);
    cJSON_AddBoolToObject(root, "demo_mode", demo_mode);
    
    if (demo_mode) {
        // Demo mode - return simulated values from shared state
        cJSON_AddBoolToObject(root, "unit_on", heatpump_demo_get_power());
        cJSON_AddStringToObject(root, "mode", arctic::workingModeToString(heatpump_demo_get_mode()));
        cJSON_AddBoolToObject(root, "defrosting", false);
        
        // Components (simulate activity when power is on)
        bool power_on = heatpump_demo_get_power();
        cJSON_AddBoolToObject(root, "compressor", power_on);
        cJSON_AddBoolToObject(root, "fans", power_on);
        cJSON_AddNumberToObject(root, "fan_speed", power_on ? 2 : 0);
        cJSON_AddBoolToObject(root, "pump", power_on);
        cJSON_AddBoolToObject(root, "aux_heater", false);
        
        // All temperatures
        cJSON* temps = cJSON_AddObjectToObject(root, "temperatures");
        cJSON_AddNumberToObject(temps, "tank", 42);
        cJSON_AddNumberToObject(temps, "outlet", 45);
        cJSON_AddNumberToObject(temps, "inlet", 38);
        cJSON_AddNumberToObject(temps, "outdoor", 22);
        cJSON_AddNumberToObject(temps, "discharge", 85);
        cJSON_AddNumberToObject(temps, "suction", 12);
        cJSON_AddNumberToObject(temps, "outdoor_coil", 35);
        cJSON_AddNumberToObject(temps, "indoor_coil", 40);
        cJSON_AddNumberToObject(temps, "ipm", 55);
        
        // Setpoints from shared state
        cJSON* setpoints = cJSON_AddObjectToObject(root, "setpoints");
        cJSON_AddNumberToObject(setpoints, "cooling", heatpump_demo_get_cooling_setpoint());
        cJSON_AddNumberToObject(setpoints, "heating", heatpump_demo_get_heating_setpoint());
        cJSON_AddNumberToObject(setpoints, "hot_water", heatpump_demo_get_hotwater_setpoint());
        
        // System readings (simulated)
        cJSON* readings = cJSON_AddObjectToObject(root, "readings");
        cJSON_AddNumberToObject(readings, "compressor_freq", power_on ? 60 : 0);
        cJSON_AddNumberToObject(readings, "fan_rpm", power_on ? 850 : 0);
        cJSON_AddNumberToObject(readings, "ac_voltage", 230);
        cJSON_AddNumberToObject(readings, "ac_current", power_on ? 5 : 0);
        cJSON_AddNumberToObject(readings, "dc_voltage", power_on ? 380.0 : 0);
        cJSON_AddNumberToObject(readings, "dc_current", power_on ? 4 : 0);
        cJSON_AddNumberToObject(readings, "high_pressure", power_on ? 2.50 : 0);
        cJSON_AddNumberToObject(readings, "low_pressure", power_on ? 0.85 : 0);
        cJSON_AddNumberToObject(readings, "primary_eev", power_on ? 350 : 0);
        cJSON_AddNumberToObject(readings, "secondary_eev", power_on ? 200 : 0);
        cJSON_AddNumberToObject(readings, "power_consumption", power_on ? 230 * 5 : 0);  // W
        
        // No errors in demo
        cJSON_AddBoolToObject(root, "has_error", false);
        cJSON_AddNullToObject(root, "error");
    } else {
        // Real mode - use actual values from heat pump
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
        cJSON_AddNumberToObject(readings, "power_consumption", hp.ac_voltage * hp.ac_current);  // W
        
        // Errors
        cJSON_AddBoolToObject(root, "has_error", hp.hasAnyError());
        if (hp.hasAnyError()) {
            char error_buf[256];
            arctic::getErrorDescriptions(error_buf, sizeof(error_buf));
            cJSON_AddStringToObject(root, "error", error_buf);
        } else {
            cJSON_AddNullToObject(root, "error");
        }
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
    
    bool connected = arctic::isConnected() || app_prefs_is_demo_mode();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "connected", connected);
    cJSON_AddBoolToObject(root, "demo_mode", app_prefs_is_demo_mode());
    
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
    
    bool connected = arctic::isConnected() || app_prefs_is_demo_mode();
    
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
    cJSON_AddBoolToObject(root, "demo_mode", app_prefs_is_demo_mode());
    
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
    
    // Check connection (allow writes in demo mode too)
    if (!arctic::isConnected() && !app_prefs_is_demo_mode()) {
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
    cJSON_AddBoolToObject(resp, "demo_mode", app_prefs_is_demo_mode());
    
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
    
    bool demo_mode = app_prefs_is_demo_mode();
    
    // Check connection (allow in demo mode)
    if (!arctic::isConnected() && !demo_mode) {
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
    
    bool success = false;
    if (demo_mode) {
        ESP_LOGI(TAG, "[DEMO] Set power = %s", power_on ? "ON" : "OFF");
        heatpump_demo_set_power(power_on);  // Update shared demo state
        success = true;
    } else {
        success = arctic::setUnitPower(power_on);
    }
    
    set_json_content_type(req);
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", success);
    cJSON_AddBoolToObject(resp, "on", power_on);
    cJSON_AddBoolToObject(resp, "demo_mode", demo_mode);
    
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
    
    bool demo_mode = app_prefs_is_demo_mode();
    
    // Check connection (allow in demo mode)
    if (!arctic::isConnected() && !demo_mode) {
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
    
    bool success = false;
    if (demo_mode) {
        ESP_LOGI(TAG, "[DEMO] Set mode = %s", mode_str);
        heatpump_demo_set_mode(mode);  // Update shared demo state
        success = true;
    } else {
        success = arctic::setWorkingMode(mode);
    }
    
    set_json_content_type(req);
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", success);
    cJSON_AddStringToObject(resp, "mode", arctic::workingModeToString(mode));
    cJSON_AddBoolToObject(resp, "demo_mode", demo_mode);
    
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
    
    bool demo_mode = app_prefs_is_demo_mode();
    
    // Check connection (allow in demo mode)
    if (!arctic::isConnected() && !demo_mode) {
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
        if (demo_mode) {
            ESP_LOGI(TAG, "[DEMO] Set cooling setpoint = %d", cooling_val);
            heatpump_demo_set_cooling_setpoint((int16_t)cooling_val);
        } else if (!arctic::setCoolingSetpoint((int16_t)cooling_val)) {
            all_success = false;
        }
    }
    
    if (heating && cJSON_IsNumber(heating)) {
        heating_val = heating->valueint;
        any_set = true;
        if (demo_mode) {
            ESP_LOGI(TAG, "[DEMO] Set heating setpoint = %d", heating_val);
            heatpump_demo_set_heating_setpoint((int16_t)heating_val);
        } else if (!arctic::setHeatingSetpoint((int16_t)heating_val)) {
            all_success = false;
        }
    }
    
    if (hot_water && cJSON_IsNumber(hot_water)) {
        hot_water_val = hot_water->valueint;
        any_set = true;
        if (demo_mode) {
            ESP_LOGI(TAG, "[DEMO] Set hot water setpoint = %d", hot_water_val);
            heatpump_demo_set_hotwater_setpoint((int16_t)hot_water_val);
        } else if (!arctic::setHotWaterSetpoint((int16_t)hot_water_val)) {
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
    
    cJSON_AddBoolToObject(resp, "demo_mode", demo_mode);
    
    char* json_str = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(resp);
    
    return ESP_OK;
}