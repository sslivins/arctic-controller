/*
 * Arctic Heat Pump Controller
 * Status Bar Implementation
 */
#include "status_bar.h"
#include "time_manager.h"
#include "settings/settings_time_screen.h"
#include "wifi_manager.h"
#include <esp_log.h>
#include <string.h>

static const char* TAG = "status_bar";

// Status bar height (doubled for better visibility)
#define STATUS_BAR_HEIGHT 80

// Colors
#define COLOR_BG        0x0d1117
#define COLOR_TEXT      0xc9d1d9
#define COLOR_WIFI_ON   0x00d4ff
#define COLOR_WIFI_OFF  0x666666
#define COLOR_NOTIFY    0xff6b35  // Orange for notification badge
#define COLOR_DROPDOWN_BG  0x161b22
#define COLOR_ITEM_HOVER   0x21262d

// Fonts
#define FONT_STATUS_BAR_ICON  (&lv_font_montserrat_32)  // Icons in status bar (WiFi, settings, bell)
#define FONT_STATUS_BAR_TIME  (&lv_font_montserrat_32)  // Time display
#define FONT_DROPDOWN_ICON    (&lv_font_montserrat_32)  // Icons in dropdown
#define FONT_DROPDOWN_TEXT    (&lv_font_montserrat_24)  // Text in dropdown

// Maximum notification message length
#define NOTIFY_MSG_MAX_LEN 64

// Notification item storage
typedef struct {
    bool active;
    char message[NOTIFY_MSG_MAX_LEN];
} notification_item_t;

// Internal state
static struct {
    lv_obj_t* container;
    lv_obj_t* time_btn;
    lv_obj_t* time_label;
    lv_obj_t* settings_btn;
    lv_obj_t* settings_icon;
    lv_obj_t* notify_btn;
    lv_obj_t* notify_icon;
    lv_obj_t* notify_badge;
    lv_obj_t* notify_dropdown;     // Dropdown panel
    lv_obj_t* wifi_btn;
    lv_obj_t* wifi_icon;
    lv_timer_t* update_timer;
    lv_timer_t* wifi_anim_timer;  // Timer for connecting animation
    status_bar_wifi_click_cb_t wifi_click_cb;
    status_bar_time_click_cb_t time_click_cb;
    status_bar_settings_click_cb_t settings_click_cb;
    status_bar_notify_item_cb_t notify_item_cb;
    notification_item_t notifications[STATUS_BAR_NOTIFY_MAX];
    bool wifi_connected;
    bool wifi_connecting;  // Connecting animation active
    uint8_t wifi_anim_step;  // Animation step counter
} bar_state = {};

