/*
 * Arctic Heat Pump Controller
 * Settings Screen - WiFi Panel Implementation
 */
#include "settings_wifi_panel.h"
#include "settings_common.h"
#include "wifi_manager.h"
#include <string.h>
#include <stdio.h>

static const char* TAG = "settings_wifi";

// ============================================================================
// Forward Declarations
// ============================================================================

static void wifi_disconnect_event_cb(lv_event_t* e);
static void wifi_connect_event_cb(lv_event_t* e);
static void wifi_cancel_event_cb(lv_event_t* e);
static void wifi_keyboard_ready_cb(lv_event_t* e);
static void wifi_show_password_event_cb(lv_event_t* e);
static void wifi_scan_timer_cb(lv_timer_t* timer);

// ============================================================================
// Event Handlers
// ============================================================================

static void wifi_disconnect_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        settings_state_t* state = settings_get_state();
        ESP_LOGI(TAG, "WiFi disconnect clicked");
        if (state->config.on_wifi_disconnect) {
            state->config.on_wifi_disconnect();
        }
    }
}

static void wifi_connect_event_cb(lv_event_t* e)
{
    (void)e;
    settings_state_t* state = settings_get_state();
    
    const char* password = lv_textarea_get_text(state->wifi_password_textarea);
    
    if (state->config.on_wifi_connect) {
        state->config.on_wifi_connect(state->wifi_selected_ssid, 
                                      state->wifi_selected_is_open ? "" : password);
    }
    
    wifi_hide_password_dialog();
}

static void wifi_cancel_event_cb(lv_event_t* e)
{
    (void)e;
    wifi_hide_password_dialog();
}

static void wifi_keyboard_ready_cb(lv_event_t* e)
{
    (void)e;
    wifi_connect_event_cb(NULL);
}

static void wifi_show_password_event_cb(lv_event_t* e)
{
    (void)e;
    settings_state_t* state = settings_get_state();
    
    state->wifi_password_visible = !state->wifi_password_visible;
    lv_textarea_set_password_mode(state->wifi_password_textarea, !state->wifi_password_visible);
    
    if (state->wifi_password_visible) {
        lv_label_set_text(state->wifi_show_password_icon, LV_SYMBOL_EYE_OPEN);
    } else {
        lv_label_set_text(state->wifi_show_password_icon, LV_SYMBOL_EYE_CLOSE);
    }
}

static void wifi_scan_timer_cb(lv_timer_t* timer)
{
    (void)timer;
    settings_state_t* state = settings_get_state();
    
    if (state->visible && state->active_panel == PANEL_WIFI) {
        ESP_LOGI(TAG, "WiFi auto-scan triggered");
        wifi_trigger_scan();
    }
}

// ============================================================================
// WiFi Functions
// ============================================================================

void wifi_trigger_scan(void)
{
    settings_state_t* state = settings_get_state();
    ESP_LOGI(TAG, "wifi_trigger_scan called, callback=%p", state->config.on_wifi_scan);
    if (state->config.on_wifi_scan) {
        settings_screen_set_scanning(true);
        state->config.on_wifi_scan();
    } else {
        ESP_LOGW(TAG, "No WiFi scan callback registered!");
    }
}

