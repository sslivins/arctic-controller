/*
 * Arctic Heat Pump Controller
 * Settings Menu - iOS-style settings list
 * 
 * Main settings screen with a list of setting categories.
 * Each row opens a full-screen sub-setting when tapped.
 */
#include "settings_menu.h"
#include "settings_wifi_screen.h"
#include "settings_firmware_screen.h"
#include "settings_time_screen.h"
#include "settings_language_screen.h"
#include "settings_display_screen.h"
#include "../settings_screen.h"  // For settings_wifi_network_t
#include "../ui_common.h"  // For ui_create_close_button
#include "i18n/i18n.h"
#include "fonts/fonts.h"
#include "wifi_manager.h"
#include <esp_log.h>
#include <string.h>

static const char* TAG = "settings_menu";

// ============================================================================
// Colors and Styles
// ============================================================================

#define COLOR_BG            lv_color_hex(0x1a1a2e)
#define COLOR_CARD          lv_color_hex(0x16213e)
#define COLOR_ROW           lv_color_hex(0x1e2a4a)
#define COLOR_ROW_PRESSED   lv_color_hex(0x2a3a5a)
#define COLOR_ACCENT        lv_color_hex(0x00d4ff)
#define COLOR_TEXT          lv_color_hex(0xffffff)
#define COLOR_TEXT_DIM      lv_color_hex(0x888888)
#define COLOR_DIVIDER       lv_color_hex(0x2a3a5a)

#define FONT_NORMAL   &montserrat_24_latin
#define FONT_LARGE    &montserrat_24_latin

// ============================================================================
// Menu State
// ============================================================================

typedef enum {
    SETTINGS_WIFI,
    SETTINGS_FIRMWARE,
    SETTINGS_TIME,
    SETTINGS_LANGUAGE,
    SETTINGS_DISPLAY,
    SETTINGS_COUNT
} settings_item_t;

typedef struct {
    bool visible;
    settings_menu_config_t config;
    lv_obj_t* screen;
    lv_obj_t* header;
    lv_obj_t* list_container;
    lv_obj_t* rows[SETTINGS_COUNT];
    lv_obj_t* wifi_status_label;  // Shows connected SSID or "Not connected"
    
    // Track which sub-screen is active
    settings_item_t active_sub_screen;
    bool sub_screen_active;
} menu_state_t;

static menu_state_t state = {};

// ============================================================================
// Forward Declarations
// ============================================================================

static void create_header(void);
static void create_menu_list(void);
static void close_btn_event_cb(lv_event_t* e);
static void row_click_cb(lv_event_t* e);

// ============================================================================
// Helper Functions
// ============================================================================

