/*
 * Arctic Heat Pump Controller
 * WiFi Manager Implementation - ESP-Hosted WiFi via ESP32-C6
 */
#include "wifi_manager.h"
#include "time_manager.h"
#include "api_server.h"
#include <string.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <bsp/m5stack_tab5.h>

static const char* TAG = "wifi_mgr";

// Event group bits
#define WIFI_CONNECTED_BIT   BIT0
#define WIFI_FAIL_BIT        BIT1
#define WIFI_SCAN_DONE_BIT   BIT2
#define WIFI_STA_STARTED_BIT BIT3

// Maximum number of APs to store from scan
#define MAX_SCAN_RESULTS 20

// Internal state
static struct {
    bool initialized;
    wifi_mgr_state_t state;
    char connected_ssid[33];
    char current_ip[16];
    
    EventGroupHandle_t event_group;
    esp_netif_t* sta_netif;
    
    // Scan state
    wifi_mgr_scan_done_cb_t scan_callback;
    wifi_mgr_ap_info_t scan_results[MAX_SCAN_RESULTS];
    uint16_t scan_count;
    
    // Connection state callback
    wifi_mgr_state_cb_t state_callback;
    
} wifi_state = {};

// Forward declarations
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data);
static void ip_event_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data);
static void notify_state_change(wifi_mgr_state_t new_state);

// ============================================================================
// Public API
// ============================================================================

bool wifi_mgr_init(void)
{
    if (wifi_state.initialized) {
        ESP_LOGW(TAG, "WiFi manager already initialized");
        return true;
    }
    
    ESP_LOGI(TAG, "Initializing WiFi manager...");
    
    // Power on the ESP32-C6 WiFi module
    ESP_LOGI(TAG, "Powering on ESP32-C6 WiFi module...");
    bsp_set_wifi_power_enable(true);
    // ESP-Hosted requires adequate time for C6 to boot and initialize SDIO
    vTaskDelay(pdMS_TO_TICKS(1500));  // Give C6 time to boot
    
    // NVS is already initialized in app_main
    
    // Create event group
    wifi_state.event_group = xEventGroupCreate();
    if (wifi_state.event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return false;
    }
    
    // Initialize TCP/IP stack
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    // Create default event loop
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        // ESP_ERR_INVALID_STATE means already created, which is OK
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    // Create default WiFi station
    wifi_state.sta_netif = esp_netif_create_default_wifi_sta();
    if (wifi_state.sta_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create default WiFi STA");
        return false;
    }
    
    // Initialize WiFi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL, NULL));
    
    // Set WiFi mode to station
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    
    // Start WiFi
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    // Wait for WIFI_EVENT_STA_START before considering init complete
    // ESP-Hosted needs time to establish RPC communication with C6
    ESP_LOGI(TAG, "Waiting for WiFi STA to start...");
    EventBits_t bits = xEventGroupWaitBits(wifi_state.event_group,
                                            WIFI_STA_STARTED_BIT,
                                            pdFALSE,  // Don't clear on exit
                                            pdTRUE,   // Wait for all bits
                                            pdMS_TO_TICKS(10000));  // 10 second timeout
    
    if (!(bits & WIFI_STA_STARTED_BIT)) {
        ESP_LOGE(TAG, "Timeout waiting for WiFi STA to start");
        return false;
    }
    
    // Additional delay for ESP-Hosted RPC to stabilize after STA start
    // The co-processor needs time to complete internal initialization
    ESP_LOGI(TAG, "WiFi STA started, waiting for RPC to stabilize...");
    vTaskDelay(pdMS_TO_TICKS(1500));
    
    wifi_state.initialized = true;
    wifi_state.state = WIFI_MGR_STATE_DISCONNECTED;
    
    ESP_LOGI(TAG, "WiFi manager initialized successfully");
    return true;
}

void wifi_mgr_deinit(void)
{
    if (!wifi_state.initialized) {
        return;
    }
    
    ESP_LOGI(TAG, "Deinitializing WiFi manager...");
    
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    
    if (wifi_state.event_group) {
        vEventGroupDelete(wifi_state.event_group);
        wifi_state.event_group = NULL;
    }
    
    // Power off C6
    bsp_set_wifi_power_enable(false);
    
    wifi_state.initialized = false;
    wifi_state.state = WIFI_MGR_STATE_NOT_INITIALIZED;
}

bool wifi_mgr_is_initialized(void)
{
    return wifi_state.initialized;
}