void wifi_update_connected_section(void)
{
    settings_state_t* state = settings_get_state();
    
    if (state->wifi_is_connected) {
        char ssid_text[64];
        char display_ssid[48];
        sanitize_ssid_for_display(display_ssid, state->wifi_connected_ssid, sizeof(display_ssid));
        snprintf(ssid_text, sizeof(ssid_text), LV_SYMBOL_WIFI " %s", display_ssid);
        lv_label_set_text(state->wifi_ssid_label, ssid_text);
        
        if (state->wifi_connected_ip[0]) {
            char ip_text[32];
            snprintf(ip_text, sizeof(ip_text), "IP: %s", state->wifi_connected_ip);
            lv_label_set_text(state->wifi_ip_label, ip_text);
        } else {
            lv_label_set_text(state->wifi_ip_label, "IP: Obtaining...");
        }
        
        // Update signal strength
        int8_t rssi = wifi_mgr_get_rssi();
        const char* strength_text;
        lv_color_t strength_color;
        
        if (rssi >= -50) {
            strength_text = "Signal: Excellent";
            strength_color = COLOR_SUCCESS;
        } else if (rssi >= -60) {
            strength_text = "Signal: Good";
            strength_color = COLOR_SUCCESS;
        } else if (rssi >= -70) {
            strength_text = "Signal: Fair";
            strength_color = COLOR_WARNING;
        } else {
            strength_text = "Signal: Weak";
            strength_color = COLOR_ERROR;
        }
        lv_label_set_text(state->wifi_signal_label, strength_text);
        lv_obj_set_style_text_color(state->wifi_signal_label, strength_color, LV_PART_MAIN);
        
        lv_obj_remove_flag(state->wifi_connected_section, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(state->wifi_connected_section, LV_OBJ_FLAG_HIDDEN);
    }
}

void wifi_show_password_dialog(const char* ssid, bool is_open)
{
    settings_state_t* state = settings_get_state();
    
    strncpy(state->wifi_selected_ssid, ssid, sizeof(state->wifi_selected_ssid) - 1);
    state->wifi_selected_is_open = is_open;
    
    char display_ssid[48];
    sanitize_ssid_for_display(display_ssid, ssid, sizeof(display_ssid));
    char ssid_text[64];
    snprintf(ssid_text, sizeof(ssid_text), "Network: %s", display_ssid);
    lv_label_set_text(state->wifi_password_ssid_label, ssid_text);
    
    lv_textarea_set_text(state->wifi_password_textarea, "");
    state->wifi_password_visible = false;
    lv_textarea_set_password_mode(state->wifi_password_textarea, true);
    lv_label_set_text(state->wifi_show_password_icon, LV_SYMBOL_EYE_CLOSE);
    
    if (is_open) {
        lv_obj_add_flag(state->wifi_password_textarea, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(state->wifi_show_password_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(state->wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(state->wifi_password_textarea, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(state->wifi_show_password_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(state->wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    
    lv_obj_remove_flag(state->wifi_password_dialog, LV_OBJ_FLAG_HIDDEN);
}

void wifi_hide_password_dialog(void)
{
    settings_state_t* state = settings_get_state();
    if (state->wifi_password_dialog) {
        lv_obj_add_flag(state->wifi_password_dialog, LV_OBJ_FLAG_HIDDEN);
    }
}

void wifi_start_scan_timer(void)
{
    settings_state_t* state = settings_get_state();
    if (!state->wifi_scan_timer) {
        state->wifi_scan_timer = lv_timer_create(wifi_scan_timer_cb, SCAN_INTERVAL_MS, NULL);
    }
}

void wifi_stop_scan_timer(void)
{
    settings_state_t* state = settings_get_state();
    if (state->wifi_scan_timer) {
        lv_timer_delete(state->wifi_scan_timer);
        state->wifi_scan_timer = NULL;
    }
}

void wifi_panel_cleanup(void)
{
    wifi_stop_scan_timer();
}

// ============================================================================
// WiFi Panel Creation
// ============================================================================

void wifi_panel_create(lv_obj_t* parent)
{
    settings_state_t* state = settings_get_state();
    
    state->wifi_panel = lv_obj_create(parent);
    lv_obj_set_size(state->wifi_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(state->wifi_panel, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(state->wifi_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state->wifi_panel, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(state->wifi_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(state->wifi_panel, 15, LV_PART_MAIN);
    disable_scrolling(state->wifi_panel);
    
    // Connected section - horizontal layout with left info, right button
    state->wifi_connected_section = lv_obj_create(state->wifi_panel);
    lv_obj_set_size(state->wifi_connected_section, LV_PCT(100), 120);
    lv_obj_set_style_bg_color(state->wifi_connected_section, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_color(state->wifi_connected_section, COLOR_SUCCESS, LV_PART_MAIN);
    lv_obj_set_style_border_width(state->wifi_connected_section, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(state->wifi_connected_section, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state->wifi_connected_section, 20, LV_PART_MAIN);
    lv_obj_set_flex_grow(state->wifi_connected_section, 0);
    disable_scrolling(state->wifi_connected_section);
    lv_obj_add_flag(state->wifi_connected_section, LV_OBJ_FLAG_HIDDEN);
    
    // Left side: Connection info
    lv_obj_t* info_container = lv_obj_create(state->wifi_connected_section);
    lv_obj_set_size(info_container, LV_PCT(70), LV_PCT(100));
    lv_obj_align(info_container, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(info_container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(info_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(info_container, 0, LV_PART_MAIN);
    disable_scrolling(info_container);
    
    lv_obj_t* conn_title = lv_label_create(info_container);
    lv_label_set_text(conn_title, LV_SYMBOL_OK " Connected");
    lv_obj_set_style_text_font(conn_title, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(conn_title, COLOR_SUCCESS, LV_PART_MAIN);
    lv_obj_align(conn_title, LV_ALIGN_TOP_LEFT, 0, 0);
    
    state->wifi_ssid_label = lv_label_create(info_container);
    lv_label_set_text(state->wifi_ssid_label, LV_SYMBOL_WIFI " NetworkName");
    lv_obj_set_style_text_font(state->wifi_ssid_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(state->wifi_ssid_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(state->wifi_ssid_label, LV_ALIGN_TOP_LEFT, 0, 25);
    
    state->wifi_ip_label = lv_label_create(info_container);
    lv_label_set_text(state->wifi_ip_label, "IP: 192.168.1.100");
    lv_obj_set_style_text_font(state->wifi_ip_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(state->wifi_ip_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(state->wifi_ip_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    
    state->wifi_signal_label = lv_label_create(info_container);
    lv_label_set_text(state->wifi_signal_label, "Signal: Excellent");
    lv_obj_set_style_text_font(state->wifi_signal_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(state->wifi_signal_label, COLOR_SUCCESS, LV_PART_MAIN);
    lv_obj_align(state->wifi_signal_label, LV_ALIGN_BOTTOM_LEFT, 220, 0);
    
    // Right side: Disconnect button
    state->wifi_disconnect_btn = lv_btn_create(state->wifi_connected_section);
    lv_obj_set_size(state->wifi_disconnect_btn, 130, 45);
    lv_obj_align(state->wifi_disconnect_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(state->wifi_disconnect_btn, COLOR_DISCONNECT, LV_PART_MAIN);
    lv_obj_set_style_radius(state->wifi_disconnect_btn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(state->wifi_disconnect_btn, wifi_disconnect_event_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t* disc_lbl = lv_label_create(state->wifi_disconnect_btn);
    lv_label_set_text(disc_lbl, "Disconnect");
    lv_obj_set_style_text_font(disc_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_center(disc_lbl);
    
    // Networks section
    state->wifi_networks_section = lv_obj_create(state->wifi_panel);
    lv_obj_set_size(state->wifi_networks_section, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(state->wifi_networks_section, 1);
    lv_obj_set_style_bg_color(state->wifi_networks_section, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_width(state->wifi_networks_section, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(state->wifi_networks_section, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state->wifi_networks_section, 15, LV_PART_MAIN);
    disable_scrolling(state->wifi_networks_section);
    
    state->wifi_networks_title = lv_label_create(state->wifi_networks_section);
    lv_label_set_text(state->wifi_networks_title, "Available Networks");
    lv_obj_set_style_text_font(state->wifi_networks_title, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(state->wifi_networks_title, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(state->wifi_networks_title, LV_ALIGN_TOP_LEFT, 0, 0);
    
    state->wifi_network_list = lv_obj_create(state->wifi_networks_section);
    lv_obj_set_size(state->wifi_network_list, LV_PCT(100), LV_PCT(90));
    lv_obj_align(state->wifi_network_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(state->wifi_network_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(state->wifi_network_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state->wifi_network_list, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(state->wifi_network_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(state->wifi_network_list, 8, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(state->wifi_network_list, LV_SCROLLBAR_MODE_AUTO);
    
    lv_obj_t* scan_label = lv_label_create(state->wifi_network_list);
    lv_label_set_text(scan_label, "Scanning for networks...");
    lv_obj_set_style_text_font(scan_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(scan_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    
    // Password dialog (attached to screen, not panel)
    state->wifi_password_dialog = lv_obj_create(state->screen);
    lv_obj_set_size(state->wifi_password_dialog, LV_PCT(100), LV_PCT(100));
    lv_obj_center(state->wifi_password_dialog);
    lv_obj_set_style_bg_color(state->wifi_password_dialog, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state->wifi_password_dialog, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_border_width(state->wifi_password_dialog, 0, LV_PART_MAIN);
    disable_scrolling(state->wifi_password_dialog);
    lv_obj_add_flag(state->wifi_password_dialog, LV_OBJ_FLAG_HIDDEN);
    
    lv_obj_t* dialog_box = lv_obj_create(state->wifi_password_dialog);
    lv_obj_set_size(dialog_box, 600, 450);
    lv_obj_align(dialog_box, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(dialog_box, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_color(dialog_box, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_border_width(dialog_box, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(dialog_box, 15, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dialog_box, 20, LV_PART_MAIN);
    disable_scrolling(dialog_box);
    
    lv_obj_t* dialog_title = lv_label_create(dialog_box);
    lv_label_set_text(dialog_title, "Enter Password");
    lv_obj_set_style_text_font(dialog_title, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(dialog_title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(dialog_title, LV_ALIGN_TOP_MID, 0, 0);
    
    state->wifi_password_ssid_label = lv_label_create(dialog_box);
    lv_label_set_text(state->wifi_password_ssid_label, "Network: ");
    lv_obj_set_style_text_font(state->wifi_password_ssid_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(state->wifi_password_ssid_label, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(state->wifi_password_ssid_label, LV_ALIGN_TOP_MID, 0, 35);
    
    state->wifi_password_textarea = lv_textarea_create(dialog_box);
    lv_obj_set_size(state->wifi_password_textarea, 400, 45);
    lv_obj_align(state->wifi_password_textarea, LV_ALIGN_TOP_MID, 0, 65);
    lv_textarea_set_one_line(state->wifi_password_textarea, true);
    lv_textarea_set_password_mode(state->wifi_password_textarea, true);
    lv_textarea_set_placeholder_text(state->wifi_password_textarea, "Password");
    lv_obj_set_style_text_font(state->wifi_password_textarea, &lv_font_montserrat_16, LV_PART_MAIN);
    
    state->wifi_show_password_btn = lv_btn_create(dialog_box);
    lv_obj_set_size(state->wifi_show_password_btn, 45, 45);
    lv_obj_align(state->wifi_show_password_btn, LV_ALIGN_TOP_RIGHT, -25, 65);
    lv_obj_set_style_bg_opa(state->wifi_show_password_btn, LV_OPA_50, LV_PART_MAIN);
    lv_obj_add_event_cb(state->wifi_show_password_btn, wifi_show_password_event_cb, LV_EVENT_CLICKED, NULL);
    
    state->wifi_show_password_icon = lv_label_create(state->wifi_show_password_btn);
    lv_label_set_text(state->wifi_show_password_icon, LV_SYMBOL_EYE_CLOSE);
    lv_obj_center(state->wifi_show_password_icon);
    
    state->wifi_keyboard = lv_keyboard_create(dialog_box);
    lv_obj_set_size(state->wifi_keyboard, 560, 200);
    lv_obj_align(state->wifi_keyboard, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_keyboard_set_textarea(state->wifi_keyboard, state->wifi_password_textarea);
    lv_obj_add_event_cb(state->wifi_keyboard, wifi_keyboard_ready_cb, LV_EVENT_READY, NULL);
    
    lv_obj_t* btn_row = lv_obj_create(dialog_box);
    lv_obj_set_size(btn_row, LV_PCT(100), 45);
    lv_obj_align(btn_row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_row, 0, LV_PART_MAIN);
    disable_scrolling(btn_row);
    
    state->wifi_cancel_btn = lv_btn_create(btn_row);
    lv_obj_set_size(state->wifi_cancel_btn, 120, 40);
    lv_obj_align(state->wifi_cancel_btn, LV_ALIGN_LEFT_MID, 50, 0);
    lv_obj_set_style_bg_color(state->wifi_cancel_btn, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_add_event_cb(state->wifi_cancel_btn, wifi_cancel_event_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t* cancel_lbl = lv_label_create(state->wifi_cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_center(cancel_lbl);
    
    state->wifi_connect_btn = lv_btn_create(btn_row);
    lv_obj_set_size(state->wifi_connect_btn, 120, 40);
    lv_obj_align(state->wifi_connect_btn, LV_ALIGN_RIGHT_MID, -50, 0);
    lv_obj_set_style_bg_color(state->wifi_connect_btn, COLOR_SUCCESS, LV_PART_MAIN);
    lv_obj_add_event_cb(state->wifi_connect_btn, wifi_connect_event_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t* connect_lbl = lv_label_create(state->wifi_connect_btn);
    lv_label_set_text(connect_lbl, "Connect");
    lv_obj_set_style_text_font(connect_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_center(connect_lbl);
}
