/*
 * Arctic Heat Pump Controller
 * Settings - Time Screen Implementation (iOS-style full screen)
 * 
 * Full-screen time/timezone configuration with back navigation.
 * Portrait mode: 720x1280
 */
#include "settings_time_screen.h"
#include "settings_menu.h"
#include "settings_common.h"
#include "time_manager.h"
#include "i18n/i18n.h"
#include "fonts/fonts.h"
#include <esp_log.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <string.h>
#include <stdio.h>

static const char* TAG = "time_screen";

// NVS namespace and keys
#define NVS_NAMESPACE    "time_cfg"
#define NVS_KEY_24H      "use_24h"

// Common timezone definitions
typedef struct {
    const char* name;
    const char* tz_string;
} timezone_entry_t;

static const timezone_entry_t timezones[] = {
    // North America
    {"US Eastern (EST/EDT)",      "EST5EDT,M3.2.0,M11.1.0"},
    {"US Central (CST/CDT)",      "CST6CDT,M3.2.0,M11.1.0"},
    {"US Mountain (MST/MDT)",     "MST7MDT,M3.2.0,M11.1.0"},
    {"US Pacific (PST/PDT)",      "PST8PDT,M3.2.0,M11.1.0"},
    {"US Alaska",                  "AKST9AKDT,M3.2.0,M11.1.0"},
    {"US Hawaii",                  "HST10"},
    {"US Arizona (no DST)",        "MST7"},
    
    // Canada
    {"Canada Atlantic",            "AST4ADT,M3.2.0,M11.1.0"},
    {"Canada Newfoundland",        "NST3:30NDT,M3.2.0,M11.1.0"},
    
    // Europe
    {"UK / London (GMT/BST)",      "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Central Europe (CET/CEST)",  "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Eastern Europe (EET/EEST)",  "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    
    // Asia
    {"Japan (JST)",                "JST-9"},
    {"China (CST)",                "CST-8"},
    {"India (IST)",                "IST-5:30"},
    {"Korea (KST)",                "KST-9"},
    {"Singapore/HK",               "SGT-8"},
    
    // Australia
    {"Australia Eastern (AEST)",   "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia Central",          "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Australia Western",          "AWST-8"},
    
    // Other
    {"UTC (No offset)",            "UTC0"},
};

#define NUM_TIMEZONES (sizeof(timezones) / sizeof(timezones[0]))

// ============================================================================
// Screen State
// ============================================================================

typedef struct {
    bool visible;
    time_screen_config_t config;
    
    // Screen objects
    lv_obj_t* screen;
    lv_obj_t* header;
    lv_obj_t* back_btn;
    lv_obj_t* content;
    
    // Time display
    lv_obj_t* preview_label;
    lv_obj_t* sync_status_label;
    lv_obj_t* format_switch;
    lv_obj_t* tz_roller;
    lv_obj_t* format_12h_label;
    lv_obj_t* format_24h_label;
    
    // State
    bool use_24h;
    int current_tz_index;
    lv_timer_t* preview_timer;
    
} time_screen_state_t;

static time_screen_state_t s_state = {};

// ============================================================================
// Forward Declarations
// ============================================================================

static void create_header(void);
static void create_content(void);
static void back_btn_cb(lv_event_t* e);
static void format_switch_cb(lv_event_t* e);
static void tz_roller_cb(lv_event_t* e);
static void preview_timer_cb(lv_timer_t* timer);
static void load_settings(void);
static void save_settings(void);
static void update_preview(void);
static void update_sync_status(void);
static int find_current_tz_index(void);

// ============================================================================
// Settings Persistence
// ============================================================================

static void load_settings(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        uint8_t val = 1;  // Default to 24h
        nvs_get_u8(nvs, NVS_KEY_24H, &val);
        s_state.use_24h = (val != 0);
        nvs_close(nvs);
    } else {
        s_state.use_24h = true;
    }
    
    s_state.current_tz_index = find_current_tz_index();
    ESP_LOGI(TAG, "Loaded settings: 24h=%d, tz_idx=%d", s_state.use_24h, s_state.current_tz_index);
}