bool wifi_mgr_start_scan(wifi_mgr_scan_done_cb_t callback)
{
    if (!wifi_state.initialized) {
        ESP_LOGE(TAG, "WiFi not initialized");
        return false;
    }
    
    ESP_LOGI(TAG, "Starting WiFi scan...");
    
    wifi_state.scan_callback = callback;
    wifi_state.scan_count = 0;
    
    // Clear the scan done bit
    xEventGroupClearBits(wifi_state.event_group, WIFI_SCAN_DONE_BIT);
    
    // Start scan
    wifi_scan_config_t scan_config = {};
    scan_config.ssid = NULL;
    scan_config.bssid = NULL;
    scan_config.channel = 0;
    scan_config.show_hidden = true;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_config.scan_time.active.min = 100;
    scan_config.scan_time.active.max = 300;
    
    esp_err_t ret = esp_wifi_scan_start(&scan_config, false);  // Non-blocking
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    return true;
}

bool wifi_mgr_connect(const char* ssid, const char* password, wifi_mgr_state_cb_t state_callback)
{
    if (!wifi_state.initialized) {
        ESP_LOGE(TAG, "WiFi not initialized");
        return false;
    }
    
    if (ssid == NULL || strlen(ssid) == 0) {
        ESP_LOGE(TAG, "Invalid SSID");
        return false;
    }
    
    ESP_LOGI(TAG, "Connecting to '%s'...", ssid);
    
    wifi_state.state_callback = state_callback;
    
    // Configure WiFi
    wifi_config_t wifi_config = {};
    memset(&wifi_config, 0, sizeof(wifi_config));
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password && strlen(password) > 0) {
        strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;  // Accept any auth mode
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    
    // Clear event bits
    xEventGroupClearBits(wifi_state.event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    
    // Apply config and connect
    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    // Store SSID for later
    strncpy(wifi_state.connected_ssid, ssid, sizeof(wifi_state.connected_ssid) - 1);
    
    notify_state_change(WIFI_MGR_STATE_CONNECTING);
    
    return true;
}

void wifi_mgr_disconnect(void)
{
    if (!wifi_state.initialized) {
        return;
    }
    
    ESP_LOGI(TAG, "Disconnecting...");
    esp_wifi_disconnect();
    wifi_state.connected_ssid[0] = '\0';
    wifi_state.current_ip[0] = '\0';
    notify_state_change(WIFI_MGR_STATE_DISCONNECTED);
}

wifi_mgr_state_t wifi_mgr_get_state(void)
{
    return wifi_state.state;
}

const char* wifi_mgr_get_connected_ssid(void)
{
    if (wifi_state.state == WIFI_MGR_STATE_CONNECTED) {
        return wifi_state.connected_ssid;
    }
    return NULL;
}

bool wifi_mgr_get_ip_addr(char* buf, size_t buf_len)
{
    if (wifi_state.state != WIFI_MGR_STATE_CONNECTED || wifi_state.current_ip[0] == '\0') {
        return false;
    }
    strncpy(buf, wifi_state.current_ip, buf_len - 1);
    buf[buf_len - 1] = '\0';
    return true;
}

// ============================================================================
// Event Handlers
// ============================================================================

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "WiFi STA started");
            xEventGroupSetBits(wifi_state.event_group, WIFI_STA_STARTED_BIT);
            break;
            
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*)event_data;
            ESP_LOGW(TAG, "Disconnected from AP, reason: %d", event->reason);
            wifi_state.current_ip[0] = '\0';
            
            // Stop REST API server on disconnect
            api_server_stop();
            
            xEventGroupSetBits(wifi_state.event_group, WIFI_FAIL_BIT);
            notify_state_change(WIFI_MGR_STATE_DISCONNECTED);
            break;
        }
        
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "Connected to AP");
            break;
            
        case WIFI_EVENT_SCAN_DONE: {
            ESP_LOGI(TAG, "Scan complete");
            
            // Get scan results
            uint16_t ap_count = 0;
            esp_wifi_scan_get_ap_num(&ap_count);
            
            if (ap_count > MAX_SCAN_RESULTS) {
                ap_count = MAX_SCAN_RESULTS;
            }
            
            wifi_ap_record_t* ap_records = NULL;
            if (ap_count > 0) {
                ap_records = (wifi_ap_record_t*)malloc(sizeof(wifi_ap_record_t) * ap_count);
                if (ap_records) {
                    esp_wifi_scan_get_ap_records(&ap_count, ap_records);
                    
                    // Convert to our format
                    for (int i = 0; i < ap_count; i++) {
                        strncpy(wifi_state.scan_results[i].ssid, 
                               (char*)ap_records[i].ssid, 
                               sizeof(wifi_state.scan_results[i].ssid) - 1);
                        wifi_state.scan_results[i].rssi = ap_records[i].rssi;
                        wifi_state.scan_results[i].authmode = ap_records[i].authmode;
                    }
                    wifi_state.scan_count = ap_count;
                    
                    free(ap_records);
                }
            }
            
            ESP_LOGI(TAG, "Found %d networks", wifi_state.scan_count);
            
            xEventGroupSetBits(wifi_state.event_group, WIFI_SCAN_DONE_BIT);
            
            // Call callback if set
            if (wifi_state.scan_callback) {
                wifi_state.scan_callback(wifi_state.scan_results, wifi_state.scan_count);
            }
            break;
        }
        
        default:
            ESP_LOGD(TAG, "WiFi event: %ld", event_id);
            break;
    }
}

