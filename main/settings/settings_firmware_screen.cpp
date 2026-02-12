/*
 * Arctic Heat Pump Controller
 * Settings - Firmware Screen Implementation (iOS-style full screen)
 * 
 * Full-screen firmware/OTA update with back navigation.
 * Portrait mode: 720x1280
 */
#include "settings_firmware_screen.h"
#include "settings_menu.h"
#include "settings_common.h"
#include "ota_manager.h"
#include "i18n/i18n.h"
#include "fonts/fonts.h"
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <stdio.h>

static const char* TAG = "firmware_screen";

// GitHub release API URL
#define GITHUB_API_URL "https://api.github.com/repos/sslivins/arctic-controller/releases/latest"
#define GITHUB_API_TIMEOUT_MS 10000
#define HTTP_RESPONSE_BUFFER_SIZE 16384

// ============================================================================
// Screen State
// ============================================================================

typedef enum {
    FW_STATE_IDLE,
    FW_STATE_CHECKING,
    FW_STATE_UPDATE_AVAILABLE,
    FW_STATE_NO_UPDATE,
    FW_STATE_DOWNLOADING,
    FW_STATE_READY_TO_REBOOT,
    FW_STATE_FAILED
} fw_ui_state_t;

typedef struct {
    bool visible;
    firmware_screen_config_t config;
    
    // Screen objects
    lv_obj_t* screen;
    lv_obj_t* header;
    lv_obj_t* back_btn;
    lv_obj_t* content;
    
    // Firmware info
    lv_obj_t* current_version_label;
    lv_obj_t* latest_version_label;
    lv_obj_t* status_label;
    lv_obj_t* progress_bar;
    lv_obj_t* progress_label;
    lv_obj_t* update_btn;
    
    // State
    fw_ui_state_t state;
    char current_version[32];
    char latest_version[32];
    char download_url[256];
    lv_timer_t* progress_timer;
    
} firmware_screen_state_t;

static firmware_screen_state_t s_state = {};

// HTTP response buffer
static char* http_response_buffer = NULL;
static int http_response_len = 0;

// ============================================================================
// Forward Declarations
// ============================================================================

static void create_header(void);
static void create_content(void);
static void back_btn_cb(lv_event_t* e);
static void update_btn_cb(lv_event_t* e);
static void update_ui_state(fw_ui_state_t new_state);
static void check_for_updates_task(void* arg);
static void async_update_ui_cb(void* arg);
static void progress_timer_cb(lv_timer_t* timer);

// ============================================================================
// HTTP Handler
// ============================================================================

