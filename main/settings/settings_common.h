/*
 * Arctic Heat Pump Controller
 * Settings Screen - Common Definitions
 * 
 * Shared colors, state, and helpers used across all settings panels.
 */
#pragma once

#include "settings_screen.h"
#include <lvgl.h>
#include <esp_log.h>

// ============================================================================
// Colors and Styles
// ============================================================================

#define COLOR_BG            lv_color_hex(0x1a1a2e)
#define COLOR_CARD          lv_color_hex(0x16213e)
#define COLOR_SIDEBAR       lv_color_hex(0x0f1a2e)
#define COLOR_SIDEBAR_BTN   lv_color_hex(0x1a2a4e)
#define COLOR_SIDEBAR_SEL   lv_color_hex(0x00d4ff)
#define COLOR_ACCENT        lv_color_hex(0x00d4ff)
#define COLOR_TEXT          lv_color_hex(0xffffff)
#define COLOR_TEXT_DIM      lv_color_hex(0x888888)
#define COLOR_SUCCESS       lv_color_hex(0x4caf50)
#define COLOR_WARNING       lv_color_hex(0xff9800)
#define COLOR_ERROR         lv_color_hex(0xf44336)
#define COLOR_DISCONNECT    lv_color_hex(0xe74c3c)

// ============================================================================
// Layout Constants
// ============================================================================

#define SIDEBAR_WIDTH_PCT   23      // Sidebar takes ~23% of screen width
#define HEADER_HEIGHT_PCT   8       // Header takes ~8% of screen height
#define SCAN_INTERVAL_MS    10000

// ============================================================================
// Panel Types
// ============================================================================

typedef enum {
    PANEL_WIFI,
    PANEL_FIRMWARE,
    PANEL_COUNT
} panel_type_t;

// ============================================================================
// Firmware Update State
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

// ============================================================================
// Shared State Structure
// ============================================================================

typedef struct {
    bool visible;
    settings_screen_config_t config;
    panel_type_t active_panel;
    
    // Screen objects
    lv_obj_t* screen;
    lv_obj_t* header;
    lv_obj_t* sidebar;
    lv_obj_t* content_area;
    
    // Sidebar buttons
    lv_obj_t* wifi_btn;
    lv_obj_t* firmware_btn;
    
    // WiFi panel objects
    lv_obj_t* wifi_panel;
    lv_obj_t* wifi_connected_section;
    lv_obj_t* wifi_ssid_label;
    lv_obj_t* wifi_ip_label;
    lv_obj_t* wifi_signal_label;
    lv_obj_t* wifi_disconnect_btn;
    lv_obj_t* wifi_networks_section;
    lv_obj_t* wifi_networks_title;
    lv_obj_t* wifi_network_list;
    lv_obj_t* wifi_password_dialog;
    lv_obj_t* wifi_password_ssid_label;
    lv_obj_t* wifi_password_textarea;
    lv_obj_t* wifi_show_password_btn;
    lv_obj_t* wifi_show_password_icon;
    lv_obj_t* wifi_keyboard;
    lv_obj_t* wifi_connect_btn;
    lv_obj_t* wifi_cancel_btn;
    bool wifi_password_visible;
    bool wifi_is_connected;
    char wifi_connected_ssid[33];
    char wifi_connected_ip[16];
    char wifi_selected_ssid[33];
    bool wifi_selected_is_open;
    settings_wifi_network_t wifi_networks[20];
    uint8_t wifi_network_count;
    lv_timer_t* wifi_scan_timer;
    
    // Firmware panel objects
    lv_obj_t* fw_panel;
    lv_obj_t* fw_current_version_label;
    lv_obj_t* fw_latest_version_label;
    lv_obj_t* fw_status_label;
    lv_obj_t* fw_progress_bar;
    lv_obj_t* fw_progress_label;
    lv_obj_t* fw_update_btn;
    update_ui_state_t update_state;
    volatile update_ui_state_t pending_state;
    char current_version[32];
    char latest_version[32];
    char download_url[256];
    lv_timer_t* progress_timer;
    
} settings_state_t;

// ============================================================================
// Screen Dimensions (set at runtime)
// ============================================================================

extern int32_t screen_width;
extern int32_t screen_height;
extern int32_t sidebar_width;
extern int32_t header_height;
extern int32_t content_width;
extern int32_t content_height;

// ============================================================================
// Shared State Access
// ============================================================================

settings_state_t* settings_get_state(void);

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Fully disable scrolling and hide scrollbars on an object
 */
static inline void disable_scrolling(lv_obj_t* obj) {
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

/**
 * @brief Sanitize SSID for display (handle special characters)
 */
void sanitize_ssid_for_display(char* dest, const char* src, size_t dest_size);

/**
 * @brief Get WiFi signal icon based on RSSI
 */
const char* get_signal_icon(int8_t rssi);
