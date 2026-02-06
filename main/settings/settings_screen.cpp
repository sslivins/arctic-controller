/*
 * Arctic Heat Pump Controller
 * Settings Screen Implementation - Main Module
 * 
 * This is the main entry point for the settings screen.
 * Panel-specific functionality is delegated to:
 * - settings_wifi_panel.cpp
 * - settings_firmware_panel.cpp
 */
#include "settings_common.h"
#include "settings_wifi_panel.h"
#include "settings_firmware_panel.h"
#include "ui_common.h"
#include "wifi_manager.h"
#include <string.h>
#include <stdio.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "settings_screen";

// GitHub release API URL for background check
#define GITHUB_API_URL "https://api.github.com/repos/sslivins/arctic-controller/releases/latest"
#define GITHUB_API_TIMEOUT_MS 10000
#define HTTP_RESPONSE_BUFFER_SIZE 16384

// ============================================================================
// Screen Dimensions (defined here, declared extern in settings_common.h)
// ============================================================================

int32_t screen_width = 0;
int32_t screen_height = 0;
int32_t sidebar_width = 0;
int32_t header_height = 0;
int32_t content_width = 0;
int32_t content_height = 0;

// ============================================================================
// Shared State
// ============================================================================

static settings_state_t state = {};

settings_state_t* settings_get_state(void)
{
    return &state;
}

// ============================================================================
// Helper Functions (defined here, declared in settings_common.h)
// ============================================================================

void sanitize_ssid_for_display(char* dest, const char* src, size_t dest_size)
{
    size_t di = 0;
    size_t si = 0;
    
    while (src[si] && di < dest_size - 1) {
        if ((unsigned char)src[si] == 0xE2 && 
            (unsigned char)src[si + 1] == 0x80) {
            unsigned char third = (unsigned char)src[si + 2];
            if (third == 0x99 || third == 0x98) {
                dest[di++] = '\'';
                si += 3;
                continue;
            } else if (third == 0x9C || third == 0x9D) {
                dest[di++] = '"';
                si += 3;
                continue;
            }
        }
        dest[di++] = src[si++];
    }
    dest[di] = '\0';
}

const char* get_signal_icon(int8_t rssi)
{
    (void)rssi;
    return LV_SYMBOL_WIFI;
}

// ============================================================================
// Internal Functions
// ============================================================================

static void init_screen_dimensions(void)
{
    lv_display_t* disp = lv_display_get_default();
    screen_width = lv_display_get_horizontal_resolution(disp);
    screen_height = lv_display_get_vertical_resolution(disp);
    sidebar_width = (screen_width * SIDEBAR_WIDTH_PCT) / 100;
    header_height = (screen_height * HEADER_HEIGHT_PCT) / 100;
    content_width = screen_width - sidebar_width;
    content_height = screen_height - header_height;
    ESP_LOGI(TAG, "Screen: %ldx%ld, Sidebar: %ld, Header: %ld, Content: %ldx%ld",
             screen_width, screen_height, sidebar_width, header_height, content_width, content_height);
}

// ============================================================================
// Event Handlers
// ============================================================================

static void close_btn_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (state.config.on_close) {
            state.config.on_close();
        }
        settings_screen_close();
    }
}

static void wifi_btn_event_cb(lv_event_t* e);
static void firmware_btn_event_cb(lv_event_t* e);
static void switch_panel(panel_type_t panel);
static void update_sidebar_selection(void);

static void wifi_btn_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        switch_panel(PANEL_WIFI);
    }
}

static void firmware_btn_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        switch_panel(PANEL_FIRMWARE);
    }
}

// ============================================================================
// Panel Switching
// ============================================================================

