/*
 * Arctic Heat Pump Controller
 * REST API Server with mDNS Implementation
 */
#include "api_server.h"
#include "wifi_manager.h"
#include "time_manager.h"
#include <esp_http_server.h>
#include <esp_log.h>
#include <mdns.h>
#include <cJSON.h>
#include <string.h>

static const char* TAG = "api_server";

// Base hostname for mDNS (arctic.local, arctic-2.local, etc.)
static const char* HOSTNAME_BASE = "arctic";
static char hostname[32] = "arctic";  // Actual hostname (may have suffix)

// HTTP server handle
static httpd_handle_t server = NULL;

// Forward declarations
static esp_err_t health_get_handler(httpd_req_t* req);
static esp_err_t status_get_handler(httpd_req_t* req);
static esp_err_t time_get_handler(httpd_req_t* req);
static esp_err_t wifi_get_handler(httpd_req_t* req);
static esp_err_t info_get_handler(httpd_req_t* req);

// Check if hostname is already in use via mDNS query
static bool hostname_in_use(const char* name)
{
    esp_ip4_addr_t addr;
    addr.addr = 0;
    
    // Query for the hostname - if we get a response, it's in use
    esp_err_t err = mdns_query_a(name, 1000, &addr);  // 1 second timeout
    
    if (err == ESP_OK && addr.addr != 0) {
        ESP_LOGW(TAG, "Hostname '%s.local' already in use (IP: " IPSTR ")", 
                 name, IP2STR(&addr));
        return true;
    }
    
    return false;
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
    
    // Find an available hostname (arctic, arctic-2, arctic-3, etc.)
    snprintf(hostname, sizeof(hostname), "%s", HOSTNAME_BASE);
    
    int suffix = 2;
    while (hostname_in_use(hostname) && suffix <= 99) {
        snprintf(hostname, sizeof(hostname), "%s-%d", HOSTNAME_BASE, suffix);
        suffix++;
    }
    
    // Set hostname
    err = mdns_hostname_set(hostname);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS hostname set failed: %s", esp_err_to_name(err));
        return false;
    }
    
    // Set instance name (include suffix if we have one)
    char instance_name[64];
    if (suffix == 2) {
        snprintf(instance_name, sizeof(instance_name), "Arctic Heat Pump Controller");
    } else {
        snprintf(instance_name, sizeof(instance_name), "Arctic Heat Pump Controller #%d", suffix - 1);
    }
    err = mdns_instance_name_set(instance_name);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS instance name set failed: %s", esp_err_to_name(err));
    }
    
    // Add HTTP service (_http._tcp)
    err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS service add failed: %s", esp_err_to_name(err));
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
    
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server: %s", esp_err_to_name(ret));
        return false;
    }
    
    // Register URI handlers
    
    // GET /api/health - Health check
    httpd_uri_t health_uri = {
        .uri = "/api/health",
        .method = HTTP_GET,
        .handler = health_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &health_uri);
    
    // GET /api/status - Full device status
    httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &status_uri);
    
    // GET /api/time - Current time info
    httpd_uri_t time_uri = {
        .uri = "/api/time",
        .method = HTTP_GET,
        .handler = time_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &time_uri);
    
    // GET /api/wifi - WiFi status
    httpd_uri_t wifi_uri = {
        .uri = "/api/wifi",
        .method = HTTP_GET,
        .handler = wifi_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &wifi_uri);
    
    // GET /api/info - Device info
    httpd_uri_t info_uri = {
        .uri = "/api/info",
        .method = HTTP_GET,
        .handler = info_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &info_uri);
    
    ESP_LOGI(TAG, "HTTP server started successfully");
    ESP_LOGI(TAG, "API endpoints:");
    ESP_LOGI(TAG, "  GET /api/health - Health check");
    ESP_LOGI(TAG, "  GET /api/status - Full device status");
    ESP_LOGI(TAG, "  GET /api/time   - Current time info");
    ESP_LOGI(TAG, "  GET /api/wifi   - WiFi status");
    ESP_LOGI(TAG, "  GET /api/info   - Device info");
    
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

// ============================================================================
// Request Handlers
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
    }
    
    // Time status
    cJSON* time_obj = cJSON_AddObjectToObject(root, "time");
    char time_str[32];
    time_mgr_get_time_str(time_str, sizeof(time_str), "%H:%M:%S");
    cJSON_AddStringToObject(time_obj, "local", time_str);
    cJSON_AddStringToObject(time_obj, "timezone", time_mgr_get_timezone());
    cJSON_AddBoolToObject(time_obj, "synced", time_mgr_is_synced());
    
    // Add epoch time
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
    set_json_content_type(req);
    
    cJSON* root = cJSON_CreateObject();
    
    // Current time
    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    
    char time_str[32];
    time_mgr_get_time_str(time_str, sizeof(time_str), "%H:%M:%S");
    cJSON_AddStringToObject(root, "time_24h", time_str);
    
    time_mgr_get_time_str(time_str, sizeof(time_str), "%I:%M:%S %p");
    cJSON_AddStringToObject(root, "time_12h", time_str);
    
    // Date
    char date_str[32];
    strftime(date_str, sizeof(date_str), "%Y-%m-%d", &timeinfo);
    cJSON_AddStringToObject(root, "date", date_str);
    
    // Full ISO timestamp
    char iso_str[32];
    strftime(iso_str, sizeof(iso_str), "%Y-%m-%dT%H:%M:%S", &timeinfo);
    cJSON_AddStringToObject(root, "iso", iso_str);
    
    // Timezone
    cJSON_AddStringToObject(root, "timezone", time_mgr_get_timezone());
    cJSON_AddNumberToObject(root, "epoch", (double)now);
    cJSON_AddBoolToObject(root, "ntp_synced", time_mgr_is_synced());
    
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

static esp_err_t wifi_get_handler(httpd_req_t* req)
{
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
        cJSON_AddStringToObject(root, "hostname", hostname);
        
        // Construct full local URL
        char url[64];
        snprintf(url, sizeof(url), "http://%s.local", hostname);
        cJSON_AddStringToObject(root, "local_url", url);
    }
    
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

static esp_err_t info_get_handler(httpd_req_t* req)
{
    set_json_content_type(req);
    
    cJSON* root = cJSON_CreateObject();
    
    cJSON_AddStringToObject(root, "name", "Arctic Heat Pump Controller");
    cJSON_AddStringToObject(root, "hostname", hostname);
    cJSON_AddStringToObject(root, "platform", "ESP32-P4");
    cJSON_AddStringToObject(root, "wifi_module", "ESP32-C6");
    cJSON_AddStringToObject(root, "version", "1.0.0");
    
    // Free heap memory
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_free_heap", (double)esp_get_minimum_free_heap_size());
    
    // Uptime
    cJSON_AddNumberToObject(root, "uptime_ms", (double)xTaskGetTickCount() * portTICK_PERIOD_MS);
    
    // API version
    cJSON_AddStringToObject(root, "api_version", "1.0");
    
    // Available endpoints
    cJSON* endpoints = cJSON_AddArrayToObject(root, "endpoints");
    cJSON_AddItemToArray(endpoints, cJSON_CreateString("/api/health"));
    cJSON_AddItemToArray(endpoints, cJSON_CreateString("/api/status"));
    cJSON_AddItemToArray(endpoints, cJSON_CreateString("/api/time"));
    cJSON_AddItemToArray(endpoints, cJSON_CreateString("/api/wifi"));
    cJSON_AddItemToArray(endpoints, cJSON_CreateString("/api/info"));
    
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json_str);
    
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}
