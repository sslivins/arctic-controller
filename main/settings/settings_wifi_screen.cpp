/*
 * Arctic Heat Pump Controller
 * Settings - WiFi Screen Implementation (iOS-style full screen)
 * 
 * Full-screen WiFi configuration with back navigation.
 * Portrait mode: 720x1280
 */
#include "settings_wifi_screen.h"
#include "settings_menu.h"
#include "settings_common.h"  // For shared layout constants
#include "keyboard_maps.h"    // Custom keyboard maps (C file for LVGL compatibility)
#include "wifi_manager.h"
#include "i18n/i18n.h"
#include "fonts/fonts.h"
#include <esp_log.h>
#include <string.h>
#include <stdio.h>

static const char* TAG = "wifi_screen";

// ============================================================================
// Helper Function Implementations
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

// Colors, fonts, and helpers come from settings_common.h

// ============================================================================
// Layout Constants
// ============================================================================

#define ROW_HEIGHT          88
#define MAX_NETWORKS        20

// ============================================================================
// WiFi Screen State
// ============================================================================

typedef struct {
    bool visible;
    wifi_screen_config_t config;
    
    // Screen objects
    lv_obj_t* screen;
    lv_obj_t* header;
    lv_obj_t* back_btn;
    lv_obj_t* title_label;
    lv_obj_t* content;
    
    // Connected section
    lv_obj_t* connected_section;
    lv_obj_t* connected_ssid_label;
    lv_obj_t* connected_ip_label;
    lv_obj_t* signal_label;
    lv_obj_t* disconnect_btn;

    // Connecting section (shown briefly while a connection is in progress)
    lv_obj_t* connecting_section;
    lv_obj_t* connecting_label;
    
    // Networks section
    lv_obj_t* networks_section;
    lv_obj_t* networks_title;
    lv_obj_t* network_list;
    lv_obj_t* scanning_label;
    
    // Password dialog
    lv_obj_t* password_dialog;
    lv_obj_t* password_ssid_label;
    lv_obj_t* password_textarea;
    lv_obj_t* show_password_btn;
    lv_obj_t* show_password_icon;
    lv_obj_t* keyboard;
    lv_obj_t* connect_btn;
    lv_obj_t* cancel_btn;
    
    // State
    bool is_connected;
    bool connecting;
    char connected_ssid[33];
    char connected_ip[16];
    char connecting_ssid[33];
    char selected_ssid[33];
    bool selected_is_open;
    bool password_visible;
    bool is_scanning;
    bool mock_mode;
    lv_timer_t* scan_timer;
    
} wifi_screen_state_t;

static wifi_screen_state_t s_state = {};

// ============================================================================
// Forward Declarations
// ============================================================================

static void create_header(void);
static void create_connected_section(void);
static void create_networks_section(void);
static void create_password_dialog(void);
static void update_connected_display(void);
static void show_password_dialog(const char* ssid, bool is_open);
static void hide_password_dialog(void);

// Event handlers
static void back_btn_cb(lv_event_t* e);
static void disconnect_btn_cb(lv_event_t* e);
static void network_item_cb(lv_event_t* e);
static void connect_btn_cb(lv_event_t* e);
static void cancel_btn_cb(lv_event_t* e);
static void show_password_cb(lv_event_t* e);
static void keyboard_ready_cb(lv_event_t* e);
static void scan_timer_cb(lv_timer_t* timer);

// ============================================================================
// Public API
// ============================================================================

void wifi_screen_create(const wifi_screen_config_t* config)
{
    if (s_state.visible) {
        ESP_LOGW(TAG, "WiFi screen already visible");
        return;
    }
    
    ESP_LOGI(TAG, "Creating WiFi screen");
    
    memset(&s_state, 0, sizeof(s_state));
    if (config) {
        s_state.config = *config;
    }
    
    // Create screen
    s_state.screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_state.screen, COLOR_BG, LV_PART_MAIN);
    disable_scrolling(s_state.screen);
    
    // Create UI components
    create_header();
    create_connected_section();
    create_networks_section();
    create_password_dialog();
    
    // Status-bar entry is instant; settings-menu navigation retains its slide.
    lv_scr_load_anim_t anim = s_state.config.use_instant_transition
        ? LV_SCR_LOAD_ANIM_NONE
        : LV_SCR_LOAD_ANIM_MOVE_LEFT;
    lv_screen_load_anim(s_state.screen, anim, s_state.config.use_instant_transition ? 0 : 300, 0, false);
    s_state.visible = true;
    
    // Check current connection status
    if (wifi_mgr_get_state() == WIFI_MGR_STATE_CONNECTED) {
        const char* ssid = wifi_mgr_get_connected_ssid();
        char ip_buf[16] = {0};
        wifi_mgr_get_ip_addr(ip_buf, sizeof(ip_buf));
        wifi_screen_update_connection(true, ssid, ip_buf);
    }
    
    // Start scan timer and trigger initial scan
    s_state.scan_timer = lv_timer_create(scan_timer_cb, SCAN_INTERVAL_MS, NULL);
    wifi_screen_trigger_scan();
}

