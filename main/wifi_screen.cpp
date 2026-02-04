/*
 * Arctic Heat Pump Controller
 * WiFi Settings Screen Implementation
 */
#include "wifi_screen.h"
#include <string.h>
#include <stdio.h>

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
    lv_obj_t* status_label;
    lv_obj_t* scan_btn;
    lv_obj_t* network_list;
    lv_obj_t* scanning_spinner;
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

// ============================================================================
// Forward Declarations
// ============================================================================

static void create_header(lv_obj_t* parent);
static void create_network_list(lv_obj_t* parent);
static void create_password_dialog(void);
static void show_password_dialog(const char* ssid, bool is_open);
static void hide_password_dialog(void);

static void on_back_clicked(lv_event_t* e);
static void on_scan_clicked(lv_event_t* e);
static void on_network_clicked(lv_event_t* e);
static void on_connect_clicked(lv_event_t* e);
static void on_cancel_clicked(lv_event_t* e);
static void on_keyboard_ready(lv_event_t* e);
static void on_show_password_clicked(lv_event_t* e);

static const char* get_signal_icon(int8_t rssi);

// ============================================================================
// Public API
// ============================================================================

void wifi_screen_create(const wifi_screen_config_t* config)
{
    if (wifi_state.visible) {
        return;  // Already visible
    }
    
    // Store config
    if (config) {
        wifi_state.config = *config;
    } else {
        memset(&wifi_state.config, 0, sizeof(wifi_state.config));
    }
    
    // Create screen
    wifi_state.screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(wifi_state.screen, COLOR_BG, LV_PART_MAIN);
    
    // Create header with back button and title
    create_header(wifi_state.screen);
    
    // Create main content area
    lv_obj_t* content = lv_obj_create(wifi_state.screen);
    lv_obj_set_size(content, 760, 520);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_style_bg_color(content, COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_border_color(content, COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(content, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(content, 15, LV_PART_MAIN);
    lv_obj_set_style_pad_all(content, 20, LV_PART_MAIN);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    
    // Connection status
    wifi_state.status_label = lv_label_create(content);
    lv_label_set_text(wifi_state.status_label, LV_SYMBOL_WIFI " Not Connected");
    lv_obj_set_style_text_font(wifi_state.status_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(wifi_state.status_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(wifi_state.status_label, LV_ALIGN_TOP_LEFT, 10, 10);
    
    // Scan button
    wifi_state.scan_btn = lv_btn_create(content);
    lv_obj_set_size(wifi_state.scan_btn, 150, 45);
    lv_obj_align(wifi_state.scan_btn, LV_ALIGN_TOP_RIGHT, -10, 5);
    lv_obj_set_style_bg_color(wifi_state.scan_btn, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_radius(wifi_state.scan_btn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(wifi_state.scan_btn, on_scan_clicked, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t* scan_label = lv_label_create(wifi_state.scan_btn);
    lv_label_set_text(scan_label, LV_SYMBOL_REFRESH " Scan");
    lv_obj_set_style_text_font(scan_label, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_center(scan_label);
    
    // Create network list area
    create_network_list(content);
    
    // Create password dialog (hidden initially)
    create_password_dialog();
    
    // Load the screen
    lv_scr_load(wifi_state.screen);
    wifi_state.visible = true;
    
    // Trigger initial scan if callback provided
    if (wifi_state.config.on_scan) {
        wifi_state.config.on_scan();
    }
}

void wifi_screen_close(void)
{
    if (!wifi_state.visible) {
        return;
    }
    
    // Hide password dialog if shown
    hide_password_dialog();
    
    // Notify close callback
    if (wifi_state.config.on_close) {
        wifi_state.config.on_close();
    }
    
    wifi_state.visible = false;
    
    // Note: The main app should load a different screen
    // We just clean up our state here
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
    
    // Store network data
    wifi_state.network_count = (count > 20) ? 20 : count;
    if (networks && wifi_state.network_count > 0) {
        memcpy(wifi_state.networks, networks, wifi_state.network_count * sizeof(wifi_network_info_t));
    }
    
    // Clear existing list items
    lv_obj_clean(wifi_state.network_list);
    
    if (wifi_state.network_count == 0) {
        // Show "No networks found" message
        lv_obj_t* empty_label = lv_label_create(wifi_state.network_list);
        lv_label_set_text(empty_label, "No networks found.\nTap 'Scan' to search for WiFi networks.");
        lv_obj_set_style_text_font(empty_label, &lv_font_montserrat_18, LV_PART_MAIN);
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
        lv_obj_set_size(row, lv_pct(100), 60);
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
        lv_obj_t* name = lv_label_create(row);
        lv_label_set_text(name, net->ssid);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(name, COLOR_TEXT, LV_PART_MAIN);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 45, 0);
        
        // Secured indicator for protected networks
        if (net->authmode > 0) {
            lv_obj_t* lock = lv_label_create(row);
            lv_label_set_text(lock, LV_SYMBOL_EYE_CLOSE);  // Use eye-close as "secured" indicator
            lv_obj_set_style_text_font(lock, &lv_font_montserrat_18, LV_PART_MAIN);
            lv_obj_set_style_text_color(lock, COLOR_TEXT_DIM, LV_PART_MAIN);
            lv_obj_align(lock, LV_ALIGN_RIGHT_MID, -10, 0);
        }
        
        // RSSI value
        char rssi_str[16];
        snprintf(rssi_str, sizeof(rssi_str), "%d dBm", net->rssi);
        lv_obj_t* rssi_label = lv_label_create(row);
        lv_label_set_text(rssi_label, rssi_str);
        lv_obj_set_style_text_font(rssi_label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(rssi_label, COLOR_TEXT_DIM, LV_PART_MAIN);
        lv_obj_align(rssi_label, LV_ALIGN_RIGHT_MID, (net->authmode > 0) ? -45 : -10, 0);
    }
}

void wifi_screen_set_scanning(bool scanning)
{
    if (!wifi_state.visible) {
        return;
    }
    
    if (scanning) {
        // Disable scan button
        lv_obj_add_state(wifi_state.scan_btn, LV_STATE_DISABLED);
        
        // Show spinner if not already created
        if (!wifi_state.scanning_spinner) {
            wifi_state.scanning_spinner = lv_spinner_create(wifi_state.network_list);
            lv_obj_set_size(wifi_state.scanning_spinner, 50, 50);
            lv_obj_center(wifi_state.scanning_spinner);
            
            wifi_state.scanning_label = lv_label_create(wifi_state.network_list);
            lv_label_set_text(wifi_state.scanning_label, "Scanning...");
            lv_obj_set_style_text_font(wifi_state.scanning_label, &lv_font_montserrat_18, LV_PART_MAIN);
            lv_obj_set_style_text_color(wifi_state.scanning_label, COLOR_TEXT_DIM, LV_PART_MAIN);
            lv_obj_align_to(wifi_state.scanning_label, wifi_state.scanning_spinner, LV_ALIGN_OUT_BOTTOM_MID, 0, 15);
        }
        
        // Clear network list and show spinner
        lv_obj_clean(wifi_state.network_list);
        
        wifi_state.scanning_spinner = lv_spinner_create(wifi_state.network_list);
        lv_obj_set_size(wifi_state.scanning_spinner, 50, 50);
        lv_obj_center(wifi_state.scanning_spinner);
        
        wifi_state.scanning_label = lv_label_create(wifi_state.network_list);
        lv_label_set_text(wifi_state.scanning_label, "Scanning for networks...");
        lv_obj_set_style_text_font(wifi_state.scanning_label, &lv_font_montserrat_18, LV_PART_MAIN);
        lv_obj_set_style_text_color(wifi_state.scanning_label, COLOR_TEXT_DIM, LV_PART_MAIN);
        lv_obj_align_to(wifi_state.scanning_label, wifi_state.scanning_spinner, LV_ALIGN_OUT_BOTTOM_MID, 0, 15);
    } else {
        // Re-enable scan button
        lv_obj_clear_state(wifi_state.scan_btn, LV_STATE_DISABLED);
        
        // Spinner will be removed when network list updates
    }
}

void wifi_screen_set_connection_status(bool connected, const char* ssid)
{
    if (!wifi_state.visible || !wifi_state.status_label) {
        return;
    }
    
    char status_text[64];
    if (connected && ssid) {
        snprintf(status_text, sizeof(status_text), LV_SYMBOL_WIFI " Connected: %s", ssid);
        lv_obj_set_style_text_color(wifi_state.status_label, COLOR_SUCCESS, LV_PART_MAIN);
    } else {
        snprintf(status_text, sizeof(status_text), LV_SYMBOL_WIFI " Not Connected");
        lv_obj_set_style_text_color(wifi_state.status_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    }
    lv_label_set_text(wifi_state.status_label, status_text);
}

void wifi_screen_show_error(const char* message)
{
    if (!wifi_state.visible) {
        return;
    }
    
    // Create a message box
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
    // Header container
    wifi_state.header = lv_obj_create(parent);
    lv_obj_set_size(wifi_state.header, lv_pct(100), 70);
    lv_obj_align(wifi_state.header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(wifi_state.header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(wifi_state.header, 0, LV_PART_MAIN);
    lv_obj_clear_flag(wifi_state.header, LV_OBJ_FLAG_SCROLLABLE);
    
    // Back button
    wifi_state.back_btn = lv_btn_create(wifi_state.header);
    lv_obj_set_size(wifi_state.back_btn, 100, 50);
    lv_obj_align(wifi_state.back_btn, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_set_style_bg_color(wifi_state.back_btn, COLOR_BTN, LV_PART_MAIN);
    lv_obj_set_style_bg_color(wifi_state.back_btn, COLOR_BTN_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_radius(wifi_state.back_btn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(wifi_state.back_btn, on_back_clicked, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t* back_label = lv_label_create(wifi_state.back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_center(back_label);
    
    // Title
    lv_obj_t* title = lv_label_create(wifi_state.header);
    lv_label_set_text(title, LV_SYMBOL_WIFI " WiFi Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_center(title);
}

static void create_network_list(lv_obj_t* parent)
{
    // Network list container (scrollable)
    wifi_state.network_list = lv_obj_create(parent);
    lv_obj_set_size(wifi_state.network_list, lv_pct(100), 400);
    lv_obj_align(wifi_state.network_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(wifi_state.network_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(wifi_state.network_list, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(wifi_state.network_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_flex_main_place(wifi_state.network_list, LV_FLEX_ALIGN_START, LV_PART_MAIN);
    lv_obj_set_style_pad_row(wifi_state.network_list, 10, LV_PART_MAIN);
    
    // Initial empty state
    lv_obj_t* empty_label = lv_label_create(wifi_state.network_list);
    lv_label_set_text(empty_label, "Tap 'Scan' to search for WiFi networks");
    lv_obj_set_style_text_font(empty_label, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(empty_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_center(empty_label);
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
    lv_label_set_text(dialog_title, "Connect to Network");
    lv_obj_set_style_text_font(dialog_title, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(dialog_title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(dialog_title, LV_ALIGN_TOP_MID, 0, 0);
    
    // Selected network name
    wifi_state.password_ssid_label = lv_label_create(dialog_box);
    lv_label_set_text(wifi_state.password_ssid_label, "Network: ");
    lv_obj_set_style_text_font(wifi_state.password_ssid_label, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(wifi_state.password_ssid_label, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(wifi_state.password_ssid_label, LV_ALIGN_TOP_MID, 0, 40);
    
    // Password input container (textarea + eye button)
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
    lv_textarea_set_placeholder_text(wifi_state.password_textarea, "Enter password...");
    lv_textarea_set_password_mode(wifi_state.password_textarea, true);
    lv_textarea_set_password_bullet(wifi_state.password_textarea, "•");  // Explicit bullet character
    lv_textarea_set_one_line(wifi_state.password_textarea, true);
    lv_obj_set_style_text_font(wifi_state.password_textarea, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(wifi_state.password_textarea, COLOR_TEXT, LV_PART_MAIN);  // White text
    lv_obj_set_style_bg_color(wifi_state.password_textarea, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_border_color(wifi_state.password_textarea, COLOR_ACCENT, LV_PART_MAIN);  // Cyan border for visibility
    lv_obj_set_style_border_width(wifi_state.password_textarea, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(wifi_state.password_textarea, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_left(wifi_state.password_textarea, 15, LV_PART_MAIN);  // Padding so text isn't against edge
    
    // Show/hide password button (eye icon)
    wifi_state.password_visible = false;
    wifi_state.show_password_btn = lv_btn_create(password_container);
    lv_obj_set_size(wifi_state.show_password_btn, 50, 50);
    lv_obj_align(wifi_state.show_password_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(wifi_state.show_password_btn, COLOR_BTN, LV_PART_MAIN);
    lv_obj_set_style_bg_color(wifi_state.show_password_btn, COLOR_BTN_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_radius(wifi_state.show_password_btn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(wifi_state.show_password_btn, on_show_password_clicked, LV_EVENT_CLICKED, NULL);
    
    wifi_state.show_password_icon = lv_label_create(wifi_state.show_password_btn);
    lv_label_set_text(wifi_state.show_password_icon, LV_SYMBOL_EYE_CLOSE);  // Eye closed = password hidden
    lv_obj_set_style_text_font(wifi_state.show_password_icon, &lv_font_montserrat_20, LV_PART_MAIN);
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
    lv_obj_set_style_text_font(cancel_label, &lv_font_montserrat_18, LV_PART_MAIN);
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
    lv_obj_set_style_text_font(connect_label, &lv_font_montserrat_18, LV_PART_MAIN);
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
    
    // Update SSID label
    char ssid_text[64];
    snprintf(ssid_text, sizeof(ssid_text), "Network: %s", ssid);
    lv_label_set_text(wifi_state.password_ssid_label, ssid_text);
    
    // Clear password field
    lv_textarea_set_text(wifi_state.password_textarea, "");
    
    // Reset password visibility to hidden
    wifi_state.password_visible = false;
    lv_textarea_set_password_mode(wifi_state.password_textarea, true);
    lv_label_set_text(wifi_state.show_password_icon, LV_SYMBOL_EYE_CLOSE);
    
    // Hide/show password field based on whether network is open
    if (is_open) {
        lv_obj_add_flag(wifi_state.password_textarea, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(wifi_state.show_password_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(wifi_state.keyboard, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(wifi_state.password_textarea, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(wifi_state.show_password_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(wifi_state.keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    
    // Show dialog
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

static void on_scan_clicked(lv_event_t* e)
{
    (void)e;
    if (wifi_state.config.on_scan) {
        wifi_screen_set_scanning(true);
        wifi_state.config.on_scan();
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
    // User pressed Enter on keyboard - trigger connect
    on_connect_clicked(NULL);
}

static void on_show_password_clicked(lv_event_t* e)
{
    (void)e;
    
    // Toggle password visibility
    wifi_state.password_visible = !wifi_state.password_visible;
    
    // Update textarea password mode
    lv_textarea_set_password_mode(wifi_state.password_textarea, !wifi_state.password_visible);
    
    // Update eye icon
    if (wifi_state.password_visible) {
        lv_label_set_text(wifi_state.show_password_icon, LV_SYMBOL_EYE_OPEN);  // Eye open = password visible
    } else {
        lv_label_set_text(wifi_state.show_password_icon, LV_SYMBOL_EYE_CLOSE);  // Eye closed = password hidden
    }
}

// ============================================================================
// Helper Functions
// ============================================================================

static const char* get_signal_icon(int8_t rssi)
{
    // Map RSSI to signal bars
    // Excellent: > -50 dBm
    // Good: -50 to -60 dBm
    // Fair: -60 to -70 dBm
    // Weak: < -70 dBm
    
    if (rssi > -50) {
        return LV_SYMBOL_WIFI;  // Full signal
    } else if (rssi > -60) {
        return LV_SYMBOL_WIFI;  // Good signal
    } else if (rssi > -70) {
        return LV_SYMBOL_WIFI;  // Fair signal
    } else {
        return LV_SYMBOL_WIFI;  // Weak signal (LVGL only has one wifi icon)
    }
}
