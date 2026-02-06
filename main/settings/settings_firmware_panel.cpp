/*
 * Arctic Heat Pump Controller
 * Settings Screen - Firmware Panel Implementation
 */
#include "settings_firmware_panel.h"
#include "settings_common.h"
#include "ota_manager.h"
#include "i18n/i18n.h"
#include <string.h>
#include <stdio.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "settings_firmware";

// GitHub release API URL
#define GITHUB_API_URL "https://api.github.com/repos/sslivins/arctic-controller/releases/latest"
#define GITHUB_API_TIMEOUT_MS 10000
#define HTTP_RESPONSE_BUFFER_SIZE 16384

// ============================================================================
// Forward Declarations
// ============================================================================

static void update_btn_event_cb(lv_event_t* e);
static void check_for_updates_task(void* arg);
static void async_update_ui_cb(void* arg);
static void progress_timer_cb(lv_timer_t* timer);

// ============================================================================
// HTTP Response Handling
// ============================================================================

static char* http_response_buffer = NULL;
static int http_response_len = 0;

static esp_err_t http_event_handler(esp_http_client_event_t* evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
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
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

// ============================================================================
// Event Handlers
// ============================================================================

static void update_btn_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        settings_state_t* state = settings_get_state();
        ESP_LOGI(TAG, "Update clicked, URL: %s", state->download_url);
        
        if (strlen(state->download_url) > 0) {
            if (ota_mgr_start_update(state->download_url)) {
                firmware_update_ui_state(UPDATE_STATE_DOWNLOADING);
                
                if (!state->progress_timer) {
                    state->progress_timer = lv_timer_create(progress_timer_cb, 200, NULL);
                }
            } else {
                lv_label_set_text(state->fw_status_label, i18n_get(STR_FW_UPDATE_FAILED));
                firmware_update_ui_state(UPDATE_STATE_FAILED);
            }
        }
    }
}

// ============================================================================
// Update Check Task
// ============================================================================

static void check_for_updates_task(void* arg)
{
    settings_state_t* state = settings_get_state();
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
        strncpy(state->latest_version, latest_ver, sizeof(state->latest_version) - 1);
        strncpy(state->download_url, download_url, sizeof(state->download_url) - 1);
        
        if (strcmp(latest_ver, state->current_version) > 0 && strlen(download_url) > 0) {
            update_available = true;
        }
    }
    
    if (state->visible) {
        if (update_available) {
            state->pending_state = UPDATE_STATE_UPDATE_AVAILABLE;
        } else if (strlen(latest_ver) > 0) {
            state->pending_state = UPDATE_STATE_NO_UPDATE;
        } else {
            state->pending_state = UPDATE_STATE_FAILED;
        }
        lv_async_call(async_update_ui_cb, NULL);
    }
    
    vTaskDelete(NULL);
}

static void async_update_ui_cb(void* arg)
{
    (void)arg;
    settings_state_t* state = settings_get_state();
    if (state->visible) {
        firmware_update_ui_state(state->pending_state);
    }
}

static void progress_timer_cb(lv_timer_t* timer)
{
    settings_state_t* state = settings_get_state();
    
    if (!state->visible) {
        lv_timer_delete(timer);
        state->progress_timer = NULL;
        return;
    }
    
    ota_status_t status = ota_mgr_get_status();
    
    switch (status.state) {
        case OTA_STATE_DOWNLOADING:
            lv_bar_set_value(state->fw_progress_bar, status.progress_percent, LV_ANIM_ON);
            {
                char buf[64];
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
                lv_label_set_text(state->fw_progress_label, buf);
            }
            break;
            
        case OTA_STATE_VERIFYING:
            lv_bar_set_value(state->fw_progress_bar, 100, LV_ANIM_ON);
            lv_label_set_text(state->fw_progress_label, i18n_get(STR_FW_VERIFYING));
            break;
            
        case OTA_STATE_READY_TO_REBOOT:
            lv_timer_delete(timer);
            state->progress_timer = NULL;
            firmware_update_ui_state(UPDATE_STATE_READY_TO_REBOOT);
            break;
            
        case OTA_STATE_FAILED:
            lv_timer_delete(timer);
            state->progress_timer = NULL;
            lv_label_set_text(state->fw_status_label, status.error_msg);
            firmware_update_ui_state(UPDATE_STATE_FAILED);
            break;
            
        default:
            break;
    }
}