void wifi_screen_close(void)
{
    if (!s_state.visible) return;
    
    ESP_LOGI(TAG, "Closing WiFi screen");
    
    // Stop timer
    if (s_state.scan_timer) {
        lv_timer_delete(s_state.scan_timer);
        s_state.scan_timer = NULL;
    }
    
    // Mark as not visible - screen deletion is handled by LVGL auto_del
    // when settings_menu_show() is called with auto_del=true
    s_state.visible = false;
    s_state.screen = NULL;  // Will be auto-deleted by LVGL
}

bool wifi_screen_is_visible(void)
{
    return s_state.visible;
}

void wifi_screen_update_connection(bool is_connected, const char* ssid, const char* ip)
{
    s_state.is_connected = is_connected;
    s_state.connecting = false;  // A definitive result ends any "connecting" state
    
    if (ssid) {
        strncpy(s_state.connected_ssid, ssid, sizeof(s_state.connected_ssid) - 1);
    } else {
        s_state.connected_ssid[0] = '\0';
    }
    
    if (ip) {
        strncpy(s_state.connected_ip, ip, sizeof(s_state.connected_ip) - 1);
    } else {
        s_state.connected_ip[0] = '\0';
    }
    
    if (s_state.visible) {
        update_connected_display();
    }
}

void wifi_screen_show_connecting(const char* ssid)
{
    s_state.connecting = true;
    if (ssid && ssid[0]) {
        strncpy(s_state.connecting_ssid, ssid, sizeof(s_state.connecting_ssid) - 1);
        s_state.connecting_ssid[sizeof(s_state.connecting_ssid) - 1] = '\0';
    } else {
        s_state.connecting_ssid[0] = '\0';
    }

    if (!s_state.visible || !s_state.connecting_section) return;

    // Hide the connected card while a new attempt is in progress; the flex
    // layout then floats the connecting card to the top of the content.
    if (s_state.connected_section) {
        lv_obj_add_flag(s_state.connected_section, LV_OBJ_FLAG_HIDDEN);
    }

    char buf[80];
    if (s_state.connecting_ssid[0]) {
        char disp[33];
        sanitize_ssid_for_display(disp, s_state.connecting_ssid, sizeof(disp));
        snprintf(buf, sizeof(buf), "%s %s...", i18n_get(STR_WIFI_CONNECTING), disp);
    } else {
        snprintf(buf, sizeof(buf), "%s...", i18n_get(STR_WIFI_CONNECTING));
    }
    lv_label_set_text(s_state.connecting_label, buf);
    lv_obj_remove_flag(s_state.connecting_section, LV_OBJ_FLAG_HIDDEN);
}