static esp_err_t http_event_handler(esp_http_client_event_t* evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (http_response_buffer == NULL) {
                http_response_buffer = (char*)heap_caps_calloc(1, HTTP_RESPONSE_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                http_response_len = 0;
            }
            if (http_response_buffer && (http_response_len + evt->data_len) < (HTTP_RESPONSE_BUFFER_SIZE - 1)) {
                memcpy(http_response_buffer + http_response_len, evt->data, evt->data_len);
                http_response_len += evt->data_len;
                http_response_buffer[http_response_len] = '\0';
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

// ============================================================================
// Public API
// ============================================================================

void firmware_screen_create(const firmware_screen_config_t* config)
{
    if (s_state.visible) {
        ESP_LOGW(TAG, "Firmware screen already visible");
        return;
    }
    
    ESP_LOGI(TAG, "Creating Firmware screen");
    
    memset(&s_state, 0, sizeof(s_state));
    if (config) {
        s_state.config = *config;
    }
    
    // Get current version
    ota_status_t ota_status = ota_mgr_get_status();
    strncpy(s_state.current_version, ota_status.current_version, sizeof(s_state.current_version) - 1);
    
    // Create screen
    s_state.screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_state.screen, COLOR_BG, LV_PART_MAIN);
    disable_scrolling(s_state.screen);
    
    create_header();
    create_content();
    
    // Load screen with slide-left animation
    lv_screen_load_anim(s_state.screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
    s_state.visible = true;
    
    // Start checking for updates
    update_ui_state(FW_STATE_CHECKING);
    xTaskCreate(check_for_updates_task, "fw_check", 8192, NULL, 5, NULL);
}

void firmware_screen_close(void)
{
    if (!s_state.visible) return;
    
    ESP_LOGI(TAG, "Closing Firmware screen");
    
    if (s_state.progress_timer) {
        lv_timer_delete(s_state.progress_timer);
        s_state.progress_timer = NULL;
    }
    
    s_state.visible = false;
    s_state.screen = NULL;
}

bool firmware_screen_is_visible(void)
{
    return s_state.visible;
}

// ============================================================================
// UI Creation
// ============================================================================

static void create_header(void)
{
    lv_display_t* disp = lv_display_get_default();
    int32_t header_height = lv_display_get_vertical_resolution(disp) * HEADER_HEIGHT_PCT / 100;
    
    s_state.header = lv_obj_create(s_state.screen);
    lv_obj_set_size(s_state.header, LV_PCT(100), header_height);
    lv_obj_align(s_state.header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_state.header, COLOR_HEADER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_state.header, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_state.header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_state.header, 15, LV_PART_MAIN);
    disable_scrolling(s_state.header);
    
    // Back button with circular background
    s_state.back_btn = lv_btn_create(s_state.header);
    lv_obj_set_size(s_state.back_btn, 50, 50);
    lv_obj_align(s_state.back_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_state.back_btn, lv_color_hex(0x3d4f6f), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_state.back_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_state.back_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_state.back_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_state.back_btn, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_state.back_btn, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_state.back_btn, LV_OPA_50, LV_PART_MAIN);
    lv_obj_add_event_cb(s_state.back_btn, back_btn_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t* back_icon = lv_label_create(s_state.back_btn);
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_icon, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(back_icon, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_center(back_icon);
    
    // Title
    lv_obj_t* title = lv_label_create(s_state.header);
    lv_label_set_text(title, i18n_get(STR_SETTINGS_UPDATE));
    lv_obj_set_style_text_font(title, UI_FONT_HEADER, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
}

static void create_content(void)
{
    lv_display_t* disp = lv_display_get_default();
    int32_t screen_height = lv_display_get_vertical_resolution(disp);
    int32_t header_height = screen_height * HEADER_HEIGHT_PCT / 100;
    
    s_state.content = lv_obj_create(s_state.screen);
    lv_obj_set_size(s_state.content, LV_PCT(100), screen_height - header_height);
    lv_obj_set_pos(s_state.content, 0, header_height);
    lv_obj_set_style_bg_opa(s_state.content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_state.content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_state.content, 20, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_state.content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_state.content, 20, LV_PART_MAIN);
    disable_scrolling(s_state.content);
    
    // Firmware card
    lv_obj_t* card = lv_obj_create(s_state.content);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 25, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 20, LV_PART_MAIN);
    disable_scrolling(card);
    
    // Title
    lv_obj_t* title = lv_label_create(card);
    char buf[64];
    snprintf(buf, sizeof(buf), LV_SYMBOL_DOWNLOAD " %s", i18n_get(STR_FW_TITLE));
    lv_label_set_text(title, buf);
    lv_obj_set_style_text_font(title, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_ACCENT, LV_PART_MAIN);
    
    // Current version
    s_state.current_version_label = lv_label_create(card);
    snprintf(buf, sizeof(buf), "%s: %s", i18n_get(STR_FW_CURRENT), 
             strlen(s_state.current_version) > 0 ? s_state.current_version : "?");
    lv_label_set_text(s_state.current_version_label, buf);
    lv_obj_set_style_text_font(s_state.current_version_label, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_state.current_version_label, COLOR_TEXT, LV_PART_MAIN);
    
    // Latest version
    s_state.latest_version_label = lv_label_create(card);
    snprintf(buf, sizeof(buf), "%s: --", i18n_get(STR_FW_LATEST));
    lv_label_set_text(s_state.latest_version_label, buf);
    lv_obj_set_style_text_font(s_state.latest_version_label, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_state.latest_version_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    
    // Status
    s_state.status_label = lv_label_create(card);
    lv_label_set_text(s_state.status_label, "");
    lv_obj_set_style_text_font(s_state.status_label, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_state.status_label, COLOR_TEXT, LV_PART_MAIN);
    
    // Progress bar
    s_state.progress_bar = lv_bar_create(card);
    lv_obj_set_size(s_state.progress_bar, LV_PCT(100), 25);
    lv_bar_set_range(s_state.progress_bar, 0, 100);
    lv_bar_set_value(s_state.progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_state.progress_bar, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_state.progress_bar, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_state.progress_bar, 12, LV_PART_MAIN);
    lv_obj_set_style_radius(s_state.progress_bar, 12, LV_PART_INDICATOR);
    lv_obj_add_flag(s_state.progress_bar, LV_OBJ_FLAG_HIDDEN);
    
    // Progress label
    s_state.progress_label = lv_label_create(card);
    lv_label_set_text(s_state.progress_label, "");
    lv_obj_set_style_text_font(s_state.progress_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_state.progress_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_add_flag(s_state.progress_label, LV_OBJ_FLAG_HIDDEN);
    
    // Update button
    s_state.update_btn = lv_btn_create(card);
    lv_obj_set_size(s_state.update_btn, 250, 60);
    lv_obj_set_style_bg_color(s_state.update_btn, COLOR_SUCCESS, LV_PART_MAIN);
    lv_obj_set_style_radius(s_state.update_btn, 10, LV_PART_MAIN);
    lv_obj_add_event_cb(s_state.update_btn, update_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_state.update_btn, LV_OBJ_FLAG_HIDDEN);
    
    lv_obj_t* btn_label = lv_label_create(s_state.update_btn);
    snprintf(buf, sizeof(buf), LV_SYMBOL_DOWNLOAD " %s", i18n_get(STR_FW_INSTALL_UPDATE));
    lv_label_set_text(btn_label, buf);
    lv_obj_set_style_text_font(btn_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_center(btn_label);
}

// ============================================================================
// Event Handlers
// ============================================================================

static void back_btn_cb(lv_event_t* e)
{
    (void)e;
    ESP_LOGI(TAG, "Back button clicked");
    
    void (*on_back)(void) = s_state.config.on_back;
    
    if (on_back) {
        on_back();
    } else {
        settings_menu_show();
    }
    
    firmware_screen_close();
}

static void update_btn_cb(lv_event_t* e)
{
    (void)e;
    ESP_LOGI(TAG, "Update button clicked, URL: %s", s_state.download_url);
    
    if (strlen(s_state.download_url) > 0) {
        if (ota_mgr_start_update(s_state.download_url)) {
            update_ui_state(FW_STATE_DOWNLOADING);
            
            if (!s_state.progress_timer) {
                s_state.progress_timer = lv_timer_create(progress_timer_cb, 200, NULL);
            }
        } else {
            lv_label_set_text(s_state.status_label, i18n_get(STR_FW_UPDATE_FAILED));
            update_ui_state(FW_STATE_FAILED);
        }
    }
}

// ============================================================================
// UI State Management
// ============================================================================

static void update_ui_state(fw_ui_state_t new_state)
{
    s_state.state = new_state;
    char buf[64];
    
    // Hide progress elements by default
    lv_obj_add_flag(s_state.progress_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_state.progress_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_state.update_btn, LV_OBJ_FLAG_HIDDEN);
    
    switch (new_state) {
        case FW_STATE_IDLE:
            snprintf(buf, sizeof(buf), "%s: --", i18n_get(STR_FW_LATEST));
            lv_label_set_text(s_state.latest_version_label, buf);
            lv_label_set_text(s_state.status_label, "");
            break;
            
        case FW_STATE_CHECKING:
            lv_label_set_text(s_state.latest_version_label, i18n_get(STR_FW_CHECKING));
            lv_label_set_text(s_state.status_label, i18n_get(STR_FW_CHECKING));
            break;
            
        case FW_STATE_UPDATE_AVAILABLE:
            snprintf(buf, sizeof(buf), "%s: %s", i18n_get(STR_FW_LATEST), s_state.latest_version);
            lv_label_set_text(s_state.latest_version_label, buf);
            snprintf(buf, sizeof(buf), LV_SYMBOL_DOWNLOAD " %s", i18n_get(STR_FW_UPDATE_AVAILABLE));
            lv_label_set_text(s_state.status_label, buf);
            lv_obj_set_style_text_color(s_state.status_label, COLOR_SUCCESS, LV_PART_MAIN);
            lv_obj_remove_flag(s_state.update_btn, LV_OBJ_FLAG_HIDDEN);
            break;
            
        case FW_STATE_NO_UPDATE:
            snprintf(buf, sizeof(buf), "%s: %s", i18n_get(STR_FW_LATEST), s_state.latest_version);
            lv_label_set_text(s_state.latest_version_label, buf);
            snprintf(buf, sizeof(buf), LV_SYMBOL_OK " %s", i18n_get(STR_FW_UP_TO_DATE));
            lv_label_set_text(s_state.status_label, buf);
            lv_obj_set_style_text_color(s_state.status_label, COLOR_SUCCESS, LV_PART_MAIN);
            break;
            
        case FW_STATE_DOWNLOADING:
            lv_label_set_text(s_state.status_label, i18n_get(STR_FW_DOWNLOADING));
            lv_obj_set_style_text_color(s_state.status_label, COLOR_ACCENT, LV_PART_MAIN);
            lv_obj_remove_flag(s_state.progress_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_state.progress_label, LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(s_state.progress_bar, 0, LV_ANIM_OFF);
            break;
            
        case FW_STATE_READY_TO_REBOOT:
            snprintf(buf, sizeof(buf), LV_SYMBOL_OK " %s", i18n_get(STR_FW_UPDATE_COMPLETE));
            lv_label_set_text(s_state.status_label, buf);
            lv_obj_set_style_text_color(s_state.status_label, COLOR_SUCCESS, LV_PART_MAIN);
            lv_bar_set_value(s_state.progress_bar, 100, LV_ANIM_OFF);
            lv_obj_remove_flag(s_state.progress_bar, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_state.progress_label, i18n_get(STR_FW_REBOOTING));
            lv_obj_remove_flag(s_state.progress_label, LV_OBJ_FLAG_HIDDEN);
            break;
            
        case FW_STATE_FAILED:
            snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING " %s", i18n_get(STR_FW_CHECK_FAILED));
            lv_label_set_text(s_state.status_label, buf);
            lv_obj_set_style_text_color(s_state.status_label, COLOR_ERROR, LV_PART_MAIN);
            break;
    }
}

// ============================================================================
// Background Tasks
// ============================================================================

static fw_ui_state_t pending_state = FW_STATE_IDLE;

static void async_update_ui_cb(void* arg)
{
    (void)arg;
    if (s_state.visible) {
        update_ui_state(pending_state);
    }
}

static void check_for_updates_task(void* arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Checking for updates from GitHub...");
    
    bool update_available = false;
    char latest_ver[32] = {0};
    char download_url[256] = {0};
    
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
    esp_http_client_set_header(client, "User-Agent", "arctic-controller/1.0");
    esp_http_client_set_header(client, "Accept", "application/vnd.github.v3+json");
    
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        
        if (status == 200 && http_response_buffer && http_response_len > 0) {
            http_response_buffer[http_response_len] = '\0';
            vTaskDelay(pdMS_TO_TICKS(10));
            
            cJSON* root = cJSON_Parse(http_response_buffer);
            if (root) {
                cJSON* tag = cJSON_GetObjectItem(root, "tag_name");
                if (tag && tag->valuestring) {
                    const char* ver = tag->valuestring;
                    if (ver[0] == 'v' || ver[0] == 'V') ver++;
                    strncpy(latest_ver, ver, sizeof(latest_ver) - 1);
                }
                
                cJSON* assets = cJSON_GetObjectItem(root, "assets");
                if (assets && cJSON_IsArray(assets)) {
                    int asset_count = cJSON_GetArraySize(assets);
                    for (int i = 0; i < asset_count; i++) {
                        cJSON* asset = cJSON_GetArrayItem(assets, i);
                        cJSON* name = cJSON_GetObjectItem(asset, "name");
                        if (name && name->valuestring && strstr(name->valuestring, "arctic_controller.bin")) {
                            cJSON* url = cJSON_GetObjectItem(asset, "browser_download_url");
                            if (url && url->valuestring) {
                                strncpy(download_url, url->valuestring, sizeof(download_url) - 1);
                            }
                            break;
                        }
                    }
                }
                cJSON_Delete(root);
            }
        }
    }
    
    esp_http_client_cleanup(client);
    
    if (http_response_buffer) {
        free(http_response_buffer);
        http_response_buffer = NULL;
    }
    
    if (strlen(latest_ver) > 0) {
        strncpy(s_state.latest_version, latest_ver, sizeof(s_state.latest_version) - 1);
        strncpy(s_state.download_url, download_url, sizeof(s_state.download_url) - 1);
        
        if (strcmp(latest_ver, s_state.current_version) > 0 && strlen(download_url) > 0) {
            update_available = true;
        }
    }
    
    if (s_state.visible) {
        if (update_available) {
            pending_state = FW_STATE_UPDATE_AVAILABLE;
        } else if (strlen(latest_ver) > 0) {
            pending_state = FW_STATE_NO_UPDATE;
        } else {
            pending_state = FW_STATE_FAILED;
        }
        lv_async_call(async_update_ui_cb, NULL);
    }
    
    vTaskDelete(NULL);
}

static void progress_timer_cb(lv_timer_t* timer)
{
    if (!s_state.visible) {
        lv_timer_delete(timer);
        s_state.progress_timer = NULL;
        return;
    }
    
    ota_status_t status = ota_mgr_get_status();
    char buf[64];
    
    switch (status.state) {
        case OTA_STATE_DOWNLOADING:
            lv_bar_set_value(s_state.progress_bar, status.progress_percent, LV_ANIM_ON);
            if (status.total_bytes > 0) {
                snprintf(buf, sizeof(buf), "%s: %d%% (%lu KB / %lu KB)", 
                         i18n_get(STR_FW_DOWNLOADING), status.progress_percent, 
                         (unsigned long)(status.bytes_downloaded / 1024), 
                         (unsigned long)(status.total_bytes / 1024));
            } else {
                snprintf(buf, sizeof(buf), "%s: %lu KB", 
                         i18n_get(STR_FW_DOWNLOADING), 
                         (unsigned long)(status.bytes_downloaded / 1024));
            }
            lv_label_set_text(s_state.progress_label, buf);
            break;
            
        case OTA_STATE_VERIFYING:
            lv_bar_set_value(s_state.progress_bar, 100, LV_ANIM_ON);
            lv_label_set_text(s_state.progress_label, i18n_get(STR_FW_VERIFYING));
            break;
            
        case OTA_STATE_READY_TO_REBOOT:
            lv_timer_delete(timer);
            s_state.progress_timer = NULL;
            update_ui_state(FW_STATE_READY_TO_REBOOT);
            break;
            
        case OTA_STATE_FAILED:
            lv_timer_delete(timer);
            s_state.progress_timer = NULL;
            lv_label_set_text(s_state.status_label, status.error_msg);
            update_ui_state(FW_STATE_FAILED);
            break;
            
        default:
            break;
    }
}

// ============================================================================
// Background Update Check (for status bar notifications)
// ============================================================================

static firmware_update_check_cb_t s_bg_update_callback = NULL;
static volatile bool s_bg_check_running = false;

static void background_update_check_task(void* arg)
{
    (void)arg;
    s_bg_check_running = true;
    
    bool update_available = false;
    char latest_ver[32] = {0};
    
    // Allocate response buffer from PSRAM
    char* response_buffer = (char*)heap_caps_calloc(1, HTTP_RESPONSE_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (response_buffer) {
        esp_http_client_config_t config = {};
        config.url = GITHUB_API_URL;
        config.timeout_ms = GITHUB_API_TIMEOUT_MS;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.buffer_size = 4096;
        config.buffer_size_tx = 2048;
        
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client) {
            esp_http_client_set_header(client, "Accept", "application/vnd.github.v3+json");
            esp_http_client_set_header(client, "User-Agent", "ESP32-Arctic-Controller");
            
            esp_err_t err = esp_http_client_open(client, 0);
            if (err == ESP_OK) {
                int content_len = esp_http_client_fetch_headers(client);
                int status = esp_http_client_get_status_code(client);
                
                if (status == 200 && content_len > 0 && content_len < HTTP_RESPONSE_BUFFER_SIZE) {
                    int response_len = esp_http_client_read_response(client, response_buffer, HTTP_RESPONSE_BUFFER_SIZE - 1);
                    response_buffer[response_len] = '\0';
                    
                    cJSON* root = cJSON_Parse(response_buffer);
                    if (root) {
                        cJSON* tag_name = cJSON_GetObjectItem(root, "tag_name");
                        if (tag_name && cJSON_IsString(tag_name)) {
                            const char* version = tag_name->valuestring;
                            if (version[0] == 'v' || version[0] == 'V') version++;
                            strncpy(latest_ver, version, sizeof(latest_ver) - 1);
                            
                            const esp_app_desc_t* app_desc = esp_app_get_description();
                            if (app_desc && strcmp(latest_ver, app_desc->version) > 0) {
                                update_available = true;
                            }
                        }
                        cJSON_Delete(root);
                    }
                }
            }
            esp_http_client_cleanup(client);
        }
        free(response_buffer);
    }
    
    // Call callback
    if (s_bg_update_callback) {
        s_bg_update_callback(update_available, latest_ver);
        s_bg_update_callback = NULL;
    }
    
    s_bg_check_running = false;
    vTaskDelete(NULL);
}

void firmware_screen_check_for_updates_async(firmware_update_check_cb_t callback)
{
    if (s_bg_check_running) {
        ESP_LOGW(TAG, "Update check already in progress, skipping");
        return;
    }
    
    s_bg_update_callback = callback;
    xTaskCreate(background_update_check_task, "bg_fw_check", 8192, NULL, 5, NULL);
}
