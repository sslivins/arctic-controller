/*
 * Arctic Heat Pump Controller
 * REST API Server with mDNS, Web Interface, and Authentication
 */
#include "api_server.h"
#include "wifi_manager.h"
#include "time_manager.h"
#include "ota_manager.h"
#include "auth_manager.h"
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

// Embedded web files (from EMBED_FILES)
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

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
static esp_err_t auth_config_get_handler(httpd_req_t* req);
static esp_err_t auth_config_post_handler(httpd_req_t* req);
static esp_err_t auth_status_get_handler(httpd_req_t* req);
static esp_err_t auth_credentials_post_handler(httpd_req_t* req);
static esp_err_t auth_apikey_get_handler(httpd_req_t* req);
static esp_err_t auth_apikey_regenerate_handler(httpd_req_t* req);
static esp_err_t heatpump_status_handler(httpd_req_t* req);

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
    config.max_uri_handlers = 24;  // Increased for all endpoints
    config.stack_size = 8192;      // Larger stack for file upload
    config.max_resp_headers = 16;  // More response headers
    config.recv_wait_timeout = 10; // 10 second receive timeout
    config.max_open_sockets = 4;   // Reduced to leave sockets for OTA/API calls
    
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server: %s", esp_err_to_name(ret));
        return false;
    }
    
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
    httpd_register_uri_handler(server, &web_root_uri);
    
    httpd_uri_t web_index_uri = {
        .uri = "/index.html",
        .method = HTTP_GET,
        .handler = web_root_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &web_index_uri);
    
    // GET /favicon.ico - Return 204 No Content (no favicon)
    httpd_uri_t favicon_uri = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = favicon_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &favicon_uri);
    
    // POST /login - Web login
    httpd_uri_t login_uri = {
        .uri = "/login",
        .method = HTTP_POST,
        .handler = web_login_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &login_uri);
    
    // POST /logout - Web logout
    httpd_uri_t logout_uri = {
        .uri = "/logout",
        .method = HTTP_POST,
        .handler = web_logout_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &logout_uri);
    
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
    httpd_register_uri_handler(server, &health_uri);
    
    // GET /api/status
    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &status_uri);
    
    // GET /api/time
    httpd_uri_t time_uri = {
        .uri = "/api/time",
        .method = HTTP_GET,
        .handler = time_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &time_uri);
    
    // GET/POST /api/time/config
    httpd_uri_t time_config_get_uri = {
        .uri = "/api/time/config",
        .method = HTTP_GET,
        .handler = time_config_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &time_config_get_uri);
    
    httpd_uri_t time_config_post_uri = {
        .uri = "/api/time/config",
        .method = HTTP_POST,
        .handler = time_config_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &time_config_post_uri);
    
    // POST /api/time/sync
    httpd_uri_t time_sync_uri = {
        .uri = "/api/time/sync",
        .method = HTTP_POST,
        .handler = time_sync_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &time_sync_uri);
    
    // GET /api/wifi
    httpd_uri_t wifi_uri = {
        .uri = "/api/wifi",
        .method = HTTP_GET,
        .handler = wifi_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &wifi_uri);
    
    // GET /api/info
    httpd_uri_t info_uri = {
        .uri = "/api/info",
        .method = HTTP_GET,
        .handler = info_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &info_uri);
    
    // GET /api/ota/status
    httpd_uri_t ota_status_uri = {
        .uri = "/api/ota/status",
        .method = HTTP_GET,
        .handler = ota_status_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &ota_status_uri);
    
    // POST /api/ota/update
    httpd_uri_t ota_update_uri = {
        .uri = "/api/ota/update",
        .method = HTTP_POST,
        .handler = ota_update_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &ota_update_uri);
    
    // POST /api/ota/upload
    httpd_uri_t ota_upload_uri = {
        .uri = "/api/ota/upload",
        .method = HTTP_POST,
        .handler = ota_upload_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &ota_upload_uri);
    
    // POST /api/ota/reboot
    httpd_uri_t ota_reboot_uri = {
        .uri = "/api/ota/reboot",
        .method = HTTP_POST,
        .handler = ota_reboot_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &ota_reboot_uri);
    
    // GET /api/auth/config
    httpd_uri_t auth_config_get_uri = {
        .uri = "/api/auth/config",
        .method = HTTP_GET,
        .handler = auth_config_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &auth_config_get_uri);
    
    // POST /api/auth/config
    httpd_uri_t auth_config_post_uri = {
        .uri = "/api/auth/config",
        .method = HTTP_POST,
        .handler = auth_config_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &auth_config_post_uri);
    
    // GET /api/auth/status - Quick auth status check for web UI
    httpd_uri_t auth_status_uri = {
        .uri = "/api/auth/status",
        .method = HTTP_GET,
        .handler = auth_status_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &auth_status_uri);
    
    // POST /api/auth/credentials
    httpd_uri_t auth_credentials_uri = {
        .uri = "/api/auth/credentials",
        .method = HTTP_POST,
        .handler = auth_credentials_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &auth_credentials_uri);
    
    // GET /api/auth/apikey
    httpd_uri_t auth_apikey_uri = {
        .uri = "/api/auth/apikey",
        .method = HTTP_GET,
        .handler = auth_apikey_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &auth_apikey_uri);
    
    // POST /api/auth/apikey/regenerate
    httpd_uri_t auth_apikey_regen_uri = {
        .uri = "/api/auth/apikey/regenerate",
        .method = HTTP_POST,
        .handler = auth_apikey_regenerate_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &auth_apikey_regen_uri);
    
    // GET /api/heatpump/status
    httpd_uri_t heatpump_uri = {
        .uri = "/api/heatpump/status",
        .method = HTTP_GET,
        .handler = heatpump_status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &heatpump_uri);
    
    ESP_LOGI(TAG, "HTTP server started successfully");
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
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    
    size_t html_len = index_html_end - index_html_start;
    httpd_resp_send(req, (const char*)index_html_start, html_len);
    
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
    
    ESP_LOGI(TAG, "Receiving firmware upload, content length: %d", req->content_len);
    
    // Get the next OTA partition
    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        send_json_error(req, "500 Internal Server Error", "No OTA partition available");
        return ESP_OK;
    }
    
    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        send_json_error(req, "500 Internal Server Error", "Failed to begin OTA");
        return ESP_OK;
    }
    
    // Receive and write firmware in chunks
    char* buf = (char*)malloc(4096);
    if (buf == NULL) {
        esp_ota_abort(ota_handle);
        send_json_error(req, "500 Internal Server Error", "Memory allocation failed");
        return ESP_OK;
    }
    
    size_t total_received = 0;
    int received;
    bool success = true;
    
    while ((received = httpd_req_recv(req, buf, 4096)) > 0) {
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
        send_json_error(req, "500 Internal Server Error", "Upload failed");
        return ESP_OK;
    }
    
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        send_json_error(req, "500 Internal Server Error", "OTA validation failed");
        return ESP_OK;
    }
    
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        send_json_error(req, "500 Internal Server Error", "Failed to set boot partition");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Firmware upload complete: %lu bytes", (unsigned long)total_received);
    
    set_json_content_type(req);
    cJSON* response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddNumberToObject(response, "bytes_received", (double)total_received);
    cJSON_AddStringToObject(response, "message", "Firmware uploaded. Ready to reboot.");
    
    char* json_str = cJSON_PrintUnformatted(response);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(response);
    
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
// Heat Pump Status (Placeholder)
// ============================================================================

static esp_err_t heatpump_status_handler(httpd_req_t* req)
{
    if (!check_api_auth(req)) {
        send_json_error(req, "401 Unauthorized", "API key required");
        return ESP_OK;
    }
    
    set_json_content_type(req);
    
    // Placeholder - will be implemented when heat pump communication is added
    cJSON* root = cJSON_CreateObject();
    
    cJSON_AddBoolToObject(root, "connected", false);
    cJSON_AddStringToObject(root, "status", "not_connected");
    
    // Placeholder values
    cJSON* compressor = cJSON_AddObjectToObject(root, "compressor");
    cJSON_AddBoolToObject(compressor, "running", false);
    cJSON_AddStringToObject(compressor, "status", "unknown");
    
    cJSON* fan = cJSON_AddObjectToObject(root, "fan");
    cJSON_AddBoolToObject(fan, "running", false);
    cJSON_AddStringToObject(fan, "status", "unknown");
    
    cJSON* pump = cJSON_AddObjectToObject(root, "pump");
    cJSON_AddBoolToObject(pump, "running", false);
    cJSON_AddStringToObject(pump, "status", "unknown");
    
    cJSON* errors = cJSON_AddArrayToObject(root, "errors");
    // No errors for now
    (void)errors;
    
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}
