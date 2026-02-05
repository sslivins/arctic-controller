/*
 * Arctic Heat Pump Controller
 * WiFi Settings Screen Implementation - iPhone-style layout
 */
#include "wifi_screen.h"
#include <string.h>
#include <stdio.h>
#include <esp_log.h>

static const char* TAG = "wifi_screen";

// ============================================================================
// Helper: Sanitize SSID for display (replace Unicode quotes with ASCII)
// ============================================================================

static void sanitize_ssid_for_display(char* dest, const char* src, size_t dest_size)
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

// ============================================================================
// Internal State
// ============================================================================

static struct {
    bool visible;
    wifi_screen_config_t config;
    
    // Screen objects
    lv_obj_t* screen;
    lv_obj_t* header;
    lv_obj_t* back_btn;
    
    // Connected section
    lv_obj_t* connected_section;
    lv_obj_t* connected_ssid_label;
    lv_obj_t* connected_ip_label;
    lv_obj_t* disconnect_btn;
    bool is_connected;
    char connected_ssid[33];
    char connected_ip[16];
    
    // Available networks section
    lv_obj_t* networks_section;
    lv_obj_t* networks_title;
    lv_obj_t* network_list;
    lv_obj_t* scanning_spinner;
    lv_obj_t* scanning_label;
    
    // Auto-scan timer
    lv_timer_t* scan_timer;
    
    // Password dialog
    lv_obj_t* password_dialog;
    lv_obj_t* password_ssid_label;
    lv_obj_t* password_textarea;
    lv_obj_t* show_password_btn;
    lv_obj_t* show_password_icon;
    lv_obj_t* keyboard;
    lv_obj_t* connect_btn;
    lv_obj_t* cancel_btn;
    bool password_visible;
    
    // Current selection
    char selected_ssid[33];
    bool selected_is_open;
    
    // Network data storage
    wifi_network_info_t networks[20];
    uint8_t network_count;
    
} wifi_state = {};

// ============================================================================
// Style Constants
// ============================================================================

#define COLOR_BG            lv_color_hex(0x1a1a2e)
#define COLOR_PANEL         lv_color_hex(0x16213e)
#define COLOR_BORDER        lv_color_hex(0x0f3460)
#define COLOR_ACCENT        lv_color_hex(0x00d4ff)
#define COLOR_TEXT          lv_color_hex(0xffffff)
#define COLOR_TEXT_DIM      lv_color_hex(0x888888)
#define COLOR_BTN           lv_color_hex(0x0f3460)
#define COLOR_BTN_PRESSED   lv_color_hex(0x1a5276)
#define COLOR_SUCCESS       lv_color_hex(0x27ae60)
#define COLOR_WARNING       lv_color_hex(0xf39c12)
#define COLOR_ERROR         lv_color_hex(0xe74c3c)
#define COLOR_DISCONNECT    lv_color_hex(0xe74c3c)

// Auto-scan interval (10 seconds)
#define SCAN_INTERVAL_MS    10000

// ============================================================================
// Forward Declarations
// ============================================================================

static void create_header(lv_obj_t* parent);
static void create_connected_section(lv_obj_t* parent);
static void create_networks_section(lv_obj_t* parent);
static void create_password_dialog(void);
static void show_password_dialog(const char* ssid, bool is_open);
static void hide_password_dialog(void);
static void update_connected_section(void);
static void trigger_scan(void);

static void on_back_clicked(lv_event_t* e);
static void on_disconnect_clicked(lv_event_t* e);
static void on_network_clicked(lv_event_t* e);
static void on_connect_clicked(lv_event_t* e);
static void on_cancel_clicked(lv_event_t* e);
static void on_keyboard_ready(lv_event_t* e);
static void on_show_password_clicked(lv_event_t* e);
static void scan_timer_cb(lv_timer_t* timer);

static const char* get_signal_icon(int8_t rssi);

// ============================================================================
// Public API
// ============================================================================