static void save_settings(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        nvs_set_u8(nvs, NVS_KEY_24H, s_state.use_24h ? 1 : 0);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Saved 24h format: %d", s_state.use_24h);
    }
}

static int find_current_tz_index(void)
{
    const char* current_tz = time_mgr_get_timezone();
    for (int i = 0; i < (int)NUM_TIMEZONES; i++) {
        if (strcmp(timezones[i].tz_string, current_tz) == 0) {
            return i;
        }
    }
    return 0;
}

// ============================================================================
// UI Updates
// ============================================================================

static void update_preview(void)
{
    if (!s_state.preview_label) return;
    
    char time_str[64];
    const char* format = s_state.use_24h ? "%H:%M:%S" : "%I:%M:%S %p";
    
    if (time_mgr_is_synced() && time_mgr_get_time_str(time_str, sizeof(time_str), format)) {
        lv_label_set_text(s_state.preview_label, time_str);
    } else {
        lv_label_set_text(s_state.preview_label, s_state.use_24h ? "--:--:--" : "--:--:-- --");
    }
}

static void update_sync_status(void)
{
    if (!s_state.sync_status_label) return;
    
    if (time_mgr_is_synced()) {
        char buf[48];
        snprintf(buf, sizeof(buf), LV_SYMBOL_OK " %s", i18n_get(STR_TIME_SYNCED));
        lv_label_set_text(s_state.sync_status_label, buf);
        lv_obj_set_style_text_color(s_state.sync_status_label, COLOR_SUCCESS, LV_PART_MAIN);
    } else {
        char buf[48];
        snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING " %s", i18n_get(STR_TIME_NOT_SYNCED));
        lv_label_set_text(s_state.sync_status_label, buf);
        lv_obj_set_style_text_color(s_state.sync_status_label, COLOR_WARNING, LV_PART_MAIN);
    }
}

// ============================================================================
// Public API
// ============================================================================

void time_screen_create(const time_screen_config_t* config)
{
    if (s_state.visible) {
        ESP_LOGW(TAG, "Time screen already visible");
        return;
    }
    
    ESP_LOGI(TAG, "Creating Time screen");
    
    memset(&s_state, 0, sizeof(s_state));
    if (config) {
        s_state.config = *config;
    }
    
    load_settings();
    
    // Create screen
    s_state.screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_state.screen, COLOR_BG, LV_PART_MAIN);
    disable_scrolling(s_state.screen);
    
    create_header();
    create_content();
    
    // Start preview timer
    s_state.preview_timer = lv_timer_create(preview_timer_cb, 1000, NULL);
    
    // Load screen with slide-left animation
    lv_screen_load_anim(s_state.screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
    s_state.visible = true;
}

void time_screen_close(void)
{
    if (!s_state.visible) return;
    
    ESP_LOGI(TAG, "Closing Time screen");
    
    if (s_state.preview_timer) {
        lv_timer_delete(s_state.preview_timer);
        s_state.preview_timer = NULL;
    }
    
    s_state.visible = false;
    s_state.screen = NULL;
}

