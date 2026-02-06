/*
 * Arctic Heat Pump Controller
 * Settings Screen Implementation
 */
#include "settings_screen.h"
#include "ota_manager.h"
#include <string.h>
#include <stdio.h>
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "settings_screen";

// GitHub release API URL
#define GITHUB_API_URL "https://api.github.com/repos/sslivins/arctic-controller/releases/latest"
#define GITHUB_API_TIMEOUT_MS 10000

// ============================================================================
// Colors and Styles (consistent with other screens)
// ============================================================================

#define COLOR_BG            lv_color_hex(0x1a1a2e)
#define COLOR_CARD          lv_color_hex(0x16213e)
#define COLOR_ACCENT        lv_color_hex(0x00d4ff)
#define COLOR_TEXT          lv_color_hex(0xffffff)
#define COLOR_TEXT_DIM      lv_color_hex(0x888888)
#define COLOR_SUCCESS       lv_color_hex(0x4caf50)
#define COLOR_WARNING       lv_color_hex(0xff9800)
#define COLOR_ERROR         lv_color_hex(0xf44336)

// ============================================================================
// Internal State
// ============================================================================

typedef enum {
    UPDATE_STATE_IDLE,
    UPDATE_STATE_CHECKING,
    UPDATE_STATE_UPDATE_AVAILABLE,
    UPDATE_STATE_NO_UPDATE,
    UPDATE_STATE_DOWNLOADING,
    UPDATE_STATE_READY_TO_REBOOT,
    UPDATE_STATE_FAILED
} update_ui_state_t;

static struct {
    bool visible;
    settings_screen_config_t config;
    
    // Screen objects
    lv_obj_t* screen;
    lv_obj_t* header;
    lv_obj_t* content;
    
    // Firmware update section
    lv_obj_t* fw_section;
    lv_obj_t* current_version_label;
    lv_obj_t* latest_version_label;
    lv_obj_t* status_label;
    lv_obj_t* progress_bar;
    lv_obj_t* progress_label;
    lv_obj_t* check_btn;
    lv_obj_t* update_btn;
    
    // State
    update_ui_state_t update_state;
    volatile update_ui_state_t pending_state;  // For async UI updates from tasks
    char current_version[32];
    char latest_version[32];
    char download_url[256];
    
    // Update timer for progress
    lv_timer_t* progress_timer;
    
} settings_state = {};

// Forward declarations
static void create_header(void);
static void create_firmware_section(void);
static void update_ui_state(update_ui_state_t new_state);
static void async_update_ui_cb(void* arg);
static void check_for_updates_task(void* arg);
static void progress_timer_cb(lv_timer_t* timer);

// ============================================================================
// Event Handlers
// ============================================================================

static void close_btn_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (settings_state.config.on_close) {
            settings_state.config.on_close();
        }
        settings_screen_close();
    }
}

static void check_btn_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Check for updates clicked");
        update_ui_state(UPDATE_STATE_CHECKING);
        
        // Start check task
        xTaskCreate(check_for_updates_task, "fw_check", 8192, NULL, 5, NULL);
    }
}

static void update_btn_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Update clicked, URL: %s", settings_state.download_url);
        
        if (strlen(settings_state.download_url) > 0) {
            if (ota_mgr_start_update(settings_state.download_url)) {
                update_ui_state(UPDATE_STATE_DOWNLOADING);
                
                // Start progress timer
                if (!settings_state.progress_timer) {
                    settings_state.progress_timer = lv_timer_create(progress_timer_cb, 200, NULL);
                }
            } else {
                lv_label_set_text(settings_state.status_label, "Failed to start update");
                update_ui_state(UPDATE_STATE_FAILED);
            }
        }
    }
}

// ============================================================================
// GitHub Release Check
// ============================================================================

static char* http_response_buffer = NULL;
static int http_response_len = 0;
#define HTTP_RESPONSE_BUFFER_SIZE 16384  // 16KB to be safe