// ============================================================================
// Public Functions
// ============================================================================

void firmware_update_ui_state(update_ui_state_t new_state)
{
    settings_state_t* state = settings_get_state();
    state->update_state = new_state;
    
    lv_obj_add_flag(state->fw_progress_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(state->fw_progress_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(state->fw_update_btn, LV_OBJ_FLAG_HIDDEN);
    
    switch (new_state) {
        case UPDATE_STATE_IDLE:
            {
                char buf[48];
                snprintf(buf, sizeof(buf), "%s: --", i18n_get(STR_FW_LATEST));
                lv_label_set_text(state->fw_latest_version_label, buf);
            }
            lv_label_set_text(state->fw_status_label, "");
            break;
            
        case UPDATE_STATE_CHECKING:
            lv_label_set_text(state->fw_latest_version_label, i18n_get(STR_FW_CHECKING));
            lv_label_set_text(state->fw_status_label, i18n_get(STR_FW_CHECKING));
            break;
            
        case UPDATE_STATE_UPDATE_AVAILABLE:
            {
                char buf[64];
                snprintf(buf, sizeof(buf), "%s: %s", i18n_get(STR_FW_LATEST), state->latest_version);
                lv_label_set_text(state->fw_latest_version_label, buf);
            }
            {
                char buf[64];
                snprintf(buf, sizeof(buf), LV_SYMBOL_DOWNLOAD " %s", i18n_get(STR_FW_UPDATE_AVAILABLE));
                lv_label_set_text(state->fw_status_label, buf);
            }
            lv_obj_set_style_text_color(state->fw_status_label, COLOR_SUCCESS, LV_PART_MAIN);
            lv_obj_remove_flag(state->fw_update_btn, LV_OBJ_FLAG_HIDDEN);
            break;
            
        case UPDATE_STATE_NO_UPDATE:
            {
                char buf[64];
                snprintf(buf, sizeof(buf), "%s: %s", i18n_get(STR_FW_LATEST), state->latest_version);
                lv_label_set_text(state->fw_latest_version_label, buf);
            }
            {
                char buf[64];
                snprintf(buf, sizeof(buf), LV_SYMBOL_OK " %s", i18n_get(STR_FW_UP_TO_DATE));
                lv_label_set_text(state->fw_status_label, buf);
            }
            lv_obj_set_style_text_color(state->fw_status_label, COLOR_SUCCESS, LV_PART_MAIN);
            break;
            
        case UPDATE_STATE_DOWNLOADING:
            lv_label_set_text(state->fw_status_label, i18n_get(STR_FW_DOWNLOADING));
            lv_obj_set_style_text_color(state->fw_status_label, COLOR_ACCENT, LV_PART_MAIN);
            lv_obj_remove_flag(state->fw_progress_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(state->fw_progress_label, LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(state->fw_progress_bar, 0, LV_ANIM_OFF);
            break;
            
        case UPDATE_STATE_READY_TO_REBOOT:
            {
                char buf[96];
                snprintf(buf, sizeof(buf), LV_SYMBOL_OK " %s", i18n_get(STR_FW_UPDATE_COMPLETE));
                lv_label_set_text(state->fw_status_label, buf);
            }
            lv_obj_set_style_text_color(state->fw_status_label, COLOR_SUCCESS, LV_PART_MAIN);
            lv_bar_set_value(state->fw_progress_bar, 100, LV_ANIM_OFF);
            lv_obj_remove_flag(state->fw_progress_bar, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(state->fw_progress_label, i18n_get(STR_FW_REBOOTING));
            lv_obj_remove_flag(state->fw_progress_label, LV_OBJ_FLAG_HIDDEN);
            break;
            
        case UPDATE_STATE_FAILED:
            {
                char buf[64];
                snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING " %s", i18n_get(STR_FW_CHECK_FAILED));
                lv_label_set_text(state->fw_status_label, buf);
            }
            lv_obj_set_style_text_color(state->fw_status_label, COLOR_ERROR, LV_PART_MAIN);
            break;
    }
}

void firmware_check_for_updates(void)
{
    settings_state_t* state = settings_get_state();
    if (state->update_state == UPDATE_STATE_IDLE) {
        firmware_update_ui_state(UPDATE_STATE_CHECKING);
        xTaskCreate(check_for_updates_task, "fw_check", 8192, NULL, 5, NULL);
    }
}

void firmware_panel_cleanup(void)
{
    settings_state_t* state = settings_get_state();
    if (state->progress_timer) {
        lv_timer_delete(state->progress_timer);
        state->progress_timer = NULL;
    }
}

// ============================================================================
// Firmware Panel Creation
// ============================================================================

void firmware_panel_create(lv_obj_t* parent)
{
    settings_state_t* state = settings_get_state();
    
    state->fw_panel = lv_obj_create(parent);
    lv_obj_set_size(state->fw_panel, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(state->fw_panel, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state->fw_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(state->fw_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(state->fw_panel, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state->fw_panel, 25, LV_PART_MAIN);
    lv_obj_set_flex_flow(state->fw_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(state->fw_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(state->fw_panel, 20, LV_PART_MAIN);
    disable_scrolling(state->fw_panel);
    lv_obj_add_flag(state->fw_panel, LV_OBJ_FLAG_HIDDEN);
    
    ota_status_t ota_status = ota_mgr_get_status();
    strncpy(state->current_version, ota_status.current_version, sizeof(state->current_version) - 1);
    
    lv_obj_t* title = lv_label_create(state->fw_panel);
    {
        char buf[64];
        snprintf(buf, sizeof(buf), LV_SYMBOL_DOWNLOAD " %s", i18n_get(STR_FW_TITLE));
        lv_label_set_text(title, buf);
    }
    lv_obj_set_style_text_font(title, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_ACCENT, LV_PART_MAIN);
    
    state->fw_current_version_label = lv_label_create(state->fw_panel);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s: %s", i18n_get(STR_FW_CURRENT), strlen(state->current_version) > 0 ? state->current_version : "?");
    lv_label_set_text(state->fw_current_version_label, buf);
    lv_obj_set_style_text_font(state->fw_current_version_label, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(state->fw_current_version_label, COLOR_TEXT, LV_PART_MAIN);
    
    state->fw_latest_version_label = lv_label_create(state->fw_panel);
    {
        char buf2[64];
        snprintf(buf2, sizeof(buf2), "%s: --", i18n_get(STR_FW_LATEST));
        lv_label_set_text(state->fw_latest_version_label, buf2);
    }
    lv_obj_set_style_text_font(state->fw_latest_version_label, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(state->fw_latest_version_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    
    state->fw_status_label = lv_label_create(state->fw_panel);
    lv_label_set_text(state->fw_status_label, "");
    lv_obj_set_style_text_font(state->fw_status_label, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(state->fw_status_label, COLOR_TEXT, LV_PART_MAIN);
    
    state->fw_progress_bar = lv_bar_create(state->fw_panel);
    lv_obj_set_size(state->fw_progress_bar, LV_PCT(100), 25);
    lv_bar_set_range(state->fw_progress_bar, 0, 100);
    lv_bar_set_value(state->fw_progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(state->fw_progress_bar, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(state->fw_progress_bar, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(state->fw_progress_bar, 12, LV_PART_MAIN);
    lv_obj_set_style_radius(state->fw_progress_bar, 12, LV_PART_INDICATOR);
    lv_obj_add_flag(state->fw_progress_bar, LV_OBJ_FLAG_HIDDEN);
    
    state->fw_progress_label = lv_label_create(state->fw_panel);
    lv_label_set_text(state->fw_progress_label, "");
    lv_obj_set_style_text_font(state->fw_progress_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(state->fw_progress_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_add_flag(state->fw_progress_label, LV_OBJ_FLAG_HIDDEN);
    
    state->fw_update_btn = lv_btn_create(state->fw_panel);
    lv_obj_set_size(state->fw_update_btn, 200, 50);
    lv_obj_set_style_bg_color(state->fw_update_btn, COLOR_SUCCESS, LV_PART_MAIN);
    lv_obj_set_style_radius(state->fw_update_btn, 10, LV_PART_MAIN);
    lv_obj_add_event_cb(state->fw_update_btn, update_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(state->fw_update_btn, LV_OBJ_FLAG_HIDDEN);
    
    lv_obj_t* update_lbl = lv_label_create(state->fw_update_btn);
    {
        char buf3[64];
        snprintf(buf3, sizeof(buf3), LV_SYMBOL_DOWNLOAD " %s", i18n_get(STR_FW_INSTALL_UPDATE));
        lv_label_set_text(update_lbl, buf3);
    }
    lv_obj_set_style_text_font(update_lbl, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_center(update_lbl);
}