void wifi_screen_update_networks(const settings_wifi_network_t* networks, uint8_t count)
{
    if (!s_state.visible || !s_state.network_list) return;
    
    ESP_LOGI(TAG, "Updating networks list: %d networks", count);
    
    // Deduplicate networks - keep strongest signal for each SSID
    settings_wifi_network_t deduped[MAX_NETWORKS];
    uint8_t deduped_count = 0;
    
    for (uint8_t i = 0; i < count && i < MAX_NETWORKS; i++) {
        const settings_wifi_network_t* net = &networks[i];
        
        // Skip empty SSIDs
        if (net->ssid[0] == '\0') continue;
        
        // Skip currently connected network (shown in "Connected" section)
        if (s_state.is_connected && strcmp(net->ssid, s_state.connected_ssid) == 0) continue;
        
        // Check if we already have this SSID
        bool found = false;
        for (uint8_t j = 0; j < deduped_count; j++) {
            if (strcmp(deduped[j].ssid, net->ssid) == 0) {
                // Keep the one with stronger signal
                if (net->rssi > deduped[j].rssi) {
                    deduped[j].rssi = net->rssi;
                    deduped[j].authmode = net->authmode;
                }
                found = true;
                break;
            }
        }
        
        // Add new unique SSID
        if (!found && deduped_count < MAX_NETWORKS) {
            memcpy(&deduped[deduped_count], net, sizeof(settings_wifi_network_t));
            deduped_count++;
        }
    }
    
    ESP_LOGI(TAG, "After dedup: %d unique networks", deduped_count);
    
    // Clear existing items
    lv_obj_clean(s_state.network_list);
    
    if (deduped_count == 0) {
        lv_obj_t* no_networks = lv_label_create(s_state.network_list);
        lv_label_set_text(no_networks, i18n_get(STR_WIFI_NO_NETWORKS));
        lv_obj_set_style_text_font(no_networks, FONT_NORMAL, LV_PART_MAIN);
        lv_obj_set_style_text_color(no_networks, COLOR_TEXT_DIM, LV_PART_MAIN);
        return;
    }
    
    // Add network items
    for (uint8_t i = 0; i < deduped_count; i++) {
        // Create row
        lv_obj_t* row = lv_obj_create(s_state.network_list);
        lv_obj_set_size(row, LV_PCT(100), ROW_HEIGHT);
        lv_obj_set_style_bg_color(row, COLOR_CARD, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(row, 20, LV_PART_MAIN);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        disable_scrolling(row);
        
        // Store network info in user data
        settings_wifi_network_t* net_data = (settings_wifi_network_t*)lv_malloc(sizeof(settings_wifi_network_t));
        *net_data = deduped[i];
        lv_obj_set_user_data(row, net_data);
        lv_obj_add_event_cb(row, network_item_cb, LV_EVENT_CLICKED, NULL);
        
        // Signal icon
        lv_obj_t* icon = lv_label_create(row);
        lv_label_set_text(icon, get_signal_icon(deduped[i].rssi));
        lv_obj_set_style_text_font(icon, FONT_LARGE, LV_PART_MAIN);
        lv_obj_set_style_text_color(icon, COLOR_ACCENT, LV_PART_MAIN);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);
        
        // SSID
        lv_obj_t* ssid_label = lv_label_create(row);
        lv_label_set_text(ssid_label, deduped[i].ssid);
        lv_obj_set_style_text_font(ssid_label, FONT_NORMAL, LV_PART_MAIN);
        lv_obj_set_style_text_color(ssid_label, COLOR_TEXT, LV_PART_MAIN);
        lv_obj_align(ssid_label, LV_ALIGN_LEFT_MID, 45, 0);
        
        // Lock icon for secured networks (most networks)
        if (deduped[i].authmode != 0) {
            lv_obj_t* lock = lv_label_create(row);
            lv_label_set_text(lock, FA_SYMBOL_LOCK);
            lv_obj_set_style_text_font(lock, FONT_NORMAL, LV_PART_MAIN);
            lv_obj_set_style_text_color(lock, COLOR_TEXT_DIM, LV_PART_MAIN);
            lv_obj_align(lock, LV_ALIGN_RIGHT_MID, 0, 0);
        }
    }
}

void wifi_screen_set_scanning(bool scanning)
{
    s_state.is_scanning = scanning;
    
    if (!s_state.visible) return;
    
    if (s_state.networks_title) {
        if (scanning) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%s %s", i18n_get(STR_WIFI_AVAILABLE_NETWORKS), LV_SYMBOL_REFRESH);
            lv_label_set_text(s_state.networks_title, buf);
        } else {
            lv_label_set_text(s_state.networks_title, i18n_get(STR_WIFI_AVAILABLE_NETWORKS));
        }
    }
}