bool time_screen_is_visible(void)
{
    return s_state.visible;
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
    
    // Back button
    s_state.back_btn = lv_btn_create(s_state.header);
    lv_obj_set_size(s_state.back_btn, 50, 40);
    lv_obj_align(s_state.back_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_state.back_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_state.back_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(s_state.back_btn, back_btn_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t* back_icon = lv_label_create(s_state.back_btn);
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_icon, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(back_icon, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_center(back_icon);
    
    // Title
    lv_obj_t* title = lv_label_create(s_state.header);
    lv_label_set_text(title, i18n_get(STR_SETTINGS_TIME));
    lv_obj_set_style_text_font(title, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
}

static void create_content(void)
{
    lv_display_t* disp = lv_display_get_default();
    int32_t screen_height = lv_display_get_vertical_resolution(disp);
    int32_t header_height = screen_height * HEADER_HEIGHT_PCT / 100;
    
    s_state.content = lv_obj_create(s_state.screen);
    lv_obj_set_size(s_state.content, LV_PCT(100), screen_height - header_height);
    lv_obj_set_pos(s_state.content, 0, header_height);
    lv_obj_set_style_bg_opa(s_state.content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_state.content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_state.content, 20, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_state.content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_state.content, 15, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(s_state.content, LV_SCROLLBAR_MODE_AUTO);
    
    // ===== CURRENT TIME ROW =====
    lv_obj_t* time_row = lv_obj_create(s_state.content);
    lv_obj_set_size(time_row, LV_PCT(100), 80);
    lv_obj_set_style_bg_color(time_row, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_width(time_row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(time_row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(time_row, 15, LV_PART_MAIN);
    disable_scrolling(time_row);
    
    // Current time preview (left)
    s_state.preview_label = lv_label_create(time_row);
    lv_obj_set_style_text_font(s_state.preview_label, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_state.preview_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(s_state.preview_label, LV_ALIGN_LEFT_MID, 10, 0);
    
    // Sync status (right)
    s_state.sync_status_label = lv_label_create(time_row);
    lv_obj_set_style_text_font(s_state.sync_status_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_align(s_state.sync_status_label, LV_ALIGN_RIGHT_MID, -10, 0);
    
    update_preview();
    update_sync_status();
    
    // ===== TIME FORMAT CARD =====
    lv_obj_t* format_card = lv_obj_create(s_state.content);
    lv_obj_set_size(format_card, LV_PCT(100), 140);
    lv_obj_set_style_bg_color(format_card, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_width(format_card, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(format_card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(format_card, 20, LV_PART_MAIN);
    disable_scrolling(format_card);
    
    // Format section title
    lv_obj_t* format_title = lv_label_create(format_card);
    lv_label_set_text(format_title, i18n_get(STR_TIME_DISPLAY_FORMAT));
    lv_obj_set_style_text_font(format_title, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(format_title, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(format_title, LV_ALIGN_TOP_LEFT, 0, 0);
    
    // 12h / 24h toggle row
    lv_obj_t* toggle_row = lv_obj_create(format_card);
    lv_obj_set_size(toggle_row, LV_PCT(100), 60);
    lv_obj_align(toggle_row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(toggle_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(toggle_row, 0, LV_PART_MAIN);
    disable_scrolling(toggle_row);
    
    // 12h label
    s_state.format_12h_label = lv_label_create(toggle_row);
    lv_label_set_text(s_state.format_12h_label, i18n_get(STR_TIME_FORMAT_12H));
    lv_obj_set_style_text_font(s_state.format_12h_label, FONT_LARGE, LV_PART_MAIN);
    lv_obj_align(s_state.format_12h_label, LV_ALIGN_LEFT_MID, 10, 0);
    
    // Switch
    s_state.format_switch = lv_switch_create(toggle_row);
    lv_obj_set_size(s_state.format_switch, 80, 45);
    lv_obj_align(s_state.format_switch, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_state.format_switch, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_state.format_switch, COLOR_ACCENT, LV_PART_INDICATOR | (lv_style_selector_t)LV_STATE_CHECKED);
    if (s_state.use_24h) {
        lv_obj_add_state(s_state.format_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(s_state.format_switch, format_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
    
    // 24h label
    s_state.format_24h_label = lv_label_create(toggle_row);
    lv_label_set_text(s_state.format_24h_label, i18n_get(STR_TIME_FORMAT_24H));
    lv_obj_set_style_text_font(s_state.format_24h_label, FONT_LARGE, LV_PART_MAIN);
    lv_obj_align(s_state.format_24h_label, LV_ALIGN_RIGHT_MID, -10, 0);
    
    // Set initial label colors
    if (s_state.use_24h) {
        lv_obj_set_style_text_color(s_state.format_12h_label, COLOR_TEXT_DIM, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_state.format_24h_label, COLOR_TEXT, LV_PART_MAIN);
    } else {
        lv_obj_set_style_text_color(s_state.format_12h_label, COLOR_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_state.format_24h_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    }
    
    // ===== TIMEZONE CARD =====
    lv_obj_t* tz_card = lv_obj_create(s_state.content);
    lv_obj_set_size(tz_card, LV_PCT(100), 350);
    lv_obj_set_style_bg_color(tz_card, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_width(tz_card, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(tz_card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tz_card, 20, LV_PART_MAIN);
    disable_scrolling(tz_card);
    
    // Timezone section title
    lv_obj_t* tz_title = lv_label_create(tz_card);
    lv_label_set_text(tz_title, i18n_get(STR_TIME_TIMEZONE));
    lv_obj_set_style_text_font(tz_title, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(tz_title, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(tz_title, LV_ALIGN_TOP_LEFT, 0, 0);
    
    // Build roller options
    static char tz_options[2048] = {0};
    tz_options[0] = '\0';
    for (size_t i = 0; i < NUM_TIMEZONES; i++) {
        if (i > 0) strcat(tz_options, "\n");
        strcat(tz_options, timezones[i].name);
    }
    
    // Timezone roller
    s_state.tz_roller = lv_roller_create(tz_card);
    lv_roller_set_options(s_state.tz_roller, tz_options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_selected(s_state.tz_roller, s_state.current_tz_index, LV_ANIM_OFF);
    lv_roller_set_visible_row_count(s_state.tz_roller, 7);
    lv_obj_set_size(s_state.tz_roller, LV_PCT(100), 280);
    lv_obj_align(s_state.tz_roller, LV_ALIGN_BOTTOM_MID, 0, 0);
    
    // Style the roller
    lv_obj_set_style_bg_color(s_state.tz_roller, lv_color_hex(0x1a1f26), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_state.tz_roller, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_state.tz_roller, lv_color_hex(0x30363d), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_state.tz_roller, 1, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_state.tz_roller, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_radius(s_state.tz_roller, 8, LV_PART_MAIN);
    lv_obj_set_style_line_width(s_state.tz_roller, 0, LV_PART_MAIN);
    
    // Style the selected item
    lv_obj_set_style_bg_color(s_state.tz_roller, COLOR_ACCENT, LV_PART_SELECTED);
    lv_obj_set_style_text_color(s_state.tz_roller, lv_color_hex(0x0d1117), LV_PART_SELECTED);
    
    lv_obj_add_event_cb(s_state.tz_roller, tz_roller_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

// ============================================================================
// Event Handlers
// ============================================================================

static void back_btn_cb(lv_event_t* e)
{
    (void)e;
    ESP_LOGI(TAG, "Back button clicked");
    
    void (*on_back)(void) = s_state.config.on_back;
    
    if (on_back) {
        on_back();
    } else {
        settings_menu_show();
    }
    
    time_screen_close();
}

static void format_switch_cb(lv_event_t* e)
{
    (void)e;
    s_state.use_24h = lv_obj_has_state(s_state.format_switch, LV_STATE_CHECKED);
    save_settings();
    update_preview();
    
    // Update label colors
    if (s_state.use_24h) {
        lv_obj_set_style_text_color(s_state.format_12h_label, COLOR_TEXT_DIM, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_state.format_24h_label, COLOR_TEXT, LV_PART_MAIN);
    } else {
        lv_obj_set_style_text_color(s_state.format_12h_label, COLOR_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_state.format_24h_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    }
    
    ESP_LOGI(TAG, "Time format changed to: %s", s_state.use_24h ? "24h" : "12h");
}

static void tz_roller_cb(lv_event_t* e)
{
    (void)e;
    uint32_t sel = lv_roller_get_selected(s_state.tz_roller);
    if (sel < NUM_TIMEZONES) {
        s_state.current_tz_index = sel;
        time_mgr_set_timezone(timezones[sel].tz_string);
        ESP_LOGI(TAG, "Timezone changed to: %s", timezones[sel].name);
        update_preview();
    }
}

static void preview_timer_cb(lv_timer_t* timer)
{
    (void)timer;
    update_preview();
    update_sync_status();
}

// ============================================================================
// Public Utility Functions
// ============================================================================

bool time_screen_get_24h_format(void)
{
    // Read from NVS directly (works even when screen isn't created)
    nvs_handle_t nvs;
    bool use_24h = true;  // Default
    
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        uint8_t val = 1;
        nvs_get_u8(nvs, NVS_KEY_24H, &val);
        use_24h = (val != 0);
        nvs_close(nvs);
    }
    
    return use_24h;
}