// Forward declarations
static void wifi_btn_event_cb(lv_event_t* e);
static void time_btn_event_cb(lv_event_t* e);
static void settings_btn_event_cb(lv_event_t* e);
static void notify_btn_event_cb(lv_event_t* e);
static void timer_update_cb(lv_timer_t* timer);
static void wifi_anim_timer_cb(lv_timer_t* timer);
static void close_dropdown(void);
static void update_badge_display(void);
static uint8_t count_active_notifications(void);

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
    bar_state.notify_item_cb = config->on_notify_item_click;
    
    // Initialize notifications
    for (int i = 0; i < STATUS_BAR_NOTIFY_MAX; i++) {
        bar_state.notifications[i].active = false;
        bar_state.notifications[i].message[0] = '\0';
    }
    
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
    lv_obj_set_user_data(bar_state.time_btn, (void*)"time");
    
    // Time label inside button - large font for visibility
    bar_state.time_label = lv_label_create(bar_state.time_btn);
    lv_label_set_text(bar_state.time_label, "--:--");
    lv_obj_set_style_text_font(bar_state.time_label, FONT_STATUS_BAR_TIME, LV_PART_MAIN);
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
    lv_obj_set_user_data(bar_state.wifi_btn, (void*)"wifi");
    
    // WiFi icon inside button - large font for visibility
    bar_state.wifi_icon = lv_label_create(bar_state.wifi_btn);
    lv_label_set_text(bar_state.wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(bar_state.wifi_icon, FONT_STATUS_BAR_ICON, LV_PART_MAIN);
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
    lv_obj_set_user_data(bar_state.settings_btn, (void*)"settings");
    
    // Settings icon
    bar_state.settings_icon = lv_label_create(bar_state.settings_btn);
    lv_label_set_text(bar_state.settings_icon, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(bar_state.settings_icon, FONT_STATUS_BAR_ICON, LV_PART_MAIN);
    lv_obj_set_style_text_color(bar_state.settings_icon, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    lv_obj_center(bar_state.settings_icon);
    
    // Notification button (between settings and wifi)
    bar_state.notify_btn = lv_btn_create(bar_state.container);
    lv_obj_set_size(bar_state.notify_btn, 80, 70);
    lv_obj_align_to(bar_state.notify_btn, bar_state.settings_btn, LV_ALIGN_OUT_LEFT_MID, -5, 0);
    lv_obj_set_style_bg_opa(bar_state.notify_btn, LV_OPA_0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_state.notify_btn, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(bar_state.notify_btn, lv_color_hex(0xffffff), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(bar_state.notify_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_state.notify_btn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(bar_state.notify_btn, notify_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_user_data(bar_state.notify_btn, (void*)"notifications");
    
    // Notification bell icon
    bar_state.notify_icon = lv_label_create(bar_state.notify_btn);
    lv_label_set_text(bar_state.notify_icon, LV_SYMBOL_BELL);
    lv_obj_set_style_text_font(bar_state.notify_icon, FONT_STATUS_BAR_ICON, LV_PART_MAIN);
    lv_obj_set_style_text_color(bar_state.notify_icon, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    lv_obj_center(bar_state.notify_icon);
    
    // Notification badge (small red circle with count) - hidden by default
    bar_state.notify_badge = lv_obj_create(bar_state.notify_btn);
    lv_obj_set_size(bar_state.notify_badge, 20, 20);
    lv_obj_align(bar_state.notify_badge, LV_ALIGN_TOP_RIGHT, 5, 5);
    lv_obj_set_style_bg_color(bar_state.notify_badge, lv_color_hex(COLOR_NOTIFY), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_state.notify_badge, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_state.notify_badge, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar_state.notify_badge, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bar_state.notify_badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bar_state.notify_badge, LV_OBJ_FLAG_HIDDEN);
    
    // Dropdown starts as NULL (created on demand)
    bar_state.notify_dropdown = NULL;
    
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
    bar_state.wifi_connecting = false;  // Stop connecting animation
    
    // Stop animation timer if running
    if (bar_state.wifi_anim_timer) {
        lv_timer_del(bar_state.wifi_anim_timer);
        bar_state.wifi_anim_timer = NULL;
    }
    
    if (bar_state.wifi_icon) {
        // Reset opacity to full
        lv_obj_set_style_opa(bar_state.wifi_icon, LV_OPA_COVER, LV_PART_MAIN);
        
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
    close_dropdown();
    
    if (bar_state.wifi_anim_timer) {
        lv_timer_del(bar_state.wifi_anim_timer);
        bar_state.wifi_anim_timer = NULL;
    }
    
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
    bar_state.notify_btn = NULL;
    bar_state.notify_icon = NULL;
    bar_state.notify_badge = NULL;
    bar_state.notify_dropdown = NULL;
    bar_state.wifi_btn = NULL;
    bar_state.wifi_icon = NULL;
    bar_state.wifi_connecting = false;
    bar_state.wifi_anim_step = 0;
    
    // Clear notifications
    for (int i = 0; i < STATUS_BAR_NOTIFY_MAX; i++) {
        bar_state.notifications[i].active = false;
    }
}

static uint8_t count_active_notifications(void)
{
    uint8_t count = 0;
    for (int i = 0; i < STATUS_BAR_NOTIFY_MAX; i++) {
        if (bar_state.notifications[i].active) {
            count++;
        }
    }
    return count;
}

static void update_badge_display(void)
{
    if (!bar_state.notify_badge || !bar_state.notify_icon) {
        return;
    }
    
    uint8_t count = count_active_notifications();
    
    if (count > 0) {
        // Show badge and highlight bell
        lv_obj_remove_flag(bar_state.notify_badge, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(bar_state.notify_icon, 
                                    lv_color_hex(COLOR_NOTIFY), LV_PART_MAIN);
    } else {
        // Hide badge and reset bell color
        lv_obj_add_flag(bar_state.notify_badge, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(bar_state.notify_icon, 
                                    lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    }
}

void status_bar_add_notification(status_bar_notify_type_t type, const char* message)
{
    if (type >= STATUS_BAR_NOTIFY_MAX) {
        return;
    }
    
    bar_state.notifications[type].active = true;
    if (message) {
        strncpy(bar_state.notifications[type].message, message, NOTIFY_MSG_MAX_LEN - 1);
        bar_state.notifications[type].message[NOTIFY_MSG_MAX_LEN - 1] = '\0';
    } else {
        bar_state.notifications[type].message[0] = '\0';
    }
    
    update_badge_display();
    ESP_LOGI(TAG, "Added notification type %d: %s", type, message ? message : "(no message)");
}

void status_bar_clear_notification(status_bar_notify_type_t type)
{
    if (type >= STATUS_BAR_NOTIFY_MAX) {
        return;
    }
    
    bar_state.notifications[type].active = false;
    bar_state.notifications[type].message[0] = '\0';
    
    update_badge_display();
    ESP_LOGI(TAG, "Cleared notification type %d", type);
}

void status_bar_clear_all_notifications(void)
{
    for (int i = 0; i < STATUS_BAR_NOTIFY_MAX; i++) {
        bar_state.notifications[i].active = false;
        bar_state.notifications[i].message[0] = '\0';
    }
    
    update_badge_display();
    ESP_LOGI(TAG, "Cleared all notifications");
}

bool status_bar_has_notification(status_bar_notify_type_t type)
{
    if (type >= STATUS_BAR_NOTIFY_MAX) {
        return false;
    }
    return bar_state.notifications[type].active;
}

uint8_t status_bar_get_notify_count(void)
{
    return count_active_notifications();
}

void status_bar_set_wifi_connecting(bool connecting)
{
    if (connecting == bar_state.wifi_connecting) {
        return;  // No change
    }
    
    bar_state.wifi_connecting = connecting;
    bar_state.wifi_connected = false;
    
    if (connecting) {
        // Start pulsing animation - use cyan color
        bar_state.wifi_anim_step = 0;
        if (bar_state.wifi_icon) {
            lv_obj_set_style_text_color(bar_state.wifi_icon, 
                                        lv_color_hex(COLOR_WIFI_ON), LV_PART_MAIN);
        }
        
        // Create animation timer (updates every 100ms for smooth pulse)
        if (!bar_state.wifi_anim_timer) {
            bar_state.wifi_anim_timer = lv_timer_create(wifi_anim_timer_cb, 100, NULL);
        }
        ESP_LOGI(TAG, "WiFi connecting animation started");
    } else {
        // Stop animation
        if (bar_state.wifi_anim_timer) {
            lv_timer_del(bar_state.wifi_anim_timer);
            bar_state.wifi_anim_timer = NULL;
        }
        // Reset opacity
        if (bar_state.wifi_icon) {
            lv_obj_set_style_opa(bar_state.wifi_icon, LV_OPA_COVER, LV_PART_MAIN);
        }
    }
}

static void wifi_anim_timer_cb(lv_timer_t* timer)
{
    (void)timer;
    
    if (!bar_state.wifi_connecting || !bar_state.wifi_icon) {
        return;
    }
    
    // Smooth sine-wave pulse between 30% and 100% opacity
    // Using a lookup table for 20 steps (2 second cycle at 100ms interval)
    static const uint8_t opacity_table[20] = {
        255, 242, 207, 158, 107, 66, 38, 24, 24, 38,
        66, 107, 158, 207, 242, 255, 255, 255, 255, 255
    };
    
    bar_state.wifi_anim_step = (bar_state.wifi_anim_step + 1) % 20;
    lv_opa_t opa = opacity_table[bar_state.wifi_anim_step];
    lv_obj_set_style_opa(bar_state.wifi_icon, opa, LV_PART_MAIN);
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
    
    // Close dropdown if open
    close_dropdown();
    
    if (bar_state.settings_click_cb) {
        bar_state.settings_click_cb();
    }
}

// Event handler for dropdown overlay click (to close it)
static void dropdown_overlay_event_cb(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        close_dropdown();
    }
}

// Event handler for notification item click
static void notify_item_event_cb(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) {
        return;
    }
    
    status_bar_notify_type_t type = (status_bar_notify_type_t)(intptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "Notification item clicked: type=%d", type);
    
    close_dropdown();
    
    if (bar_state.notify_item_cb) {
        bar_state.notify_item_cb(type);
    }
}

static void close_dropdown(void)
{
    if (bar_state.notify_dropdown) {
        lv_obj_del(bar_state.notify_dropdown);
        bar_state.notify_dropdown = NULL;
    }
}

static void show_dropdown(void)
{
    // Close if already open (toggle behavior)
    if (bar_state.notify_dropdown) {
        close_dropdown();
        return;
    }
    
    uint8_t count = count_active_notifications();
    if (count == 0) {
        ESP_LOGI(TAG, "No notifications to show");
        return;
    }
    
    // Get screen (parent of status bar container)
    lv_obj_t* screen = lv_obj_get_parent(bar_state.container);
    if (!screen) {
        return;
    }
    
    // Create overlay container that fills the screen (for click-outside-to-close)
    bar_state.notify_dropdown = lv_obj_create(screen);
    lv_obj_set_size(bar_state.notify_dropdown, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(bar_state.notify_dropdown, 0, 0);
    lv_obj_set_style_bg_opa(bar_state.notify_dropdown, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_state.notify_dropdown, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_border_width(bar_state.notify_dropdown, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_state.notify_dropdown, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bar_state.notify_dropdown, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(bar_state.notify_dropdown, dropdown_overlay_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Create the dropdown panel inside the overlay
    lv_obj_t* panel = lv_obj_create(bar_state.notify_dropdown);
    lv_obj_set_size(panel, 500, count * 90 + 30);  // 90px per item + padding
    lv_obj_align(panel, LV_ALIGN_TOP_RIGHT, -25, STATUS_BAR_HEIGHT + 10);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COLOR_DROPDOWN_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x30363d), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(panel, 20, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(panel, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(panel, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    // Stop click events from propagating to overlay
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    
    // Add notification items
    for (int i = 0; i < STATUS_BAR_NOTIFY_MAX; i++) {
        if (!bar_state.notifications[i].active) {
            continue;
        }
        
        // Create item button
        lv_obj_t* item = lv_btn_create(panel);
        lv_obj_set_size(item, LV_PCT(100), 80);
        lv_obj_set_style_bg_color(item, lv_color_hex(COLOR_DROPDOWN_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(item, LV_OPA_0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(item, lv_color_hex(COLOR_ITEM_HOVER), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(item, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_radius(item, 8, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(item, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(item, notify_item_event_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        
        // Icon for the notification type
        lv_obj_t* icon = lv_label_create(item);
        const char* icon_text = LV_SYMBOL_BELL;
        switch (i) {
            case STATUS_BAR_NOTIFY_FIRMWARE_UPDATE:
                icon_text = LV_SYMBOL_DOWNLOAD;
                break;
            case STATUS_BAR_NOTIFY_WIFI_UNSTABLE:
                icon_text = LV_SYMBOL_WARNING;
                break;
            case STATUS_BAR_NOTIFY_LOW_BATTERY:
                icon_text = LV_SYMBOL_BATTERY_EMPTY;
                break;
            default:
                break;
        }
        lv_label_set_text(icon, icon_text);
        lv_obj_set_style_text_font(icon, FONT_DROPDOWN_ICON, LV_PART_MAIN);
        lv_obj_set_style_text_color(icon, lv_color_hex(COLOR_NOTIFY), LV_PART_MAIN);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 10, 0);
        
        // Message label
        lv_obj_t* label = lv_label_create(item);
        const char* msg = bar_state.notifications[i].message;
        if (msg[0] == '\0') {
            // Default messages if none provided
            switch (i) {
                case STATUS_BAR_NOTIFY_FIRMWARE_UPDATE:
                    msg = "Firmware update available";
                    break;
                case STATUS_BAR_NOTIFY_WIFI_UNSTABLE:
                    msg = "WiFi connection unstable";
                    break;
                case STATUS_BAR_NOTIFY_LOW_BATTERY:
                    msg = "Low battery warning";
                    break;
                default:
                    msg = "Notification";
                    break;
            }
        }
        lv_label_set_text(label, msg);
        lv_obj_set_style_text_font(label, FONT_DROPDOWN_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 60, 0);
    }
    
    ESP_LOGI(TAG, "Notification dropdown shown with %d items", count);
}

static void notify_btn_event_cb(lv_event_t* e)
{
    (void)e;
    ESP_LOGI(TAG, "Notification button clicked");
    
    show_dropdown();
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