static void switch_panel(panel_type_t panel)
{
    if (state.active_panel == panel) {
        return;
    }
    
    state.active_panel = panel;
    update_sidebar_selection();
    
    // Hide all panels
    lv_obj_add_flag(state.wifi_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(state.fw_panel, LV_OBJ_FLAG_HIDDEN);
    
    // Show active panel
    switch (panel) {
        case PANEL_WIFI:
            lv_obj_remove_flag(state.wifi_panel, LV_OBJ_FLAG_HIDDEN);
            wifi_start_scan_timer();
            wifi_trigger_scan();
            break;
            
        case PANEL_FIRMWARE:
            lv_obj_remove_flag(state.fw_panel, LV_OBJ_FLAG_HIDDEN);
            wifi_stop_scan_timer();
            firmware_check_for_updates();
            break;
            
        default:
            break;
    }
}

static void update_sidebar_selection(void)
{
    // Reset all buttons
    lv_obj_set_style_bg_color(state.wifi_btn, COLOR_SIDEBAR_BTN, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.wifi_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(state.firmware_btn, COLOR_SIDEBAR_BTN, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.firmware_btn, 0, LV_PART_MAIN);
    
    // Highlight selected
    lv_obj_t* selected = NULL;
    switch (state.active_panel) {
        case PANEL_WIFI: selected = state.wifi_btn; break;
        case PANEL_FIRMWARE: selected = state.firmware_btn; break;
        default: break;
    }
    
    if (selected) {
        lv_obj_set_style_border_width(selected, 3, LV_PART_MAIN);
        lv_obj_set_style_border_color(selected, COLOR_SIDEBAR_SEL, LV_PART_MAIN);
        lv_obj_set_style_border_side(selected, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
    }
}

// ============================================================================
// UI Creation
// ============================================================================

static void create_header(void)
{
    state.header = lv_obj_create(state.screen);
    lv_obj_set_size(state.header, LV_PCT(100), header_height);
    lv_obj_align(state.header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(state.header, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state.header, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.header, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(state.header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.header, 10, LV_PART_MAIN);
    disable_scrolling(state.header);
    
    lv_obj_t* title = lv_label_create(state.header);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 10, 0);
    
    ui_create_close_button(state.header, close_btn_event_cb);
}

static void create_sidebar(void)
{
    state.sidebar = lv_obj_create(state.screen);
    lv_obj_set_size(state.sidebar, sidebar_width, content_height);
    lv_obj_align(state.sidebar, LV_ALIGN_TOP_LEFT, 0, header_height);
    lv_obj_set_style_bg_color(state.sidebar, COLOR_SIDEBAR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state.sidebar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.sidebar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(state.sidebar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.sidebar, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(state.sidebar, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(state.sidebar, LV_FLEX_FLOW_COLUMN);
    disable_scrolling(state.sidebar);
    
    // WiFi button
    state.wifi_btn = lv_btn_create(state.sidebar);
    lv_obj_set_size(state.wifi_btn, LV_PCT(100), 60);
    lv_obj_set_style_bg_color(state.wifi_btn, COLOR_SIDEBAR_BTN, LV_PART_MAIN);
    lv_obj_set_style_radius(state.wifi_btn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(state.wifi_btn, wifi_btn_event_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t* wifi_icon = lv_label_create(state.wifi_btn);
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(wifi_icon, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_align(wifi_icon, LV_ALIGN_LEFT_MID, 5, 0);
    
    lv_obj_t* wifi_lbl = lv_label_create(state.wifi_btn);
    lv_label_set_text(wifi_lbl, "WiFi");
    lv_obj_set_style_text_font(wifi_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(wifi_lbl, LV_ALIGN_LEFT_MID, 40, 0);
    
    // Firmware button
    state.firmware_btn = lv_btn_create(state.sidebar);
    lv_obj_set_size(state.firmware_btn, LV_PCT(100), 60);
    lv_obj_set_style_bg_color(state.firmware_btn, COLOR_SIDEBAR_BTN, LV_PART_MAIN);
    lv_obj_set_style_radius(state.firmware_btn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(state.firmware_btn, firmware_btn_event_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t* fw_icon = lv_label_create(state.firmware_btn);
    lv_label_set_text(fw_icon, LV_SYMBOL_DOWNLOAD);
    lv_obj_set_style_text_font(fw_icon, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_align(fw_icon, LV_ALIGN_LEFT_MID, 5, 0);
    
    lv_obj_t* fw_lbl = lv_label_create(state.firmware_btn);
    lv_label_set_text(fw_lbl, "Update");
    lv_obj_set_style_text_font(fw_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(fw_lbl, LV_ALIGN_LEFT_MID, 40, 0);
}

static void create_content_area(void)
{
    state.content_area = lv_obj_create(state.screen);
    lv_obj_set_size(state.content_area, content_width, content_height);
    lv_obj_set_pos(state.content_area, sidebar_width, header_height);
    lv_obj_set_style_bg_opa(state.content_area, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.content_area, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.content_area, 15, LV_PART_MAIN);
    disable_scrolling(state.content_area);
    
    // Create panels - delegated to panel modules
    wifi_panel_create(state.content_area);
    firmware_panel_create(state.content_area);
}

// ============================================================================
// Public API
// ============================================================================

void settings_screen_create(const settings_screen_config_t* config)
{
    if (state.visible) {
        return;
    }
    
    ESP_LOGI(TAG, "Creating settings screen");
    
    // Initialize screen dimensions for responsive layout
    init_screen_dimensions();
    
    if (config) {
        state.config = *config;
    } else {
        memset(&state.config, 0, sizeof(state.config));
    }
    
    state.visible = true;
    state.active_panel = PANEL_COUNT;  // Invalid - so first switch_panel works
    state.update_state = UPDATE_STATE_IDLE;
    state.wifi_is_connected = false;
    state.wifi_network_count = 0;
    memset(state.latest_version, 0, sizeof(state.latest_version));
    memset(state.download_url, 0, sizeof(state.download_url));
    
    state.screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(state.screen, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state.screen, LV_OPA_COVER, LV_PART_MAIN);
    
    create_header();
    create_sidebar();
    create_content_area();
    
    update_sidebar_selection();
    switch_panel(PANEL_WIFI);
    
    // Update WiFi status if already connected
    if (wifi_mgr_get_state() == WIFI_MGR_STATE_CONNECTED) {
        char ip[16] = {};
        wifi_mgr_get_ip_addr(ip, sizeof(ip));
        settings_screen_set_wifi_status(true, wifi_mgr_get_connected_ssid(), ip);
    }
    
    lv_screen_load_anim(state.screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}

void settings_screen_close(void)
{
    if (!state.visible) {
        return;
    }
    
    ESP_LOGI(TAG, "Closing settings screen");
    
    wifi_panel_cleanup();
    firmware_panel_cleanup();
    
    state.visible = false;
    
    if (state.screen) {
        lv_obj_delete(state.screen);
        state.screen = NULL;
    }
}

bool settings_screen_is_visible(void)
{
    return state.visible;
}

void settings_screen_update_networks(const settings_wifi_network_t* networks, uint8_t count)
{
    if (!state.visible || !state.wifi_network_list) {
        return;
    }
    
    // Deduplicate networks
    settings_wifi_network_t deduped[20];
    uint8_t deduped_count = 0;
    
    for (uint8_t i = 0; i < count && i < 20; i++) {
        const settings_wifi_network_t* net = &networks[i];
        
        if (net->ssid[0] == '\0') continue;
        if (state.wifi_is_connected && strcmp(net->ssid, state.wifi_connected_ssid) == 0) continue;
        
        bool found = false;
        for (uint8_t j = 0; j < deduped_count; j++) {
            if (strcmp(deduped[j].ssid, net->ssid) == 0) {
                if (net->rssi > deduped[j].rssi) {
                    deduped[j].rssi = net->rssi;
                    deduped[j].authmode = net->authmode;
                }
                found = true;
                break;
            }
        }
        
        if (!found && deduped_count < 20) {
            memcpy(&deduped[deduped_count], net, sizeof(settings_wifi_network_t));
            deduped_count++;
        }
    }
    
    state.wifi_network_count = deduped_count;
    if (deduped_count > 0) {
        memcpy(state.wifi_networks, deduped, deduped_count * sizeof(settings_wifi_network_t));
    }
    
    lv_obj_clean(state.wifi_network_list);
    
    if (state.wifi_network_count == 0) {
        lv_obj_t* empty = lv_label_create(state.wifi_network_list);
        lv_label_set_text(empty, "Scanning for networks...");
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(empty, COLOR_TEXT_DIM, LV_PART_MAIN);
        return;
    }
    
    for (uint8_t i = 0; i < state.wifi_network_count; i++) {
        const settings_wifi_network_t* net = &state.wifi_networks[i];
        
        lv_obj_t* row = lv_obj_create(state.wifi_network_list);
        lv_obj_set_size(row, LV_PCT(100), 50);
        lv_obj_set_style_bg_color(row, COLOR_CARD, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(row, 12, LV_PART_MAIN);
        disable_scrolling(row);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            uintptr_t index = (uintptr_t)lv_event_get_user_data(e);
            settings_state_t* st = settings_get_state();
            if (index < st->wifi_network_count) {
                const settings_wifi_network_t* n = &st->wifi_networks[index];
                bool is_open = (n->authmode == 0);
                wifi_show_password_dialog(n->ssid, is_open);
            }
        }, LV_EVENT_CLICKED, (void*)(uintptr_t)i);
        
        lv_obj_t* signal = lv_label_create(row);
        lv_label_set_text(signal, get_signal_icon(net->rssi));
        lv_obj_set_style_text_font(signal, &lv_font_montserrat_24, LV_PART_MAIN);
        lv_obj_set_style_text_color(signal, COLOR_ACCENT, LV_PART_MAIN);
        lv_obj_align(signal, LV_ALIGN_LEFT_MID, 0, 0);
        
        char display_ssid[64];
        sanitize_ssid_for_display(display_ssid, net->ssid, sizeof(display_ssid));
        lv_obj_t* name = lv_label_create(row);
        lv_label_set_text(name, display_ssid);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(name, COLOR_TEXT, LV_PART_MAIN);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 35, 0);
        
        if (net->authmode > 0) {
            lv_obj_t* lock = lv_label_create(row);
            lv_label_set_text(lock, LV_SYMBOL_EYE_CLOSE);
            lv_obj_set_style_text_font(lock, &lv_font_montserrat_14, LV_PART_MAIN);
            lv_obj_set_style_text_color(lock, COLOR_TEXT_DIM, LV_PART_MAIN);
            lv_obj_align(lock, LV_ALIGN_RIGHT_MID, -5, 0);
        }
    }
}

void settings_screen_set_scanning(bool scanning)
{
    if (!state.visible || !state.wifi_networks_title) {
        return;
    }
    
    if (scanning) {
        lv_label_set_text(state.wifi_networks_title, "Available Networks  " LV_SYMBOL_REFRESH);
    } else {
        lv_label_set_text(state.wifi_networks_title, "Available Networks");
    }
}

void settings_screen_set_wifi_status(bool connected, const char* ssid, const char* ip_addr)
{
    state.wifi_is_connected = connected;
    
    if (connected && ssid) {
        strncpy(state.wifi_connected_ssid, ssid, sizeof(state.wifi_connected_ssid) - 1);
        if (ip_addr) {
            strncpy(state.wifi_connected_ip, ip_addr, sizeof(state.wifi_connected_ip) - 1);
        } else {
            state.wifi_connected_ip[0] = '\0';
        }
    } else {
        state.wifi_connected_ssid[0] = '\0';
        state.wifi_connected_ip[0] = '\0';
    }
    
    if (state.visible) {
        wifi_update_connected_section();
    }
}

void settings_screen_show_error(const char* message)
{
    if (!state.visible) {
        return;
    }
    
    lv_obj_t* msgbox = lv_msgbox_create(state.screen);
    lv_msgbox_add_title(msgbox, "Error");
    lv_msgbox_add_text(msgbox, message);
    lv_msgbox_add_close_button(msgbox);
    lv_obj_center(msgbox);
}

void settings_screen_show_firmware_panel(void)
{
    if (!state.visible) {
        return;
    }
    
    switch_panel(PANEL_FIRMWARE);
}

// ============================================================================
// Background Update Check
// ============================================================================

static update_check_cb_t s_update_check_callback = NULL;

static void background_update_check_task(void* arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Background check for updates...");
    
    bool update_available = false;
    char latest_ver[32] = {0};
    char* response_buffer = (char*)heap_caps_calloc(1, HTTP_RESPONSE_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    
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
    
    if (s_update_check_callback) {
        s_update_check_callback(update_available, latest_ver);
        s_update_check_callback = NULL;
    }
    
    vTaskDelete(NULL);
}

void settings_screen_check_for_updates_async(update_check_cb_t callback)
{
    s_update_check_callback = callback;
    xTaskCreate(background_update_check_task, "bg_update_chk", 8192, NULL, 5, NULL);
}