void wifi_screen_trigger_scan(void)
{
    ESP_LOGI(TAG, "Triggering WiFi scan");
    if (s_state.config.on_wifi_scan) {
        wifi_screen_set_scanning(true);
        s_state.config.on_wifi_scan();
    }
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
    lv_obj_set_user_data(s_state.back_btn, (void*)"wifi_back");
    
    lv_obj_t* back_icon = lv_label_create(s_state.back_btn);
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_icon, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(back_icon, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_center(back_icon);
    
    // Title
    s_state.title_label = lv_label_create(s_state.header);
    lv_label_set_text(s_state.title_label, i18n_get(STR_SETTINGS_WIFI));
    lv_obj_set_style_text_font(s_state.title_label, UI_FONT_HEADER, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_state.title_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(s_state.title_label, LV_ALIGN_CENTER, 0, 0);
}

static void create_connected_section(void)
{
    lv_display_t* disp = lv_display_get_default();
    int32_t header_height = lv_display_get_vertical_resolution(disp) * HEADER_HEIGHT_PCT / 100;
    
    // Content container
    s_state.content = lv_obj_create(s_state.screen);
    lv_obj_set_size(s_state.content, LV_PCT(100), lv_pct(100 - HEADER_HEIGHT_PCT));
    lv_obj_align(s_state.content, LV_ALIGN_TOP_MID, 0, header_height);
    lv_obj_set_style_bg_opa(s_state.content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_state.content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_state.content, 15, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_state.content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_state.content, 20, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(s_state.content, LV_SCROLLBAR_MODE_AUTO);
    
    // Connected section (hidden by default) - taller for better spacing
    s_state.connected_section = lv_obj_create(s_state.content);
    lv_obj_set_size(s_state.connected_section, LV_PCT(100), 200);
    lv_obj_set_style_bg_color(s_state.connected_section, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_state.connected_section, COLOR_SUCCESS, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_state.connected_section, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_state.connected_section, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_state.connected_section, 20, LV_PART_MAIN);
    disable_scrolling(s_state.connected_section);
    lv_obj_add_flag(s_state.connected_section, LV_OBJ_FLAG_HIDDEN);
    
    // === TOP ROW: "Connected" status on left ===
    lv_obj_t* conn_title = lv_label_create(s_state.connected_section);
    char title_buf[48];
    snprintf(title_buf, sizeof(title_buf), LV_SYMBOL_OK " %s", i18n_get(STR_WIFI_CONNECTED));
    lv_label_set_text(conn_title, title_buf);
    lv_obj_set_style_text_font(conn_title, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(conn_title, COLOR_SUCCESS, LV_PART_MAIN);
    lv_obj_align(conn_title, LV_ALIGN_TOP_LEFT, 0, 0);
    
    // === TOP ROW: Signal strength on right ===
    s_state.signal_label = lv_label_create(s_state.connected_section);
    lv_label_set_text(s_state.signal_label, "");
    lv_obj_set_style_text_font(s_state.signal_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_align(s_state.signal_label, LV_ALIGN_TOP_RIGHT, 0, 0);
    
    // === CENTER: SSID - large and prominent ===
    s_state.connected_ssid_label = lv_label_create(s_state.connected_section);
    lv_label_set_text(s_state.connected_ssid_label, LV_SYMBOL_WIFI " NetworkName");
    lv_obj_set_style_text_font(s_state.connected_ssid_label, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_state.connected_ssid_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(s_state.connected_ssid_label, LV_ALIGN_LEFT_MID, 0, 0);
    
    // === BOTTOM LEFT: IP address ===
    s_state.connected_ip_label = lv_label_create(s_state.connected_section);
    lv_label_set_text(s_state.connected_ip_label, "IP: ...");
    lv_obj_set_style_text_font(s_state.connected_ip_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_state.connected_ip_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(s_state.connected_ip_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    
    // === BOTTOM RIGHT: Disconnect button ===
    s_state.disconnect_btn = lv_btn_create(s_state.connected_section);
    lv_obj_set_size(s_state.disconnect_btn, 160, 50);
    lv_obj_align(s_state.disconnect_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(s_state.disconnect_btn, COLOR_DISCONNECT, LV_PART_MAIN);
    lv_obj_set_style_radius(s_state.disconnect_btn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(s_state.disconnect_btn, disconnect_btn_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t* disc_label = lv_label_create(s_state.disconnect_btn);
    lv_label_set_text(disc_label, i18n_get(STR_WIFI_DISCONNECT));
    lv_obj_set_style_text_font(disc_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_center(disc_label);

    // === Connecting section (hidden by default) ===
    // Shown briefly between tapping Connect and the connected card appearing,
    // so the user gets immediate feedback that something is happening.
    s_state.connecting_section = lv_obj_create(s_state.content);
    lv_obj_set_size(s_state.connecting_section, LV_PCT(100), 100);
    lv_obj_set_style_bg_color(s_state.connecting_section, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_state.connecting_section, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_state.connecting_section, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_state.connecting_section, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_state.connecting_section, 20, LV_PART_MAIN);
    disable_scrolling(s_state.connecting_section);
    lv_obj_add_flag(s_state.connecting_section, LV_OBJ_FLAG_HIDDEN);

    // Spinner on the left
    lv_obj_t* spinner = lv_spinner_create(s_state.connecting_section);
    lv_obj_set_size(spinner, 48, 48);
    lv_obj_align(spinner, LV_ALIGN_LEFT_MID, 0, 0);
    lv_spinner_set_anim_params(spinner, 1000, 60);
    lv_obj_set_style_arc_color(spinner, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, COLOR_ACCENT, LV_PART_INDICATOR);

    // "Connecting to <ssid>…" label to the right of the spinner
    s_state.connecting_label = lv_label_create(s_state.connecting_section);
    lv_label_set_text(s_state.connecting_label, "");
    lv_obj_set_style_text_font(s_state.connecting_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_state.connecting_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(s_state.connecting_label, LV_ALIGN_LEFT_MID, 68, 0);
}

static void create_networks_section(void)
{
    // Networks section
    s_state.networks_section = lv_obj_create(s_state.content);
    lv_obj_set_size(s_state.networks_section, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(s_state.networks_section, 1);
    lv_obj_set_style_bg_opa(s_state.networks_section, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_state.networks_section, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_state.networks_section, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_state.networks_section, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_state.networks_section, 10, LV_PART_MAIN);
    disable_scrolling(s_state.networks_section);
    
    // Section title
    s_state.networks_title = lv_label_create(s_state.networks_section);
    lv_label_set_text(s_state.networks_title, i18n_get(STR_WIFI_AVAILABLE_NETWORKS));
    lv_obj_set_style_text_font(s_state.networks_title, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_state.networks_title, COLOR_TEXT_DIM, LV_PART_MAIN);
    
    // Network list container
    s_state.network_list = lv_obj_create(s_state.networks_section);
    lv_obj_set_size(s_state.network_list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(s_state.network_list, 1);
    lv_obj_set_style_bg_opa(s_state.network_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_state.network_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_state.network_list, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_state.network_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_state.network_list, 8, LV_PART_MAIN);
    disable_scrolling(s_state.network_list);
    
    // Initial scanning message
    s_state.scanning_label = lv_label_create(s_state.network_list);
    lv_label_set_text(s_state.scanning_label, i18n_get(STR_WIFI_SCANNING));
    lv_obj_set_style_text_font(s_state.scanning_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_state.scanning_label, COLOR_TEXT_DIM, LV_PART_MAIN);
}

static void create_password_dialog(void)
{
    // Full-screen overlay
    s_state.password_dialog = lv_obj_create(s_state.screen);
    lv_obj_set_size(s_state.password_dialog, LV_PCT(100), LV_PCT(100));
    lv_obj_center(s_state.password_dialog);
    lv_obj_set_style_bg_color(s_state.password_dialog, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_state.password_dialog, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_state.password_dialog, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_state.password_dialog, 0, LV_PART_MAIN);
    disable_scrolling(s_state.password_dialog);
    lv_obj_add_flag(s_state.password_dialog, LV_OBJ_FLAG_HIDDEN);
    
    // Cancel/Connect buttons live in a bottom action bar (built after the
    // keyboard below), mirroring the Control edit dialog. No top header here.

    // Content panel with rounded corners - positioned in the upper area,
    // above the bottom action bar + keyboard.
    lv_obj_t* content = lv_obj_create(s_state.password_dialog);
    lv_obj_set_size(content, LV_PCT(90), LV_PCT(18));
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 40);  // Near the top now that the header is gone
    lv_obj_set_style_bg_color(content, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(content, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(content, 25, LV_PART_MAIN);
    disable_scrolling(content);
    
    // SSID label - prominent at top, centered
    s_state.password_ssid_label = lv_label_create(content);
    lv_label_set_text(s_state.password_ssid_label, "");
    lv_obj_set_style_text_font(s_state.password_ssid_label, &montserrat_32_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_state.password_ssid_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(s_state.password_ssid_label, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_user_data(s_state.password_ssid_label, (void*)"wifi_password_ssid");
    
    // Password input row - below SSID
    lv_obj_t* input_row = lv_obj_create(content);
    lv_obj_set_size(input_row, LV_PCT(95), 80);
    lv_obj_align(input_row, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_opa(input_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(input_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(input_row, 0, LV_PART_MAIN);
    disable_scrolling(input_row);
    
    // Password textarea
    s_state.password_textarea = lv_textarea_create(input_row);
    lv_obj_set_size(s_state.password_textarea, LV_PCT(82), LV_PCT(100));
    lv_obj_align(s_state.password_textarea, LV_ALIGN_LEFT_MID, 0, 0);
    lv_textarea_set_one_line(s_state.password_textarea, true);
    lv_textarea_set_password_mode(s_state.password_textarea, true);
    lv_textarea_set_placeholder_text(s_state.password_textarea, i18n_get(STR_WIFI_PASSWORD));
    lv_obj_set_user_data(s_state.password_textarea, (void*)"wifi_password_input");
    lv_obj_set_style_text_font(s_state.password_textarea, &lv_font_montserrat_32, LV_PART_MAIN);
    
    // Show password button
    s_state.show_password_btn = lv_btn_create(input_row);
    lv_obj_set_size(s_state.show_password_btn, LV_PCT(15), LV_PCT(100));
    lv_obj_align(s_state.show_password_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_state.show_password_btn, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_radius(s_state.show_password_btn, 10, LV_PART_MAIN);
    lv_obj_add_event_cb(s_state.show_password_btn, show_password_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_user_data(s_state.show_password_btn, (void*)"wifi_show_password");
    
    s_state.show_password_icon = lv_label_create(s_state.show_password_btn);
    lv_label_set_text(s_state.show_password_icon, LV_SYMBOL_EYE_CLOSE);
    lv_obj_set_style_text_font(s_state.show_password_icon, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_center(s_state.show_password_icon);
    
    // Keyboard at bottom - 25% height
    s_state.keyboard = lv_keyboard_create(s_state.password_dialog);
    lv_obj_set_size(s_state.keyboard, LV_PCT(100), LV_PCT(25));
    lv_obj_align(s_state.keyboard, LV_ALIGN_BOTTOM_MID, 0, -25);
    lv_obj_set_style_bg_color(s_state.keyboard, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_state.keyboard, FONT_NORMAL, LV_PART_ITEMS);
    lv_keyboard_set_textarea(s_state.keyboard, s_state.password_textarea);
    lv_obj_add_event_cb(s_state.keyboard, keyboard_ready_cb, LV_EVENT_READY, NULL);
    
    // Apply custom keyboard maps (without OK and keyboard-toggle buttons)
    lv_keyboard_set_map(s_state.keyboard, LV_KEYBOARD_MODE_TEXT_LOWER, kb_map_lc, kb_ctrl_lc);
    lv_keyboard_set_map(s_state.keyboard, LV_KEYBOARD_MODE_TEXT_UPPER, kb_map_uc, kb_ctrl_uc);
    lv_keyboard_set_map(s_state.keyboard, LV_KEYBOARD_MODE_SPECIAL, kb_map_spec, kb_ctrl_spec);

    // Fix #1: pop the pressed key up above the finger for visual confirmation.
    // The custom ctrl maps already carry LV_BUTTONMATRIX_CTRL_POPOVER on the
    // character keys (see KB_BTN in keyboard_maps.c); enabling popovers keeps
    // those flags instead of stripping them.
    lv_keyboard_set_popovers(s_state.keyboard, true);

    // Fix #2: bottom action bar (Cancel left / Connect right), mirroring the
    // Control edit dialog. Sits just above the keyboard.
    lv_obj_t* action_bar = lv_obj_create(s_state.password_dialog);
    lv_obj_set_size(action_bar, LV_PCT(100), 110);
    lv_obj_align_to(action_bar, s_state.keyboard, LV_ALIGN_OUT_TOP_MID, 0, -8);
    lv_obj_set_style_bg_color(action_bar, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(action_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(action_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(action_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(action_bar, 30, LV_PART_MAIN);
    disable_scrolling(action_bar);

    // Cancel — ghost/outline, left, quiet.
    s_state.cancel_btn = lv_btn_create(action_bar);
    lv_obj_set_size(s_state.cancel_btn, 300, 80);
    lv_obj_align(s_state.cancel_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_state.cancel_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_radius(s_state.cancel_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_state.cancel_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_state.cancel_btn, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_state.cancel_btn, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_add_event_cb(s_state.cancel_btn, cancel_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_user_data(s_state.cancel_btn, (void*)"wifi_cancel_btn");

    lv_obj_t* cancel_lbl = lv_label_create(s_state.cancel_btn);
    lv_label_set_text(cancel_lbl, i18n_get(STR_CANCEL));
    lv_obj_set_style_text_font(cancel_lbl, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(cancel_lbl, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_center(cancel_lbl);

    // Connect — filled accent, right, primary/dominant.
    s_state.connect_btn = lv_btn_create(action_bar);
    lv_obj_set_size(s_state.connect_btn, 300, 80);
    lv_obj_align(s_state.connect_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_state.connect_btn, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_state.connect_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_state.connect_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_state.connect_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_state.connect_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(s_state.connect_btn, connect_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_user_data(s_state.connect_btn, (void*)"wifi_connect_btn");

    lv_obj_t* connect_lbl = lv_label_create(s_state.connect_btn);
    lv_label_set_text(connect_lbl, i18n_get(STR_WIFI_CONNECT));
    lv_obj_set_style_text_font(connect_lbl, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(connect_lbl, COLOR_BG, LV_PART_MAIN);
    lv_obj_center(connect_lbl);

    // Keyboard must draw above the action bar so the top-row key popovers
    // (which render one row above the keyboard, into the bar's space) paint
    // on top of the bar instead of being clipped behind it.
    lv_obj_move_foreground(s_state.keyboard);
}

// ============================================================================
// UI Updates
// ============================================================================

static void update_connected_display(void)
{
    if (!s_state.connected_section) return;

    // Any definitive connected/disconnected render clears the transient
    // "connecting" indicator.
    if (s_state.connecting_section) {
        lv_obj_add_flag(s_state.connecting_section, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_state.is_connected) {
        // Show connected section
        lv_obj_remove_flag(s_state.connected_section, LV_OBJ_FLAG_HIDDEN);
        
        // Update SSID
        char ssid_buf[64];
        snprintf(ssid_buf, sizeof(ssid_buf), LV_SYMBOL_WIFI " %s", s_state.connected_ssid);
        lv_label_set_text(s_state.connected_ssid_label, ssid_buf);
        
        // Update IP
        char ip_buf[48];
        if (s_state.connected_ip[0]) {
            snprintf(ip_buf, sizeof(ip_buf), "%s: %s", i18n_get(STR_WIFI_IP_ADDRESS), s_state.connected_ip);
        } else {
            snprintf(ip_buf, sizeof(ip_buf), "%s: ...", i18n_get(STR_WIFI_IP_ADDRESS));
        }
        lv_label_set_text(s_state.connected_ip_label, ip_buf);
        
        // Update signal with icon and strength text
        int8_t rssi = wifi_mgr_get_rssi();
        const char* signal_icon;
        const char* strength_text;
        lv_color_t strength_color;
        
        if (rssi >= -50) {
            signal_icon = LV_SYMBOL_WIFI;
            strength_text = i18n_get(STR_WIFI_SIGNAL_EXCELLENT);
            strength_color = COLOR_SUCCESS;
        } else if (rssi >= -60) {
            signal_icon = LV_SYMBOL_WIFI;
            strength_text = i18n_get(STR_WIFI_SIGNAL_GOOD);
            strength_color = COLOR_SUCCESS;
        } else if (rssi >= -70) {
            signal_icon = LV_SYMBOL_WIFI;
            strength_text = i18n_get(STR_WIFI_SIGNAL_FAIR);
            strength_color = COLOR_WARNING;
        } else {
            signal_icon = LV_SYMBOL_WIFI;
            strength_text = i18n_get(STR_WIFI_SIGNAL_WEAK);
            strength_color = COLOR_ERROR;
        }
        
        char signal_buf[48];
        snprintf(signal_buf, sizeof(signal_buf), "%s %s (%d dBm)", signal_icon, strength_text, rssi);
        lv_label_set_text(s_state.signal_label, signal_buf);
        lv_obj_set_style_text_color(s_state.signal_label, strength_color, LV_PART_MAIN);
        
    } else {
        lv_obj_add_flag(s_state.connected_section, LV_OBJ_FLAG_HIDDEN);
    }
}

// Animation callback for dialog Y position
static void dialog_anim_cb(void* var, int32_t val)
{
    lv_obj_set_y((lv_obj_t*)var, val);
}

// Animation complete callback for hiding
static void dialog_hide_anim_ready_cb(lv_anim_t* a)
{
    lv_obj_t* dialog = (lv_obj_t*)lv_anim_get_user_data(a);
    lv_obj_add_flag(dialog, LV_OBJ_FLAG_HIDDEN);
}

static void show_password_dialog(const char* ssid, bool is_open)
{
    strncpy(s_state.selected_ssid, ssid, sizeof(s_state.selected_ssid) - 1);
    s_state.selected_is_open = is_open;
    
    // Update SSID label - just show the network name prominently
    lv_label_set_text(s_state.password_ssid_label, ssid);
    
    // Reset password field
    lv_textarea_set_text(s_state.password_textarea, "");
    s_state.password_visible = false;
    lv_textarea_set_password_mode(s_state.password_textarea, true);
    lv_label_set_text(s_state.show_password_icon, LV_SYMBOL_EYE_CLOSE);
    
    // Get screen height for animation
    lv_display_t* disp = lv_display_get_default();
    int32_t screen_height = lv_display_get_vertical_resolution(disp);
    
    // Position dialog off-screen at bottom, then animate up
    lv_obj_set_y(s_state.password_dialog, screen_height);
    lv_obj_remove_flag(s_state.password_dialog, LV_OBJ_FLAG_HIDDEN);
    
    // Animate slide up
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_state.password_dialog);
    lv_anim_set_values(&a, screen_height, 0);
    lv_anim_set_duration(&a, 300);
    lv_anim_set_exec_cb(&a, dialog_anim_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void hide_password_dialog(void)
{
    if (!s_state.password_dialog) return;
    
    // Get screen height for animation
    lv_display_t* disp = lv_display_get_default();
    int32_t screen_height = lv_display_get_vertical_resolution(disp);
    
    // Animate slide down
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_state.password_dialog);
    lv_anim_set_values(&a, 0, screen_height);
    lv_anim_set_duration(&a, 250);
    lv_anim_set_exec_cb(&a, dialog_anim_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_user_data(&a, s_state.password_dialog);
    lv_anim_set_completed_cb(&a, dialog_hide_anim_ready_cb);
    lv_anim_start(&a);
}

// ============================================================================
// Event Handlers
// ============================================================================

static void back_btn_cb(lv_event_t* e)
{
    (void)e;
    ESP_LOGI(TAG, "Back button clicked");
    
    // Save callback before modifying state
    void (*on_back_cb)(void) = s_state.config.on_back;
    
    // First, load the previous screen (so it becomes active)
    if (on_back_cb) {
        on_back_cb();
    } else {
        settings_menu_show();
    }
    
    // Now it's safe to close/delete our screen (no longer active)
    wifi_screen_close();
}

static void disconnect_btn_cb(lv_event_t* e)
{
    (void)e;
    ESP_LOGI(TAG, "Disconnect clicked");
    
    if (s_state.config.on_wifi_disconnect) {
        s_state.config.on_wifi_disconnect();
    }
}

static void network_item_cb(lv_event_t* e)
{
    lv_obj_t* row = (lv_obj_t*)lv_event_get_target(e);
    settings_wifi_network_t* net = (settings_wifi_network_t*)lv_obj_get_user_data(row);
    
    if (net) {
        bool is_open = (net->authmode == 0);
        ESP_LOGI(TAG, "Network selected: %s (open=%d)", net->ssid, is_open);
        
        if (is_open) {
            // Open network - connect directly without password dialog
            if (s_state.mock_mode) {
                // In mock mode, don't actually connect — just log
                ESP_LOGI(TAG, "Mock mode: suppressing connect to open network '%s'", net->ssid);
            } else if (s_state.config.on_wifi_connect) {
                s_state.config.on_wifi_connect(net->ssid, "");
            }
        } else {
            // Secured network - show password dialog
            show_password_dialog(net->ssid, is_open);
        }
    }
}

static void connect_btn_cb(lv_event_t* e)
{
    (void)e;
    
    const char* password = lv_textarea_get_text(s_state.password_textarea);
    
    ESP_LOGI(TAG, "Connecting to %s", s_state.selected_ssid);
    
    if (s_state.mock_mode) {
        // In mock mode, don't actually connect — just log and dismiss
        ESP_LOGI(TAG, "Mock mode: suppressing connect to '%s'", s_state.selected_ssid);
    } else if (s_state.config.on_wifi_connect) {
        s_state.config.on_wifi_connect(s_state.selected_ssid, 
                                       s_state.selected_is_open ? "" : password);
    }
    
    hide_password_dialog();
}

static void cancel_btn_cb(lv_event_t* e)
{
    (void)e;
    hide_password_dialog();
}

static void show_password_cb(lv_event_t* e)
{
    (void)e;
    
    s_state.password_visible = !s_state.password_visible;
    lv_textarea_set_password_mode(s_state.password_textarea, !s_state.password_visible);
    
    if (s_state.password_visible) {
        lv_label_set_text(s_state.show_password_icon, LV_SYMBOL_EYE_OPEN);
    } else {
        lv_label_set_text(s_state.show_password_icon, LV_SYMBOL_EYE_CLOSE);
    }
}

static void keyboard_ready_cb(lv_event_t* e)
{
    (void)e;
    // Enter pressed on keyboard - connect
    connect_btn_cb(NULL);
}

static void scan_timer_cb(lv_timer_t* timer)
{
    (void)timer;
    
    if (s_state.visible) {
        ESP_LOGI(TAG, "Auto-scan triggered");
        wifi_screen_trigger_scan();
    }
}

void wifi_screen_set_mock_mode(bool enable)
{
    s_state.mock_mode = enable;
    
    if (enable) {
        // Pause scan timer so real scans don't overwrite injected networks
        if (s_state.scan_timer) {
            lv_timer_pause(s_state.scan_timer);
            ESP_LOGI(TAG, "Mock mode ON: scan timer paused");
        }
    } else {
        // Resume scan timer
        if (s_state.scan_timer) {
            lv_timer_resume(s_state.scan_timer);
            ESP_LOGI(TAG, "Mock mode OFF: scan timer resumed");
        }
    }
}

bool wifi_screen_is_mock_mode(void)
{
    return s_state.mock_mode;
}

bool wifi_screen_is_password_dialog_visible(void)
{
    if (!s_state.password_dialog) return false;
    return !lv_obj_has_flag(s_state.password_dialog, LV_OBJ_FLAG_HIDDEN);
}