static esp_err_t http_event_handler(esp_http_client_event_t* evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            // Handle both chunked and non-chunked responses
            if (http_response_buffer == NULL) {
                http_response_buffer = (char*)heap_caps_calloc(1, HTTP_RESPONSE_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                http_response_len = 0;
                if (http_response_buffer == NULL) {
                    ESP_LOGE(TAG, "Failed to allocate HTTP response buffer!");
                    return ESP_ERR_NO_MEM;
                }
            }
            if (http_response_buffer && (http_response_len + evt->data_len) < (HTTP_RESPONSE_BUFFER_SIZE - 1)) {
                memcpy(http_response_buffer + http_response_len, evt->data, evt->data_len);
                http_response_len += evt->data_len;
                http_response_buffer[http_response_len] = '\0';
            } else if (http_response_buffer) {
                ESP_LOGW(TAG, "HTTP response buffer overflow! len=%d, new=%d", http_response_len, evt->data_len);
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void check_for_updates_task(void* arg)
{
    ESP_LOGI(TAG, "Checking for updates from GitHub...");
    
    bool update_available = false;
    char latest_ver[32] = {0};
    char download_url[256] = {0};
    
    // Reset buffer
    if (http_response_buffer) {
        free(http_response_buffer);
        http_response_buffer = NULL;
    }
    http_response_len = 0;
    
    esp_http_client_config_t config = {};
    config.url = GITHUB_API_URL;
    config.event_handler = http_event_handler;
    config.timeout_ms = GITHUB_API_TIMEOUT_MS;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 4096;
    config.buffer_size_tx = 2048;
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    
    // GitHub API requires User-Agent
    esp_http_client_set_header(client, "User-Agent", "arctic-controller/1.0");
    esp_http_client_set_header(client, "Accept", "application/vnd.github.v3+json");
    
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "GitHub API response: %d", status);
        
        if (status == 200 && http_response_buffer && http_response_len > 0) {
            ESP_LOGI(TAG, "Response length: %d bytes", http_response_len);
            
            // Ensure buffer is properly terminated
            http_response_buffer[http_response_len] = '\0';
            
            // Small yield to ensure all data is stable
            vTaskDelay(pdMS_TO_TICKS(10));
            
            // Parse JSON response
            cJSON* root = cJSON_Parse(http_response_buffer);
            if (root) {
                ESP_LOGI(TAG, "JSON parsed successfully");
                // Get tag_name (version)
                cJSON* tag = cJSON_GetObjectItem(root, "tag_name");
                if (tag && tag->valuestring) {
                    // Remove 'v' prefix if present
                    const char* ver = tag->valuestring;
                    if (ver[0] == 'v' || ver[0] == 'V') {
                        ver++;
                    }
                    strncpy(latest_ver, ver, sizeof(latest_ver) - 1);
                    ESP_LOGI(TAG, "Latest version: %s", latest_ver);
                }
                
                // Find the firmware binary in assets
                cJSON* assets = cJSON_GetObjectItem(root, "assets");
                if (assets && cJSON_IsArray(assets)) {
                    int asset_count = cJSON_GetArraySize(assets);
                    for (int i = 0; i < asset_count; i++) {
                        cJSON* asset = cJSON_GetArrayItem(assets, i);
                        cJSON* name = cJSON_GetObjectItem(asset, "name");
                        if (name && name->valuestring) {
                            // Look for arctic_controller.bin
                            if (strstr(name->valuestring, "arctic_controller.bin")) {
                                cJSON* url = cJSON_GetObjectItem(asset, "browser_download_url");
                                if (url && url->valuestring) {
                                    strncpy(download_url, url->valuestring, sizeof(download_url) - 1);
                                    ESP_LOGI(TAG, "Download URL: %s", download_url);
                                }
                                break;
                            }
                        }
                    }
                }
                
                cJSON_Delete(root);
            } else {
                const char* error_ptr = cJSON_GetErrorPtr();
                ESP_LOGE(TAG, "JSON parse failed! Error at: %.50s", error_ptr ? error_ptr : "(unknown)");
                ESP_LOGE(TAG, "Buffer length: %d, Buffer start: %.100s", http_response_len, http_response_buffer ? http_response_buffer : "(null)");
                // Check for non-printable characters that might corrupt JSON
                if (http_response_buffer) {
                    for (int i = 0; i < http_response_len && i < 20; i++) {
                        if (http_response_buffer[i] < 32 && http_response_buffer[i] != '\n' && http_response_buffer[i] != '\r' && http_response_buffer[i] != '\t') {
                            ESP_LOGE(TAG, "Non-printable char at pos %d: 0x%02X", i, (unsigned char)http_response_buffer[i]);
                        }
                    }
                }
            }
        } else {
            ESP_LOGE(TAG, "No response buffer or bad status. Buffer: %p, Status: %d", http_response_buffer, status);
        }
    } else {
        ESP_LOGE(TAG, "GitHub API request failed: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
    
    if (http_response_buffer) {
        free(http_response_buffer);
        http_response_buffer = NULL;
    }
    
    // Compare versions
    if (strlen(latest_ver) > 0) {
        strncpy(settings_state.latest_version, latest_ver, sizeof(settings_state.latest_version) - 1);
        strncpy(settings_state.download_url, download_url, sizeof(settings_state.download_url) - 1);
        
        // Simple version comparison (assumes semantic versioning)
        if (strcmp(latest_ver, settings_state.current_version) > 0 && strlen(download_url) > 0) {
            update_available = true;
        }
    }
    
    // Update UI on main thread via async call
    if (settings_state.visible) {
        if (update_available) {
            settings_state.pending_state = UPDATE_STATE_UPDATE_AVAILABLE;
        } else if (strlen(latest_ver) > 0) {
            settings_state.pending_state = UPDATE_STATE_NO_UPDATE;
        } else {
            settings_state.pending_state = UPDATE_STATE_FAILED;
        }
        lv_async_call(async_update_ui_cb, NULL);
    }
    
    vTaskDelete(NULL);
}

// Async callback for thread-safe UI updates from background tasks
static void async_update_ui_cb(void* arg)
{
    (void)arg;
    if (settings_state.visible) {
        update_ui_state(settings_state.pending_state);
    }
}

// ============================================================================
// Progress Timer
// ============================================================================

static void progress_timer_cb(lv_timer_t* timer)
{
    if (!settings_state.visible) {
        lv_timer_delete(timer);
        settings_state.progress_timer = NULL;
        return;
    }
    
    ota_status_t status = ota_mgr_get_status();
    
    switch (status.state) {
        case OTA_STATE_DOWNLOADING:
            lv_bar_set_value(settings_state.progress_bar, status.progress_percent, LV_ANIM_ON);
            {
                char buf[64];
                if (status.total_bytes > 0) {
                    snprintf(buf, sizeof(buf), "Downloading: %d%% (%zu KB / %zu KB)", 
                             status.progress_percent,
                             status.bytes_downloaded / 1024,
                             status.total_bytes / 1024);
                } else {
                    snprintf(buf, sizeof(buf), "Downloading: %zu KB", 
                             status.bytes_downloaded / 1024);
                }
                lv_label_set_text(settings_state.progress_label, buf);
            }
            break;
            
        case OTA_STATE_VERIFYING:
            lv_bar_set_value(settings_state.progress_bar, 100, LV_ANIM_ON);
            lv_label_set_text(settings_state.progress_label, "Verifying firmware...");
            break;
            
        case OTA_STATE_READY_TO_REBOOT:
            lv_timer_delete(timer);
            settings_state.progress_timer = NULL;
            update_ui_state(UPDATE_STATE_READY_TO_REBOOT);
            break;
            
        case OTA_STATE_FAILED:
            lv_timer_delete(timer);
            settings_state.progress_timer = NULL;
            lv_label_set_text(settings_state.status_label, status.error_msg);
            update_ui_state(UPDATE_STATE_FAILED);
            break;
            
        default:
            break;
    }
}

// ============================================================================
// UI State Management
// ============================================================================

static void update_ui_state(update_ui_state_t new_state)
{
    settings_state.update_state = new_state;
    
    // Hide all dynamic elements first
    lv_obj_add_flag(settings_state.progress_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(settings_state.progress_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(settings_state.update_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_state(settings_state.check_btn, LV_STATE_DISABLED);
    
    switch (new_state) {
        case UPDATE_STATE_IDLE:
            lv_label_set_text(settings_state.latest_version_label, "Latest: --");
            lv_label_set_text(settings_state.status_label, "");
            break;
            
        case UPDATE_STATE_CHECKING:
            lv_label_set_text(settings_state.latest_version_label, "Latest: Checking...");
            lv_label_set_text(settings_state.status_label, "Checking for updates...");
            lv_obj_add_state(settings_state.check_btn, LV_STATE_DISABLED);
            break;
            
        case UPDATE_STATE_UPDATE_AVAILABLE:
            {
                char buf[64];
                snprintf(buf, sizeof(buf), "Latest: %s", settings_state.latest_version);
                lv_label_set_text(settings_state.latest_version_label, buf);
            }
            lv_label_set_text(settings_state.status_label, LV_SYMBOL_DOWNLOAD " Update available!");
            lv_obj_set_style_text_color(settings_state.status_label, COLOR_SUCCESS, LV_PART_MAIN);
            lv_obj_remove_flag(settings_state.update_btn, LV_OBJ_FLAG_HIDDEN);
            break;
            
        case UPDATE_STATE_NO_UPDATE:
            {
                char buf[64];
                snprintf(buf, sizeof(buf), "Latest: %s", settings_state.latest_version);
                lv_label_set_text(settings_state.latest_version_label, buf);
            }
            lv_label_set_text(settings_state.status_label, LV_SYMBOL_OK " You're up to date!");
            lv_obj_set_style_text_color(settings_state.status_label, COLOR_SUCCESS, LV_PART_MAIN);
            break;
            
        case UPDATE_STATE_DOWNLOADING:
            lv_label_set_text(settings_state.status_label, "Downloading update...");
            lv_obj_set_style_text_color(settings_state.status_label, COLOR_ACCENT, LV_PART_MAIN);
            lv_obj_remove_flag(settings_state.progress_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(settings_state.progress_label, LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(settings_state.progress_bar, 0, LV_ANIM_OFF);
            lv_obj_add_state(settings_state.check_btn, LV_STATE_DISABLED);
            break;
            
        case UPDATE_STATE_READY_TO_REBOOT:
            lv_label_set_text(settings_state.status_label, LV_SYMBOL_OK " Update complete! Rebooting...");
            lv_obj_set_style_text_color(settings_state.status_label, COLOR_SUCCESS, LV_PART_MAIN);
            lv_bar_set_value(settings_state.progress_bar, 100, LV_ANIM_OFF);
            lv_obj_remove_flag(settings_state.progress_bar, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(settings_state.progress_label, "Rebooting in 3 seconds...");
            lv_obj_remove_flag(settings_state.progress_label, LV_OBJ_FLAG_HIDDEN);
            // Auto-reboot handled by OTA manager
            break;
            
        case UPDATE_STATE_FAILED:
            lv_label_set_text(settings_state.status_label, LV_SYMBOL_WARNING " Update check failed");
            lv_obj_set_style_text_color(settings_state.status_label, COLOR_ERROR, LV_PART_MAIN);
            break;
    }
}

// ============================================================================
// UI Creation
// ============================================================================

static void create_header(void)
{
    settings_state.header = lv_obj_create(settings_state.screen);
    lv_obj_set_size(settings_state.header, LV_PCT(100), 60);
    lv_obj_align(settings_state.header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(settings_state.header, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(settings_state.header, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(settings_state.header, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(settings_state.header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(settings_state.header, 10, LV_PART_MAIN);
    lv_obj_remove_flag(settings_state.header, LV_OBJ_FLAG_SCROLLABLE);
    
    // Title
    lv_obj_t* title = lv_label_create(settings_state.header);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 10, 0);
    
    // Close button
    lv_obj_t* close_btn = lv_btn_create(settings_state.header);
    lv_obj_set_size(close_btn, 40, 40);
    lv_obj_align(close_btn, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(close_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(close_btn, close_btn_event_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t* close_icon = lv_label_create(close_btn);
    lv_label_set_text(close_icon, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(close_icon, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(close_icon, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_center(close_icon);
}

static void create_firmware_section(void)
{
    // Get current version from OTA manager
    ota_status_t status = ota_mgr_get_status();
    strncpy(settings_state.current_version, status.current_version, sizeof(settings_state.current_version) - 1);
    
    // Section container - use full width
    settings_state.fw_section = lv_obj_create(settings_state.content);
    lv_obj_set_size(settings_state.fw_section, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(settings_state.fw_section, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(settings_state.fw_section, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(settings_state.fw_section, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(settings_state.fw_section, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(settings_state.fw_section, 30, LV_PART_MAIN);
    lv_obj_set_flex_flow(settings_state.fw_section, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(settings_state.fw_section, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(settings_state.fw_section, 20, LV_PART_MAIN);
    
    // Section title - larger font
    lv_obj_t* section_title = lv_label_create(settings_state.fw_section);
    lv_label_set_text(section_title, LV_SYMBOL_DOWNLOAD " Firmware Update");
    lv_obj_set_style_text_font(section_title, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(section_title, COLOR_ACCENT, LV_PART_MAIN);
    
    // Current version - larger font
    settings_state.current_version_label = lv_label_create(settings_state.fw_section);
    char buf[64];
    snprintf(buf, sizeof(buf), "Current: %s", 
             strlen(settings_state.current_version) > 0 ? settings_state.current_version : "Unknown");
    lv_label_set_text(settings_state.current_version_label, buf);
    lv_obj_set_style_text_font(settings_state.current_version_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(settings_state.current_version_label, COLOR_TEXT, LV_PART_MAIN);
    
    // Latest version - larger font
    settings_state.latest_version_label = lv_label_create(settings_state.fw_section);
    lv_label_set_text(settings_state.latest_version_label, "Latest: --");
    lv_obj_set_style_text_font(settings_state.latest_version_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(settings_state.latest_version_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    
    // Status label - larger font
    settings_state.status_label = lv_label_create(settings_state.fw_section);
    lv_label_set_text(settings_state.status_label, "");
    lv_obj_set_style_text_font(settings_state.status_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(settings_state.status_label, COLOR_TEXT, LV_PART_MAIN);
    
    // Progress bar (hidden initially) - larger
    settings_state.progress_bar = lv_bar_create(settings_state.fw_section);
    lv_obj_set_size(settings_state.progress_bar, LV_PCT(100), 30);
    lv_bar_set_range(settings_state.progress_bar, 0, 100);
    lv_bar_set_value(settings_state.progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(settings_state.progress_bar, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(settings_state.progress_bar, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(settings_state.progress_bar, 15, LV_PART_MAIN);
    lv_obj_set_style_radius(settings_state.progress_bar, 15, LV_PART_INDICATOR);
    lv_obj_add_flag(settings_state.progress_bar, LV_OBJ_FLAG_HIDDEN);
    
    // Progress label (hidden initially) - larger font
    settings_state.progress_label = lv_label_create(settings_state.fw_section);
    lv_label_set_text(settings_state.progress_label, "");
    lv_obj_set_style_text_font(settings_state.progress_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(settings_state.progress_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_add_flag(settings_state.progress_label, LV_OBJ_FLAG_HIDDEN);
    
    // Button container
    lv_obj_t* btn_row = lv_obj_create(settings_state.fw_section);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_top(btn_row, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_row(btn_row, 15, LV_PART_MAIN);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    
    // Check for updates button - larger
    settings_state.check_btn = lv_btn_create(btn_row);
    lv_obj_set_size(settings_state.check_btn, 280, 60);
    lv_obj_set_style_bg_color(settings_state.check_btn, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_radius(settings_state.check_btn, 12, LV_PART_MAIN);
    lv_obj_add_event_cb(settings_state.check_btn, check_btn_event_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t* check_lbl = lv_label_create(settings_state.check_btn);
    lv_label_set_text(check_lbl, LV_SYMBOL_REFRESH " Check for Updates");
    lv_obj_set_style_text_font(check_lbl, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_center(check_lbl);
    
    // Update button (hidden initially) - larger
    settings_state.update_btn = lv_btn_create(btn_row);
    lv_obj_set_size(settings_state.update_btn, 220, 60);
    lv_obj_set_style_bg_color(settings_state.update_btn, COLOR_SUCCESS, LV_PART_MAIN);
    lv_obj_set_style_radius(settings_state.update_btn, 12, LV_PART_MAIN);
    lv_obj_add_event_cb(settings_state.update_btn, update_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(settings_state.update_btn, LV_OBJ_FLAG_HIDDEN);
    
    lv_obj_t* update_lbl = lv_label_create(settings_state.update_btn);
    lv_label_set_text(update_lbl, LV_SYMBOL_DOWNLOAD " Install Update");
    lv_obj_set_style_text_font(update_lbl, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_center(update_lbl);
    
    // Note: Reboot is now automatic after OTA completes (3 second delay)
}

// ============================================================================
// Public API
// ============================================================================

void settings_screen_create(const settings_screen_config_t* config)
{
    if (settings_state.visible) {
        return;
    }
    
    ESP_LOGI(TAG, "Creating settings screen");
    
    // Store config
    if (config) {
        settings_state.config = *config;
    } else {
        memset(&settings_state.config, 0, sizeof(settings_state.config));
    }
    
    settings_state.visible = true;
    settings_state.update_state = UPDATE_STATE_IDLE;
    memset(settings_state.latest_version, 0, sizeof(settings_state.latest_version));
    memset(settings_state.download_url, 0, sizeof(settings_state.download_url));
    
    // Create screen
    settings_state.screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(settings_state.screen, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(settings_state.screen, LV_OPA_COVER, LV_PART_MAIN);
    
    // Create header
    create_header();
    
    // Content area (scrollable)
    settings_state.content = lv_obj_create(settings_state.screen);
    lv_obj_set_size(settings_state.content, LV_PCT(100), LV_PCT(100));
    lv_obj_align(settings_state.content, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_opa(settings_state.content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(settings_state.content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(settings_state.content, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(settings_state.content, 80, LV_PART_MAIN);  // Extra for scrolling
    lv_obj_set_flex_flow(settings_state.content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(settings_state.content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(settings_state.content, 20, LV_PART_MAIN);
    
    // Create sections
    create_firmware_section();
    
    // Load screen
    lv_screen_load_anim(settings_state.screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}

void settings_screen_close(void)
{
    if (!settings_state.visible) {
        return;
    }
    
    ESP_LOGI(TAG, "Closing settings screen");
    
    // Stop progress timer if running
    if (settings_state.progress_timer) {
        lv_timer_delete(settings_state.progress_timer);
        settings_state.progress_timer = NULL;
    }
    
    settings_state.visible = false;
    
    if (settings_state.screen) {
        lv_obj_delete(settings_state.screen);
        settings_state.screen = NULL;
    }
}

bool settings_screen_is_visible(void)
{
    return settings_state.visible;
}

// ============================================================================
// Background Update Check (can be called without UI)
// ============================================================================

static update_check_cb_t s_update_check_callback = NULL;

static void background_update_check_task(void* arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Background check for updates...");
    
    bool update_available = false;
    char latest_ver[32] = {0};
    char* response_buffer = NULL;
    int response_len = 0;
    
    // Allocate buffer from PSRAM
    response_buffer = (char*)heap_caps_calloc(1, HTTP_RESPONSE_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (!response_buffer) {
        ESP_LOGE(TAG, "Failed to allocate response buffer");
        goto done;
    }
    
    {
        esp_http_client_config_t config = {};
        config.url = GITHUB_API_URL;
        config.timeout_ms = GITHUB_API_TIMEOUT_MS;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.buffer_size = 4096;
        config.buffer_size_tx = 2048;
        
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGE(TAG, "Failed to init HTTP client");
            goto done;
        }
        
        esp_http_client_set_header(client, "Accept", "application/vnd.github.v3+json");
        esp_http_client_set_header(client, "User-Agent", "ESP32-Arctic-Controller");
        
        esp_err_t err = esp_http_client_open(client, 0);
        if (err == ESP_OK) {
            int content_len = esp_http_client_fetch_headers(client);
            int status = esp_http_client_get_status_code(client);
            
            if (status == 200 && content_len > 0 && content_len < HTTP_RESPONSE_BUFFER_SIZE) {
                response_len = esp_http_client_read_response(client, response_buffer, HTTP_RESPONSE_BUFFER_SIZE - 1);
                response_buffer[response_len] = '\0';
            }
        }
        
        esp_http_client_cleanup(client);
    }
    
    // Parse response
    if (response_len > 0) {
        cJSON* root = cJSON_Parse(response_buffer);
        if (root) {
            cJSON* tag_name = cJSON_GetObjectItem(root, "tag_name");
            if (tag_name && cJSON_IsString(tag_name)) {
                const char* version = tag_name->valuestring;
                // Skip 'v' prefix if present
                if (version[0] == 'v' || version[0] == 'V') {
                    version++;
                }
                strncpy(latest_ver, version, sizeof(latest_ver) - 1);
                
                // Compare with current version
                const esp_app_desc_t* app_desc = esp_app_get_description();
                if (app_desc && strcmp(latest_ver, app_desc->version) > 0) {
                    update_available = true;
                }
            }
            cJSON_Delete(root);
        }
    }

done:
    if (response_buffer) {
        free(response_buffer);
    }
    
    // Call callback if set
    if (s_update_check_callback) {
        s_update_check_callback(update_available, latest_ver);
        s_update_check_callback = NULL;
    }
    
    if (update_available) {
        ESP_LOGI(TAG, "Update available: %s", latest_ver);
    } else {
        ESP_LOGI(TAG, "No updates available");
    }
    
    vTaskDelete(NULL);
}

void settings_screen_check_for_updates_async(update_check_cb_t callback)
{
    s_update_check_callback = callback;
    xTaskCreate(background_update_check_task, "bg_update_chk", 8192, NULL, 5, NULL);
}