void wifi_screen_create(const wifi_screen_config_t* config)
{
    if (wifi_state.visible) {
        return;
    }
    
    // Store config
    if (config) {
        wifi_state.config = *config;
    } else {
        memset(&wifi_state.config, 0, sizeof(wifi_state.config));
    }
    
    // Reset connection state
    wifi_state.is_connected = false;
    wifi_state.connected_ssid[0] = '\0';
    wifi_state.connected_ip[0] = '\0';
    
    // Create screen
    wifi_state.screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(wifi_state.screen, COLOR_BG, LV_PART_MAIN);
    
    // Create header
    create_header(wifi_state.screen);
    
    // Main content container
    lv_obj_t* content = lv_obj_create(wifi_state.screen);
    lv_obj_set_size(content, 760, 540);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 15, LV_PART_MAIN);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    
    // Create connected section (hidden initially)
    create_connected_section(content);
    
    // Create available networks section
    create_networks_section(content);
    
    // Create password dialog (hidden initially)
    create_password_dialog();
    
    // Load the screen
    lv_scr_load(wifi_state.screen);
    wifi_state.visible = true;
    
    // Start auto-scan timer
    wifi_state.scan_timer = lv_timer_create(scan_timer_cb, SCAN_INTERVAL_MS, NULL);
    
    // Trigger initial scan
    trigger_scan();
    
    ESP_LOGI(TAG, "WiFi screen created with auto-scan");
}

void wifi_screen_close(void)
{
    if (!wifi_state.visible) {
        return;
    }
    
    // Stop auto-scan timer
    if (wifi_state.scan_timer) {
        lv_timer_del(wifi_state.scan_timer);
        wifi_state.scan_timer = NULL;
    }
    
    // Hide password dialog
    hide_password_dialog();
    
    // Notify close callback
    if (wifi_state.config.on_close) {
        wifi_state.config.on_close();
    }
    
    wifi_state.visible = false;
    ESP_LOGI(TAG, "WiFi screen closed");
}

bool wifi_screen_is_visible(void)
{
    return wifi_state.visible;
}

void wifi_screen_update_networks(const wifi_network_info_t* networks, uint8_t count)
{
    if (!wifi_state.visible || !wifi_state.network_list) {
        return;
    }
    
    // Deduplicate networks by SSID (keep strongest signal)
    wifi_network_info_t deduped[20];
    uint8_t deduped_count = 0;
    
    for (uint8_t i = 0; i < count && i < 20; i++) {
        const wifi_network_info_t* net = &networks[i];
        
        // Skip empty SSIDs (hidden networks)
        if (net->ssid[0] == '\0') {
            continue;
        }
        
        // Skip the currently connected network (shown in connected section)
        if (wifi_state.is_connected && strcmp(net->ssid, wifi_state.connected_ssid) == 0) {
            continue;
        }
        
        // Check if SSID already exists in deduped list
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
            memcpy(&deduped[deduped_count], net, sizeof(wifi_network_info_t));
            deduped_count++;
        }
    }
    
    // Store deduplicated network data
    wifi_state.network_count = deduped_count;
    if (deduped_count > 0) {
        memcpy(wifi_state.networks, deduped, deduped_count * sizeof(wifi_network_info_t));
    }
    
    // Clear existing list items
    lv_obj_clean(wifi_state.network_list);
    
    if (wifi_state.network_count == 0) {
        lv_obj_t* empty_label = lv_label_create(wifi_state.network_list);
        lv_label_set_text(empty_label, "Scanning for networks...");
        lv_obj_set_style_text_font(empty_label, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(empty_label, COLOR_TEXT_DIM, LV_PART_MAIN);
        lv_obj_set_style_text_align(empty_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_center(empty_label);
        return;
    }
    
    // Create list items for each network
    for (uint8_t i = 0; i < wifi_state.network_count; i++) {
        const wifi_network_info_t* net = &wifi_state.networks[i];
        
        // Create row container
        lv_obj_t* row = lv_obj_create(wifi_state.network_list);
        lv_obj_set_size(row, lv_pct(100), 55);
        lv_obj_set_style_bg_color(row, COLOR_PANEL, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, COLOR_BTN, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(row, 15, LV_PART_MAIN);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, on_network_clicked, LV_EVENT_CLICKED, (void*)(uintptr_t)i);
        
        // Signal strength icon
        lv_obj_t* signal = lv_label_create(row);
        lv_label_set_text(signal, get_signal_icon(net->rssi));
        lv_obj_set_style_text_font(signal, &lv_font_montserrat_24, LV_PART_MAIN);
        lv_obj_set_style_text_color(signal, COLOR_ACCENT, LV_PART_MAIN);
        lv_obj_align(signal, LV_ALIGN_LEFT_MID, 0, 0);
        
        // Network name
        char display_ssid[64];
        sanitize_ssid_for_display(display_ssid, net->ssid, sizeof(display_ssid));
        lv_obj_t* name = lv_label_create(row);
        lv_label_set_text(name, display_ssid);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_24, LV_PART_MAIN);
        lv_obj_set_style_text_color(name, COLOR_TEXT, LV_PART_MAIN);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 40, 0);
        
        // Lock icon for secured networks
        if (net->authmode > 0) {
            lv_obj_t* lock = lv_label_create(row);
            lv_label_set_text(lock, LV_SYMBOL_EYE_CLOSE);
            lv_obj_set_style_text_font(lock, &lv_font_montserrat_16, LV_PART_MAIN);
            lv_obj_set_style_text_color(lock, COLOR_TEXT_DIM, LV_PART_MAIN);
            lv_obj_align(lock, LV_ALIGN_RIGHT_MID, -10, 0);
        }
    }
}