static void ip_event_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data)
{
    if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        snprintf(wifi_state.current_ip, sizeof(wifi_state.current_ip),
                 IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", wifi_state.current_ip);
        
        // Start NTP time synchronization
        time_mgr_start_sync();
        
        // Initialize mDNS and start REST API server
        api_server_init_mdns();
        api_server_start();
        
        xEventGroupSetBits(wifi_state.event_group, WIFI_CONNECTED_BIT);
        notify_state_change(WIFI_MGR_STATE_CONNECTED);
    }
}

static void notify_state_change(wifi_mgr_state_t new_state)
{
    wifi_state.state = new_state;
    
    if (wifi_state.state_callback) {
        const char* ssid = (new_state == WIFI_MGR_STATE_CONNECTED) ? 
                           wifi_state.connected_ssid : NULL;
        wifi_state.state_callback(new_state, ssid);
    }
}

// ============================================================================
// NVS Credential Storage
// ============================================================================

#define NVS_NAMESPACE "wifi_creds"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASSWORD "password"

bool wifi_mgr_save_credentials(const char* ssid, const char* password)
{
    if (!ssid || strlen(ssid) == 0) {
        ESP_LOGE(TAG, "Invalid SSID for saving");
        return false;
    }
    
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return false;
    }
    
    err = nvs_set_str(nvs, NVS_KEY_SSID, ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save SSID: %s", esp_err_to_name(err));
        nvs_close(nvs);
        return false;
    }
    
    // Save password (empty string if NULL)
    err = nvs_set_str(nvs, NVS_KEY_PASSWORD, password ? password : "");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save password: %s", esp_err_to_name(err));
        nvs_close(nvs);
        return false;
    }
    
    err = nvs_commit(nvs);
    nvs_close(nvs);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
        return false;
    }
    
    ESP_LOGI(TAG, "WiFi credentials saved for '%s'", ssid);
    return true;
}

bool wifi_mgr_load_credentials(char* ssid, size_t ssid_len, char* password, size_t password_len)
{
    if (!ssid || ssid_len < 33 || !password || password_len < 65) {
        ESP_LOGE(TAG, "Invalid buffers for loading credentials");
        return false;
    }
    
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "No saved credentials (NVS open failed)");
        return false;
    }
    
    size_t len = ssid_len;
    err = nvs_get_str(nvs, NVS_KEY_SSID, ssid, &len);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "No saved SSID");
        nvs_close(nvs);
        return false;
    }
    
    len = password_len;
    err = nvs_get_str(nvs, NVS_KEY_PASSWORD, password, &len);
    if (err != ESP_OK) {
        // Password might not exist, that's OK for open networks
        password[0] = '\0';
    }
    
    nvs_close(nvs);
    
    ESP_LOGI(TAG, "Loaded saved credentials for '%s'", ssid);
    return true;
}

bool wifi_mgr_has_saved_credentials(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return false;
    }
    
    size_t len = 0;
    err = nvs_get_str(nvs, NVS_KEY_SSID, NULL, &len);
    nvs_close(nvs);
    
    return (err == ESP_OK && len > 1);  // len includes null terminator
}

void wifi_mgr_clear_credentials(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return;
    }
    
    nvs_erase_key(nvs, NVS_KEY_SSID);
    nvs_erase_key(nvs, NVS_KEY_PASSWORD);
    nvs_commit(nvs);
    nvs_close(nvs);
    
    ESP_LOGI(TAG, "WiFi credentials cleared");
}
