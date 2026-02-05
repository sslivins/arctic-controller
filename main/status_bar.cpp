/*
 * Arctic Heat Pump Controller
 * Status Bar Implementation
 */
#include "status_bar.h"
#include "time_manager.h"
#include "time_screen.h"
#include "wifi_manager.h"
#include <esp_log.h>

static const char* TAG = "status_bar";

// Status bar height (doubled for better visibility)
#define STATUS_BAR_HEIGHT 80

// Colors
#define COLOR_BG        0x0d1117
#define COLOR_TEXT      0xc9d1d9
#define COLOR_WIFI_ON   0x00d4ff
#define COLOR_WIFI_OFF  0x666666

// Internal state
static struct {
    lv_obj_t* container;
    lv_obj_t* time_btn;
    lv_obj_t* time_label;
    lv_obj_t* settings_btn;
    lv_obj_t* settings_icon;
    lv_obj_t* wifi_btn;
    lv_obj_t* wifi_icon;
    lv_timer_t* update_timer;
    status_bar_wifi_click_cb_t wifi_click_cb;
    status_bar_time_click_cb_t time_click_cb;
    status_bar_settings_click_cb_t settings_click_cb;
    bool wifi_connected;
} bar_state = {};

// Forward declarations
static void wifi_btn_event_cb(lv_event_t* e);
static void time_btn_event_cb(lv_event_t* e);
static void settings_btn_event_cb(lv_event_t* e);
static void timer_update_cb(lv_timer_t* timer);