void wifi_screen_set_scanning(bool scanning)
{
    if (!wifi_state.visible) {
        return;
    }
    
    // Update the networks title to show scanning status
    if (wifi_state.networks_title) {
        if (scanning) {
            lv_label_set_text(wifi_state.networks_title, "Available Networks  " LV_SYMBOL_REFRESH);
        } else {
            lv_label_set_text(wifi_state.networks_title, "Available Networks");
        }
    }
}

void wifi_screen_set_connection_status(bool connected, const char* ssid, const char* ip_addr)
{
    wifi_state.is_connected = connected;
    
    if (connected && ssid) {
        strncpy(wifi_state.connected_ssid, ssid, sizeof(wifi_state.connected_ssid) - 1);
        if (ip_addr) {
            strncpy(wifi_state.connected_ip, ip_addr, sizeof(wifi_state.connected_ip) - 1);
        } else {
            wifi_state.connected_ip[0] = '\0';
        }
    } else {
        wifi_state.connected_ssid[0] = '\0';
        wifi_state.connected_ip[0] = '\0';
    }
    
    if (wifi_state.visible) {
        update_connected_section();
    }
}

void wifi_screen_show_error(const char* message)
{
    if (!wifi_state.visible) {
        return;
    }
    
    lv_obj_t* msgbox = lv_msgbox_create(wifi_state.screen);
    lv_msgbox_add_title(msgbox, "Error");
    lv_msgbox_add_text(msgbox, message);
    lv_msgbox_add_close_button(msgbox);
    lv_obj_center(msgbox);
}

// ============================================================================
// Internal Functions
// ============================================================================