static void disable_scrolling(lv_obj_t* obj)
{
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

static lv_obj_t* create_settings_row(lv_obj_t* parent, const char* icon, 
                                       const char* label, settings_item_t item)
{
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 88);
    lv_obj_set_style_bg_color(row, COLOR_ROW, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 20, LV_PART_MAIN);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    disable_scrolling(row);
    
    // Pressed state
    lv_obj_set_style_bg_color(row, COLOR_ROW_PRESSED, LV_PART_MAIN | (lv_style_selector_t)LV_STATE_PRESSED);
    
    // Store item type in user data
    lv_obj_set_user_data(row, (void*)(intptr_t)item);
    lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, NULL);
    
    // Icon on left
    lv_obj_t* icon_label = lv_label_create(row);
    lv_label_set_text(icon_label, icon);
    lv_obj_set_style_text_font(icon_label, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon_label, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(icon_label, LV_ALIGN_LEFT_MID, 0, 0);
    
    // Label
    lv_obj_t* text_label = lv_label_create(row);
    lv_label_set_text(text_label, label);
    lv_obj_set_style_text_font(text_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(text_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(text_label, LV_ALIGN_LEFT_MID, 45, 0);
    
    // Chevron on right (>)
    lv_obj_t* chevron = lv_label_create(row);
    lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(chevron, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(chevron, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, 0, 0);
    
    return row;
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
        settings_menu_close();
    }
}

static void row_click_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    
    lv_obj_t* row = (lv_obj_t*)lv_event_get_target(e);
    settings_item_t item = (settings_item_t)(intptr_t)lv_obj_get_user_data(row);
    
    ESP_LOGI(TAG, "Settings row clicked: %d", item);
    
    switch (item) {
        case SETTINGS_WIFI: {
            ESP_LOGI(TAG, "Opening WiFi settings...");
            wifi_screen_config_t wifi_cfg = {
                .on_wifi_scan = state.config.on_wifi_scan,
                .on_wifi_connect = state.config.on_wifi_connect,
                .on_wifi_disconnect = state.config.on_wifi_disconnect,
                .on_back = settings_menu_show,
            };
            wifi_screen_create(&wifi_cfg);
            state.sub_screen_active = true;
            state.active_sub_screen = SETTINGS_WIFI;
            break;
        }
        case SETTINGS_FIRMWARE: {
            ESP_LOGI(TAG, "Opening Firmware settings...");
            firmware_screen_config_t fw_cfg = {
                .on_back = settings_menu_show,
            };
            firmware_screen_create(&fw_cfg);
            state.sub_screen_active = true;
            state.active_sub_screen = SETTINGS_FIRMWARE;
            break;
        }
        case SETTINGS_TIME: {
            ESP_LOGI(TAG, "Opening Time settings...");
            time_screen_config_t time_cfg = {
                .on_back = settings_menu_show,
            };
            time_screen_create(&time_cfg);
            state.sub_screen_active = true;
            state.active_sub_screen = SETTINGS_TIME;
            break;
        }
        case SETTINGS_LANGUAGE: {
            ESP_LOGI(TAG, "Opening Language settings...");
            language_screen_config_t lang_cfg = {
                .on_back = settings_menu_show,
            };
            language_screen_create(&lang_cfg);
            state.sub_screen_active = true;
            state.active_sub_screen = SETTINGS_LANGUAGE;
            break;
        }
        case SETTINGS_DISPLAY: {
            ESP_LOGI(TAG, "Opening Display settings...");
            display_screen_config_t disp_cfg = {
                .on_back = settings_menu_show,
            };
            display_screen_create(&disp_cfg);
            state.sub_screen_active = true;
            state.active_sub_screen = SETTINGS_DISPLAY;
            break;
        }
        default:
            break;
    }
}

// ============================================================================
// UI Creation
// ============================================================================

static void create_header(void)
{
    lv_display_t* disp = lv_display_get_default();
    int32_t header_height = lv_display_get_vertical_resolution(disp) * 8 / 100;
    
    state.header = lv_obj_create(state.screen);
    lv_obj_set_size(state.header, LV_PCT(100), header_height);
    lv_obj_align(state.header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(state.header, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state.header, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.header, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(state.header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.header, 10, LV_PART_MAIN);
    disable_scrolling(state.header);
    
    // Title
    lv_obj_t* title = lv_label_create(state.header);
    lv_label_set_text(title, i18n_get(STR_SETTINGS));
    lv_obj_set_style_text_font(title, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 10, 0);
    
    // Close button
    ui_create_close_button(state.header, close_btn_event_cb);
}

static void create_menu_list(void)
{
    lv_display_t* disp = lv_display_get_default();
    int32_t screen_height = lv_display_get_vertical_resolution(disp);
    int32_t header_height = screen_height * 8 / 100;
    
    state.list_container = lv_obj_create(state.screen);
    lv_obj_set_size(state.list_container, LV_PCT(100), screen_height - header_height);
    lv_obj_set_pos(state.list_container, 0, header_height);
    lv_obj_set_style_bg_opa(state.list_container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.list_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.list_container, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_row(state.list_container, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(state.list_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(state.list_container, LV_SCROLLBAR_MODE_AUTO);
    
    // Create setting rows
    state.rows[SETTINGS_WIFI] = create_settings_row(
        state.list_container, LV_SYMBOL_WIFI, 
        i18n_get(STR_SETTINGS_WIFI), SETTINGS_WIFI);
    
    // Add WiFi status subtitle
    lv_obj_t* wifi_row = state.rows[SETTINGS_WIFI];
    state.wifi_status_label = lv_label_create(wifi_row);
    lv_label_set_text(state.wifi_status_label, i18n_get(STR_WIFI_DISCONNECTED));
    lv_obj_set_style_text_font(state.wifi_status_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.wifi_status_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(state.wifi_status_label, LV_ALIGN_RIGHT_MID, -30, 0);
    
    state.rows[SETTINGS_FIRMWARE] = create_settings_row(
        state.list_container, LV_SYMBOL_DOWNLOAD, 
        i18n_get(STR_SETTINGS_UPDATE), SETTINGS_FIRMWARE);
    
    state.rows[SETTINGS_TIME] = create_settings_row(
        state.list_container, LV_SYMBOL_BELL, 
        i18n_get(STR_SETTINGS_TIME), SETTINGS_TIME);
    
    state.rows[SETTINGS_LANGUAGE] = create_settings_row(
        state.list_container, LV_SYMBOL_SETTINGS, 
        i18n_get(STR_SETTINGS_LANGUAGE), SETTINGS_LANGUAGE);
    
    state.rows[SETTINGS_DISPLAY] = create_settings_row(
        state.list_container, LV_SYMBOL_IMAGE, 
        i18n_get(STR_SETTINGS_DISPLAY), SETTINGS_DISPLAY);
}

// ============================================================================
// Public API
// ============================================================================

void settings_menu_create(const settings_menu_config_t* config)
{
    if (state.visible) {
        return;
    }
    
    ESP_LOGI(TAG, "Creating settings menu");
    
    if (config) {
        state.config = *config;
    } else {
        memset(&state.config, 0, sizeof(state.config));
    }
    
    state.visible = true;
    state.sub_screen_active = false;
    
    state.screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(state.screen, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state.screen, LV_OPA_COVER, LV_PART_MAIN);
    
    create_header();
    create_menu_list();
    
    // Update WiFi status if connected
    if (wifi_mgr_get_state() == WIFI_MGR_STATE_CONNECTED) {
        settings_menu_update_wifi_status(true, wifi_mgr_get_connected_ssid());
    }
    
    // Slide down from top when opening settings
    lv_screen_load_anim(state.screen, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, false);
}

void settings_menu_close(void)
{
    if (!state.visible) {
        return;
    }
    
    ESP_LOGI(TAG, "Closing settings menu");
    
    state.visible = false;
    // Screen deletion is handled by LVGL auto_del when returning to main screen
    state.screen = NULL;
}

bool settings_menu_is_visible(void)
{
    return state.visible;
}

void settings_menu_show(void)
{
    if (!state.visible || !state.screen) {
        return;
    }
    
    state.sub_screen_active = false;
    // Slide right animation with auto_del=true - LVGL will delete the old screen after animation
    lv_screen_load_anim(state.screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, true);
}

void settings_menu_refresh(void)
{
    if (!state.visible) {
        return;
    }
    
    // Store config and recreate
    settings_menu_config_t saved_config = state.config;
    settings_menu_close();
    settings_menu_create(&saved_config);
}

void settings_menu_update_wifi_status(bool connected, const char* ssid)
{
    // Forward to WiFi screen if it's active
    if (state.sub_screen_active && state.active_sub_screen == SETTINGS_WIFI) {
        char ip_buf[16] = {0};
        wifi_mgr_get_ip_addr(ip_buf, sizeof(ip_buf));
        wifi_screen_update_connection(connected, ssid, ip_buf);
        return;
    }
    
    if (!state.visible || !state.wifi_status_label) {
        return;
    }
    
    if (connected && ssid && ssid[0]) {
        lv_label_set_text(state.wifi_status_label, ssid);
        lv_obj_set_style_text_color(state.wifi_status_label, COLOR_ACCENT, LV_PART_MAIN);
    } else {
        lv_label_set_text(state.wifi_status_label, i18n_get(STR_WIFI_DISCONNECTED));
        lv_obj_set_style_text_color(state.wifi_status_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    }
}