lv_obj_t* status_bar_create(const status_bar_config_t* config)
{
    if (!config || !config->parent) {
        ESP_LOGE(TAG, "Invalid config");
        return NULL;
    }
    
    // Store callbacks
    bar_state.wifi_click_cb = config->on_wifi_click;
    bar_state.time_click_cb = config->on_time_click;
    bar_state.settings_click_cb = config->on_settings_click;
    
    // Create container
    bar_state.container = lv_obj_create(config->parent);
    lv_obj_set_size(bar_state.container, LV_PCT(100), STATUS_BAR_HEIGHT);
    lv_obj_align(bar_state.container, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar_state.container, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_state.container, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar_state.container, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_state.container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(bar_state.container, 25, LV_PART_MAIN);
    lv_obj_set_style_pad_right(bar_state.container, 25, LV_PART_MAIN);
    lv_obj_clear_flag(bar_state.container, LV_OBJ_FLAG_SCROLLABLE);
    
    // Time button (left side) - clickable with touch feedback
    bar_state.time_btn = lv_btn_create(bar_state.container);
    lv_obj_set_size(bar_state.time_btn, 220, 70);  // Wider for 12h format like "12:30 PM"
    lv_obj_align(bar_state.time_btn, LV_ALIGN_LEFT_MID, -10, 0);
    lv_obj_set_style_bg_opa(bar_state.time_btn, LV_OPA_0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_state.time_btn, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(bar_state.time_btn, lv_color_hex(0xffffff), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(bar_state.time_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_state.time_btn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(bar_state.time_btn, time_btn_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Time label inside button - large font for visibility
    bar_state.time_label = lv_label_create(bar_state.time_btn);
    lv_label_set_text(bar_state.time_label, "--:--");
    lv_obj_set_style_text_font(bar_state.time_label, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_set_style_text_color(bar_state.time_label, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    lv_obj_align(bar_state.time_label, LV_ALIGN_LEFT_MID, 10, 0);
    
    // WiFi button (right side) - larger touch target
    bar_state.wifi_btn = lv_btn_create(bar_state.container);
    lv_obj_set_size(bar_state.wifi_btn, 100, 70);
    lv_obj_align(bar_state.wifi_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_opa(bar_state.wifi_btn, LV_OPA_0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_state.wifi_btn, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(bar_state.wifi_btn, lv_color_hex(0xffffff), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(bar_state.wifi_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_state.wifi_btn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(bar_state.wifi_btn, wifi_btn_event_cb, LV_EVENT_CLICKED, NULL);
    
    // WiFi icon inside button - large font for visibility
    bar_state.wifi_icon = lv_label_create(bar_state.wifi_btn);
    lv_label_set_text(bar_state.wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(bar_state.wifi_icon, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_set_style_text_color(bar_state.wifi_icon, lv_color_hex(COLOR_WIFI_OFF), LV_PART_MAIN);
    lv_obj_center(bar_state.wifi_icon);
    
    // Settings button (between time and wifi)
    bar_state.settings_btn = lv_btn_create(bar_state.container);
    lv_obj_set_size(bar_state.settings_btn, 80, 70);
    lv_obj_align_to(bar_state.settings_btn, bar_state.wifi_btn, LV_ALIGN_OUT_LEFT_MID, -10, 0);
    lv_obj_set_style_bg_opa(bar_state.settings_btn, LV_OPA_0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_state.settings_btn, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(bar_state.settings_btn, lv_color_hex(0xffffff), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(bar_state.settings_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_state.settings_btn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(bar_state.settings_btn, settings_btn_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Settings icon
    bar_state.settings_icon = lv_label_create(bar_state.settings_btn);
    lv_label_set_text(bar_state.settings_icon, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(bar_state.settings_icon, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(bar_state.settings_icon, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    lv_obj_center(bar_state.settings_icon);
    
    // Update time immediately
    status_bar_update_time();
    
    // Update WiFi state based on current connection
    status_bar_set_wifi_state(
        wifi_mgr_get_state() == WIFI_MGR_STATE_CONNECTED,
        wifi_mgr_get_connected_ssid()
    );
    
    // Create timer for periodic updates (every 10 seconds)
    bar_state.update_timer = lv_timer_create(timer_update_cb, 10000, NULL);
    
    ESP_LOGI(TAG, "Status bar created");
    return bar_state.container;
}

void status_bar_set_wifi_state(bool connected, const char* ssid)
{
    bar_state.wifi_connected = connected;
    
    if (bar_state.wifi_icon) {
        if (connected) {
            lv_obj_set_style_text_color(bar_state.wifi_icon, 
                                        lv_color_hex(COLOR_WIFI_ON), LV_PART_MAIN);
        } else {
            lv_obj_set_style_text_color(bar_state.wifi_icon, 
                                        lv_color_hex(COLOR_WIFI_OFF), LV_PART_MAIN);
        }
    }
    
    (void)ssid;  // Could show as tooltip in future
}

void status_bar_update_time(void)
{
    if (!bar_state.time_label) {
        return;
    }
    
    char time_str[16];
    // Use 12h or 24h format based on user setting
    const char* format = time_screen_get_24h_format() ? "%H:%M" : "%I:%M %p";
    
    // Get current time
    time_t now;
    time(&now);
    
    // Check if time appears valid (after Jan 1, 2020)
    // RTC maintains time while battery has charge, so this should usually be valid
    if (now >= 1577836800) {
        // Time is valid, show it
        if (time_mgr_get_time_str(time_str, sizeof(time_str), format)) {
            lv_label_set_text(bar_state.time_label, time_str);
        }
    } else {
        // Time is invalid (epoch/1970) - battery was drained, wait for NTP
        lv_label_set_text(bar_state.time_label, "--:--");
    }
}

void status_bar_delete(void)
{
    if (bar_state.update_timer) {
        lv_timer_del(bar_state.update_timer);
        bar_state.update_timer = NULL;
    }
    
    if (bar_state.container) {
        lv_obj_del(bar_state.container);
        bar_state.container = NULL;
    }
    
    bar_state.time_btn = NULL;
    bar_state.time_label = NULL;
    bar_state.settings_btn = NULL;
    bar_state.settings_icon = NULL;
    bar_state.wifi_btn = NULL;
    bar_state.wifi_icon = NULL;
}

static void wifi_btn_event_cb(lv_event_t* e)
{
    (void)e;
    ESP_LOGI(TAG, "WiFi button clicked");
    
    if (bar_state.wifi_click_cb) {
        bar_state.wifi_click_cb();
    }
}

static void time_btn_event_cb(lv_event_t* e)
{
    (void)e;
    ESP_LOGI(TAG, "Time button clicked");
    
    if (bar_state.time_click_cb) {
        bar_state.time_click_cb();
    }
}

static void settings_btn_event_cb(lv_event_t* e)
{
    (void)e;
    ESP_LOGI(TAG, "Settings button clicked");
    
    if (bar_state.settings_click_cb) {
        bar_state.settings_click_cb();
    }
}

static void timer_update_cb(lv_timer_t* timer)
{
    (void)timer;
    status_bar_update_time();
    
    // Also update WiFi state
    status_bar_set_wifi_state(
        wifi_mgr_get_state() == WIFI_MGR_STATE_CONNECTED,
        wifi_mgr_get_connected_ssid()
    );
}