static void create_header(lv_obj_t* parent)
{
    wifi_state.header = lv_obj_create(parent);
    lv_obj_set_size(wifi_state.header, lv_pct(100), 70);
    lv_obj_align(wifi_state.header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(wifi_state.header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(wifi_state.header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(wifi_state.header, 25, LV_PART_MAIN);
    lv_obj_set_style_pad_right(wifi_state.header, 25, LV_PART_MAIN);
    lv_obj_clear_flag(wifi_state.header, LV_OBJ_FLAG_SCROLLABLE);
    
    // Title (left aligned)
    lv_obj_t* title = lv_label_create(wifi_state.header);
    lv_label_set_text(title, LV_SYMBOL_WIFI " WiFi");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);
    
    // Close button (X button on right, matching time screen)
    wifi_state.back_btn = lv_btn_create(wifi_state.header);
    lv_obj_set_size(wifi_state.back_btn, 50, 50);
    lv_obj_align(wifi_state.back_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(wifi_state.back_btn, COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_color(wifi_state.back_btn, COLOR_BTN_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_radius(wifi_state.back_btn, 25, LV_PART_MAIN);
    lv_obj_add_event_cb(wifi_state.back_btn, on_back_clicked, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t* close_icon = lv_label_create(wifi_state.back_btn);
    lv_label_set_text(close_icon, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(close_icon, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(close_icon, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_center(close_icon);
}

static void create_connected_section(lv_obj_t* parent)
{
    // Connected network section (iPhone-style)
    wifi_state.connected_section = lv_obj_create(parent);
    lv_obj_set_size(wifi_state.connected_section, lv_pct(100), 120);
    lv_obj_set_style_bg_color(wifi_state.connected_section, COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_border_color(wifi_state.connected_section, COLOR_SUCCESS, LV_PART_MAIN);
    lv_obj_set_style_border_width(wifi_state.connected_section, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(wifi_state.connected_section, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(wifi_state.connected_section, 15, LV_PART_MAIN);
    lv_obj_clear_flag(wifi_state.connected_section, LV_OBJ_FLAG_SCROLLABLE);
    
    // Section title
    lv_obj_t* section_title = lv_label_create(wifi_state.connected_section);
    lv_label_set_text(section_title, "Connected");
    lv_obj_set_style_text_font(section_title, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(section_title, COLOR_SUCCESS, LV_PART_MAIN);
    lv_obj_align(section_title, LV_ALIGN_TOP_LEFT, 0, 0);
    
    // Connected SSID with WiFi icon
    wifi_state.connected_ssid_label = lv_label_create(wifi_state.connected_section);
    lv_label_set_text(wifi_state.connected_ssid_label, LV_SYMBOL_WIFI " NetworkName");
    lv_obj_set_style_text_font(wifi_state.connected_ssid_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(wifi_state.connected_ssid_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(wifi_state.connected_ssid_label, LV_ALIGN_TOP_LEFT, 0, 25);
    
    // IP Address
    wifi_state.connected_ip_label = lv_label_create(wifi_state.connected_section);
    lv_label_set_text(wifi_state.connected_ip_label, "IP: 192.168.1.100");
    lv_obj_set_style_text_font(wifi_state.connected_ip_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(wifi_state.connected_ip_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(wifi_state.connected_ip_label, LV_ALIGN_TOP_LEFT, 0, 55);
    
    // Disconnect button
    wifi_state.disconnect_btn = lv_btn_create(wifi_state.connected_section);
    lv_obj_set_size(wifi_state.disconnect_btn, 130, 40);
    lv_obj_align(wifi_state.disconnect_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(wifi_state.disconnect_btn, COLOR_DISCONNECT, LV_PART_MAIN);
    lv_obj_set_style_radius(wifi_state.disconnect_btn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(wifi_state.disconnect_btn, on_disconnect_clicked, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t* disconnect_label = lv_label_create(wifi_state.disconnect_btn);
    lv_label_set_text(disconnect_label, "Disconnect");
    lv_obj_set_style_text_font(disconnect_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_center(disconnect_label);
    
    // Initially hidden
    lv_obj_add_flag(wifi_state.connected_section, LV_OBJ_FLAG_HIDDEN);
}

static void create_networks_section(lv_obj_t* parent)
{
    // Networks section container
    wifi_state.networks_section = lv_obj_create(parent);
    lv_obj_set_size(wifi_state.networks_section, lv_pct(100), 400);
    lv_obj_set_style_bg_color(wifi_state.networks_section, COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_border_color(wifi_state.networks_section, COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(wifi_state.networks_section, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(wifi_state.networks_section, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(wifi_state.networks_section, 15, LV_PART_MAIN);
    lv_obj_clear_flag(wifi_state.networks_section, LV_OBJ_FLAG_SCROLLABLE);
    
    // Section title
    wifi_state.networks_title = lv_label_create(wifi_state.networks_section);
    lv_label_set_text(wifi_state.networks_title, "Available Networks");
    lv_obj_set_style_text_font(wifi_state.networks_title, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(wifi_state.networks_title, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(wifi_state.networks_title, LV_ALIGN_TOP_LEFT, 0, 0);
    
    // Network list (scrollable)
    wifi_state.network_list = lv_obj_create(wifi_state.networks_section);
    lv_obj_set_size(wifi_state.network_list, lv_pct(100), 340);
    lv_obj_align(wifi_state.network_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(wifi_state.network_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(wifi_state.network_list, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(wifi_state.network_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(wifi_state.network_list, 8, LV_PART_MAIN);
    
    // Initial scanning message
    lv_obj_t* scanning_label = lv_label_create(wifi_state.network_list);
    lv_label_set_text(scanning_label, "Scanning for networks...");
    lv_obj_set_style_text_font(scanning_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(scanning_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_center(scanning_label);
}

static void update_connected_section(void)
{
    if (wifi_state.is_connected) {
        // Update labels
        char ssid_text[64];
        char display_ssid[48];
        sanitize_ssid_for_display(display_ssid, wifi_state.connected_ssid, sizeof(display_ssid));
        snprintf(ssid_text, sizeof(ssid_text), LV_SYMBOL_WIFI " %s", display_ssid);
        lv_label_set_text(wifi_state.connected_ssid_label, ssid_text);
        
        if (wifi_state.connected_ip[0]) {
            char ip_text[32];
            snprintf(ip_text, sizeof(ip_text), "IP: %s", wifi_state.connected_ip);
            lv_label_set_text(wifi_state.connected_ip_label, ip_text);
        } else {
            lv_label_set_text(wifi_state.connected_ip_label, "IP: Obtaining...");
        }
        
        // Show connected section
        lv_obj_clear_flag(wifi_state.connected_section, LV_OBJ_FLAG_HIDDEN);
        
        // Adjust networks section size
        lv_obj_set_size(wifi_state.networks_section, lv_pct(100), 280);
        lv_obj_set_size(wifi_state.network_list, lv_pct(100), 220);
    } else {
        // Hide connected section
        lv_obj_add_flag(wifi_state.connected_section, LV_OBJ_FLAG_HIDDEN);
        
        // Restore networks section size
        lv_obj_set_size(wifi_state.networks_section, lv_pct(100), 400);
        lv_obj_set_size(wifi_state.network_list, lv_pct(100), 340);
    }
}

static void trigger_scan(void)
{
    if (wifi_state.config.on_scan) {
        wifi_screen_set_scanning(true);
        wifi_state.config.on_scan();
    }
}

static void create_password_dialog(void)
{
    // Create dialog container (fullscreen overlay)
    wifi_state.password_dialog = lv_obj_create(wifi_state.screen);
    lv_obj_set_size(wifi_state.password_dialog, lv_pct(100), lv_pct(100));
    lv_obj_center(wifi_state.password_dialog);
    lv_obj_set_style_bg_color(wifi_state.password_dialog, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(wifi_state.password_dialog, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_border_width(wifi_state.password_dialog, 0, LV_PART_MAIN);
    lv_obj_clear_flag(wifi_state.password_dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(wifi_state.password_dialog, LV_OBJ_FLAG_HIDDEN);
    
    // Dialog box
    lv_obj_t* dialog_box = lv_obj_create(wifi_state.password_dialog);
    lv_obj_set_size(dialog_box, 700, 500);
    lv_obj_align(dialog_box, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(dialog_box, COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_border_color(dialog_box, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_border_width(dialog_box, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(dialog_box, 15, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dialog_box, 20, LV_PART_MAIN);
    lv_obj_clear_flag(dialog_box, LV_OBJ_FLAG_SCROLLABLE);
    
    // Dialog title
    lv_obj_t* dialog_title = lv_label_create(dialog_box);
    lv_label_set_text(dialog_title, "Enter Password");
    lv_obj_set_style_text_font(dialog_title, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(dialog_title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(dialog_title, LV_ALIGN_TOP_MID, 0, 0);
    
    // Selected network name
    wifi_state.password_ssid_label = lv_label_create(dialog_box);
    lv_label_set_text(wifi_state.password_ssid_label, "Network: ");
    lv_obj_set_style_text_font(wifi_state.password_ssid_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(wifi_state.password_ssid_label, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(wifi_state.password_ssid_label, LV_ALIGN_TOP_MID, 0, 40);
    
    // Password input container
    lv_obj_t* password_container = lv_obj_create(dialog_box);
    lv_obj_set_size(password_container, 560, 50);
    lv_obj_align(password_container, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_style_bg_opa(password_container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(password_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(password_container, 0, LV_PART_MAIN);
    lv_obj_clear_flag(password_container, LV_OBJ_FLAG_SCROLLABLE);
    
    // Password textarea
    wifi_state.password_textarea = lv_textarea_create(password_container);
    lv_obj_set_size(wifi_state.password_textarea, 500, 50);
    lv_obj_align(wifi_state.password_textarea, LV_ALIGN_LEFT_MID, 0, 0);
    lv_textarea_set_placeholder_text(wifi_state.password_textarea, "Password...");
    lv_textarea_set_password_mode(wifi_state.password_textarea, true);
    lv_textarea_set_one_line(wifi_state.password_textarea, true);
    lv_obj_set_style_text_font(wifi_state.password_textarea, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(wifi_state.password_textarea, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_bg_color(wifi_state.password_textarea, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_border_color(wifi_state.password_textarea, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_border_width(wifi_state.password_textarea, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(wifi_state.password_textarea, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_left(wifi_state.password_textarea, 15, LV_PART_MAIN);
    
    // Show/hide password button
    wifi_state.password_visible = false;
    wifi_state.show_password_btn = lv_btn_create(password_container);
    lv_obj_set_size(wifi_state.show_password_btn, 50, 50);
    lv_obj_align(wifi_state.show_password_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(wifi_state.show_password_btn, COLOR_BTN, LV_PART_MAIN);
    lv_obj_set_style_radius(wifi_state.show_password_btn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(wifi_state.show_password_btn, on_show_password_clicked, LV_EVENT_CLICKED, NULL);
    
    wifi_state.show_password_icon = lv_label_create(wifi_state.show_password_btn);
    lv_label_set_text(wifi_state.show_password_icon, LV_SYMBOL_EYE_CLOSE);
    lv_obj_set_style_text_font(wifi_state.show_password_icon, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_center(wifi_state.show_password_icon);
    
    // Button container
    lv_obj_t* btn_container = lv_obj_create(dialog_box);
    lv_obj_set_size(btn_container, 500, 60);
    lv_obj_align(btn_container, LV_ALIGN_TOP_MID, 0, 140);
    lv_obj_set_style_bg_opa(btn_container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_container, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn_container, LV_OBJ_FLAG_SCROLLABLE);
    
    // Cancel button
    wifi_state.cancel_btn = lv_btn_create(btn_container);
    lv_obj_set_size(wifi_state.cancel_btn, 150, 50);
    lv_obj_align(wifi_state.cancel_btn, LV_ALIGN_LEFT_MID, 50, 0);
    lv_obj_set_style_bg_color(wifi_state.cancel_btn, COLOR_BTN, LV_PART_MAIN);
    lv_obj_set_style_radius(wifi_state.cancel_btn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(wifi_state.cancel_btn, on_cancel_clicked, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t* cancel_label = lv_label_create(wifi_state.cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_font(cancel_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_center(cancel_label);
    
    // Connect button
    wifi_state.connect_btn = lv_btn_create(btn_container);
    lv_obj_set_size(wifi_state.connect_btn, 150, 50);
    lv_obj_align(wifi_state.connect_btn, LV_ALIGN_RIGHT_MID, -50, 0);
    lv_obj_set_style_bg_color(wifi_state.connect_btn, COLOR_SUCCESS, LV_PART_MAIN);
    lv_obj_set_style_radius(wifi_state.connect_btn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(wifi_state.connect_btn, on_connect_clicked, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t* connect_label = lv_label_create(wifi_state.connect_btn);
    lv_label_set_text(connect_label, "Connect");
    lv_obj_set_style_text_font(connect_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_center(connect_label);
    
    // Keyboard
    wifi_state.keyboard = lv_keyboard_create(wifi_state.password_dialog);
    lv_obj_set_size(wifi_state.keyboard, lv_pct(100), 280);
    lv_obj_align(wifi_state.keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(wifi_state.keyboard, wifi_state.password_textarea);
    lv_obj_add_event_cb(wifi_state.keyboard, on_keyboard_ready, LV_EVENT_READY, NULL);
}

static void show_password_dialog(const char* ssid, bool is_open)
{
    strncpy(wifi_state.selected_ssid, ssid, sizeof(wifi_state.selected_ssid) - 1);
    wifi_state.selected_is_open = is_open;
    
    char display_ssid[64];
    sanitize_ssid_for_display(display_ssid, ssid, sizeof(display_ssid));
    char ssid_text[96];
    snprintf(ssid_text, sizeof(ssid_text), "Network: %s", display_ssid);
    lv_label_set_text(wifi_state.password_ssid_label, ssid_text);
    
    lv_textarea_set_text(wifi_state.password_textarea, "");
    
    wifi_state.password_visible = false;
    lv_textarea_set_password_mode(wifi_state.password_textarea, true);
    lv_label_set_text(wifi_state.show_password_icon, LV_SYMBOL_EYE_CLOSE);
    
    if (is_open) {
        lv_obj_add_flag(wifi_state.password_textarea, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(wifi_state.show_password_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(wifi_state.keyboard, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(wifi_state.password_textarea, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(wifi_state.show_password_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(wifi_state.keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    
    lv_obj_clear_flag(wifi_state.password_dialog, LV_OBJ_FLAG_HIDDEN);
}

static void hide_password_dialog(void)
{
    if (wifi_state.password_dialog) {
        lv_obj_add_flag(wifi_state.password_dialog, LV_OBJ_FLAG_HIDDEN);
    }
}

// ============================================================================
// Event Handlers
// ============================================================================

static void on_back_clicked(lv_event_t* e)
{
    (void)e;
    wifi_screen_close();
}

static void on_disconnect_clicked(lv_event_t* e)
{
    (void)e;
    ESP_LOGI(TAG, "Disconnect clicked");
    
    if (wifi_state.config.on_disconnect) {
        wifi_state.config.on_disconnect();
    }
}

static void on_network_clicked(lv_event_t* e)
{
    uintptr_t index = (uintptr_t)lv_event_get_user_data(e);
    
    if (index < wifi_state.network_count) {
        const wifi_network_info_t* net = &wifi_state.networks[index];
        bool is_open = (net->authmode == 0);
        show_password_dialog(net->ssid, is_open);
    }
}

static void on_connect_clicked(lv_event_t* e)
{
    (void)e;
    
    const char* password = lv_textarea_get_text(wifi_state.password_textarea);
    
    if (wifi_state.config.on_connect) {
        wifi_state.config.on_connect(wifi_state.selected_ssid, 
                                      wifi_state.selected_is_open ? "" : password);
    }
    
    hide_password_dialog();
}

static void on_cancel_clicked(lv_event_t* e)
{
    (void)e;
    hide_password_dialog();
}

static void on_keyboard_ready(lv_event_t* e)
{
    (void)e;
    on_connect_clicked(NULL);
}

static void on_show_password_clicked(lv_event_t* e)
{
    (void)e;
    
    wifi_state.password_visible = !wifi_state.password_visible;
    lv_textarea_set_password_mode(wifi_state.password_textarea, !wifi_state.password_visible);
    
    if (wifi_state.password_visible) {
        lv_label_set_text(wifi_state.show_password_icon, LV_SYMBOL_EYE_OPEN);
    } else {
        lv_label_set_text(wifi_state.show_password_icon, LV_SYMBOL_EYE_CLOSE);
    }
}

static void scan_timer_cb(lv_timer_t* timer)
{
    (void)timer;
    
    if (wifi_state.visible) {
        ESP_LOGI(TAG, "Auto-scan triggered");
        trigger_scan();
    }
}

// ============================================================================
// Helper Functions
// ============================================================================

static const char* get_signal_icon(int8_t rssi)
{
    (void)rssi;
    return LV_SYMBOL_WIFI;
}
