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
#include "location_manager.h"
#include "weather.h"
#include "geocoding.h"
#include "i18n/i18n.h"
#include "fonts/fonts.h"
#include <esp_log.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "time_screen";

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

    // Location
    lv_obj_t* location_value_label;   // current location name in the Location card

    // Timezone auto/manual
    lv_obj_t* tz_auto_switch;
    lv_obj_t* tz_auto_value_label;    // read-only derived zone (shown in auto mode)

    // Location search dialog
    lv_obj_t* search_dialog;
    lv_obj_t* search_textarea;
    lv_obj_t* search_keyboard;
    lv_obj_t* search_results_list;
    lv_obj_t* search_status_label;
    lv_timer_t* search_debounce_timer;

    // State
    bool use_24h;
    bool tz_auto;
    int current_tz_index;
    lv_timer_t* preview_timer;
    
} time_screen_state_t;

static time_screen_state_t s_state = {};

// ============================================================================
// Geocoding worker state (single in-flight search; guarded by s_geo_busy)
// ============================================================================

#define MAX_GEO_RESULTS 6
static geo_result_t s_geo_results[MAX_GEO_RESULTS];
static int          s_geo_count = 0;      // >=0: result count, <0: error
static char         s_geo_query[128];
static volatile bool s_geo_busy = false;
static uint32_t     s_search_gen = 0;     // bumped on each dialog open/close

// Stable per-row test tags so the device harness can find/tap results.
static const char* const k_result_tags[MAX_GEO_RESULTS] = {
    "loc_result_0", "loc_result_1", "loc_result_2",
    "loc_result_3", "loc_result_4", "loc_result_5",
};

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

// Location + timezone-auto
static void create_location_card(lv_obj_t* parent);
static void create_timezone_card(lv_obj_t* parent);
static void update_location_label(void);
static void update_tz_display(void);
static void derived_tz_display(char* buf, size_t buf_len);
static void tz_auto_switch_cb(lv_event_t* e);
static void change_location_btn_cb(lv_event_t* e);

// Location search dialog
static void open_search_dialog(void);
static void close_search_dialog(void);
static void search_textarea_cb(lv_event_t* e);
static void search_debounce_cb(lv_timer_t* timer);
static void search_cancel_cb(lv_event_t* e);
static void geocoding_worker(void* arg);
static void populate_results_cb(void* arg);
static void result_selected_cb(lv_event_t* e);

// ============================================================================
// Settings Persistence
// ============================================================================

static void load_settings(void)
{
    // Get format from time_manager (which has the authoritative in-memory state)
    s_state.use_24h = time_mgr_get_24h_format();
    
    s_state.current_tz_index = find_current_tz_index();
    s_state.tz_auto = location_mgr_get_tz_auto();
    ESP_LOGI(TAG, "Loaded settings: 24h=%d, tz_idx=%d, tz_auto=%d",
             s_state.use_24h, s_state.current_tz_index, s_state.tz_auto);
}

static void save_settings(void)
{
    // Use time_manager API to save format - it updates both NVS and in-memory state
    time_mgr_set_24h_format(s_state.use_24h);
    ESP_LOGI(TAG, "Saved 24h format: %d", s_state.use_24h);
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

    // Tear down the search dialog + its debounce timer and invalidate any
    // in-flight geocoding worker's pending UI callback.
    close_search_dialog();
    
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
    lv_obj_set_user_data(s_state.back_btn, (void*)"time_back");
    
    lv_obj_t* back_icon = lv_label_create(s_state.back_btn);
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_icon, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(back_icon, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_center(back_icon);
    
    // Title
    lv_obj_t* title = lv_label_create(s_state.header);
    lv_label_set_text(title, i18n_get(STR_SETTINGS_TIME));
    lv_obj_set_style_text_font(title, UI_FONT_HEADER, LV_PART_MAIN);
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
    lv_obj_set_user_data(s_state.preview_label, (void*)"time_preview");
    
    // Sync status (right)
    s_state.sync_status_label = lv_label_create(time_row);
    lv_obj_set_style_text_font(s_state.sync_status_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_align(s_state.sync_status_label, LV_ALIGN_RIGHT_MID, -10, 0);
    
    update_preview();
    update_sync_status();

    // ===== LOCATION CARD =====
    create_location_card(s_state.content);
    
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
    
    // iOS-style single row: "24-Hour Time" label (left) + switch (right).
    // On = 24-hour, off = 12-hour. Same underlying use_24h boolean.
    lv_obj_t* toggle_row = lv_obj_create(format_card);
    lv_obj_set_size(toggle_row, LV_PCT(100), 60);
    lv_obj_align(toggle_row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(toggle_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(toggle_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(toggle_row, 0, LV_PART_MAIN);
    disable_scrolling(toggle_row);

    lv_obj_t* format_label = lv_label_create(toggle_row);
    lv_label_set_text(format_label, i18n_get(STR_TIME_24H_FORMAT));
    lv_obj_set_style_text_font(format_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(format_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(format_label, LV_ALIGN_LEFT_MID, 0, 0);

    s_state.format_switch = lv_switch_create(toggle_row);
    lv_obj_set_size(s_state.format_switch, 80, 45);
    lv_obj_align(s_state.format_switch, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_state.format_switch, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_state.format_switch, COLOR_ACCENT,
                              LV_PART_INDICATOR | (lv_style_selector_t)LV_STATE_CHECKED);
    if (s_state.use_24h) {
        lv_obj_add_state(s_state.format_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(s_state.format_switch, format_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_user_data(s_state.format_switch, (void*)"time_format_switch");
    
    // ===== TIMEZONE CARD =====
    create_timezone_card(s_state.content);
}

// ============================================================================
// Location + Timezone-auto UI
// ============================================================================

static void create_location_card(lv_obj_t* parent)
{
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 20, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 12, LV_PART_MAIN);
    disable_scrolling(card);

    // Section title
    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, i18n_get(STR_LOCATION_TITLE));
    lv_obj_set_style_text_font(title, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_ACCENT, LV_PART_MAIN);

    // Row: current location name (left) + Change button (right)
    lv_obj_t* row = lv_obj_create(card);
    lv_obj_set_size(row, LV_PCT(100), 70);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    disable_scrolling(row);

    s_state.location_value_label = lv_label_create(row);
    lv_obj_set_style_text_font(s_state.location_value_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_state.location_value_label, COLOR_TEXT, LV_PART_MAIN);
    lv_label_set_long_mode(s_state.location_value_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_state.location_value_label, LV_PCT(60));
    lv_obj_align(s_state.location_value_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_user_data(s_state.location_value_label, (void*)"location_value");

    lv_obj_t* change_btn = lv_btn_create(row);
    lv_obj_set_size(change_btn, 200, 64);
    lv_obj_align(change_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(change_btn, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(change_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(change_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(change_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(change_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(change_btn, change_location_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_user_data(change_btn, (void*)"location_change_btn");

    lv_obj_t* change_lbl = lv_label_create(change_btn);
    lv_label_set_text(change_lbl, i18n_get(STR_LOCATION_CHANGE));
    lv_obj_set_style_text_font(change_lbl, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(change_lbl, lv_color_hex(0x0d1117), LV_PART_MAIN);
    lv_obj_center(change_lbl);

    update_location_label();
}

static void create_timezone_card(lv_obj_t* parent)
{
    lv_obj_t* tz_card = lv_obj_create(parent);
    lv_obj_set_size(tz_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(tz_card, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_width(tz_card, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(tz_card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tz_card, 20, LV_PART_MAIN);
    lv_obj_set_flex_flow(tz_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tz_card, 10, LV_PART_MAIN);
    disable_scrolling(tz_card);

    // Timezone section title
    lv_obj_t* tz_title = lv_label_create(tz_card);
    lv_label_set_text(tz_title, i18n_get(STR_TIME_TIMEZONE));
    lv_obj_set_style_text_font(tz_title, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(tz_title, COLOR_ACCENT, LV_PART_MAIN);

    // ---- Automatic toggle row ----
    lv_obj_t* auto_row = lv_obj_create(tz_card);
    lv_obj_set_size(auto_row, LV_PCT(100), 60);
    lv_obj_set_style_bg_opa(auto_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(auto_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(auto_row, 0, LV_PART_MAIN);
    disable_scrolling(auto_row);

    lv_obj_t* auto_label = lv_label_create(auto_row);
    lv_label_set_text(auto_label, i18n_get(STR_TIME_TZ_AUTOMATIC));
    lv_obj_set_style_text_font(auto_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(auto_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(auto_label, LV_ALIGN_LEFT_MID, 0, 0);

    s_state.tz_auto_switch = lv_switch_create(auto_row);
    lv_obj_set_size(s_state.tz_auto_switch, 80, 45);
    lv_obj_align(s_state.tz_auto_switch, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_state.tz_auto_switch, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_state.tz_auto_switch, COLOR_ACCENT,
                              LV_PART_INDICATOR | (lv_style_selector_t)LV_STATE_CHECKED);
    if (s_state.tz_auto) {
        lv_obj_add_state(s_state.tz_auto_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(s_state.tz_auto_switch, tz_auto_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_user_data(s_state.tz_auto_switch, (void*)"tz_auto_switch");

    // ---- Auto derived-zone read-only label (shown when auto) ----
    s_state.tz_auto_value_label = lv_label_create(tz_card);
    lv_obj_set_style_text_font(s_state.tz_auto_value_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_state.tz_auto_value_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_width(s_state.tz_auto_value_label, LV_PCT(100));
    lv_obj_set_user_data(s_state.tz_auto_value_label, (void*)"tz_auto_value");

    // ---- Manual timezone roller (shown when auto is off) ----
    static char tz_options[2048] = {0};
    tz_options[0] = '\0';
    for (size_t i = 0; i < NUM_TIMEZONES; i++) {
        if (i > 0) strcat(tz_options, "\n");
        strcat(tz_options, timezones[i].name);
    }

    s_state.tz_roller = lv_roller_create(tz_card);
    lv_roller_set_options(s_state.tz_roller, tz_options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_selected(s_state.tz_roller, s_state.current_tz_index, LV_ANIM_OFF);
    lv_roller_set_visible_row_count(s_state.tz_roller, 7);
    lv_obj_set_width(s_state.tz_roller, LV_PCT(100));
    lv_obj_set_style_bg_color(s_state.tz_roller, lv_color_hex(0x1a1f26), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_state.tz_roller, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_state.tz_roller, lv_color_hex(0x30363d), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_state.tz_roller, 1, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_state.tz_roller, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_radius(s_state.tz_roller, 12, LV_PART_MAIN);
    lv_obj_set_style_line_width(s_state.tz_roller, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_state.tz_roller, COLOR_ACCENT, LV_PART_SELECTED);
    lv_obj_set_style_text_color(s_state.tz_roller, lv_color_hex(0x0d1117), LV_PART_SELECTED);
    lv_obj_add_event_cb(s_state.tz_roller, tz_roller_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_user_data(s_state.tz_roller, (void*)"timezone_roller");

    update_tz_display();
}

// Build the read-only derived-timezone string for auto mode.
static void derived_tz_display(char* buf, size_t buf_len)
{
    const location_t* loc = location_mgr_get();
    const char* posix = location_mgr_derived_posix();
    if (posix) {
        // Prefer the friendly roller name when the POSIX rule matches one.
        for (size_t i = 0; i < NUM_TIMEZONES; i++) {
            if (strcmp(timezones[i].tz_string, posix) == 0) {
                snprintf(buf, buf_len, "%s", timezones[i].name);
                return;
            }
        }
        snprintf(buf, buf_len, "%s", loc->iana_tz);
    } else if (loc->iana_tz[0] != '\0') {
        snprintf(buf, buf_len, "%s %s", loc->iana_tz, i18n_get(STR_TIME_TZ_UNMAPPED));
    } else {
        snprintf(buf, buf_len, "%s", i18n_get(STR_TIME_TZ_UNMAPPED));
    }
}

// Toggle roller vs. read-only auto label based on tz_auto.
static void update_tz_display(void)
{
    if (!s_state.tz_roller || !s_state.tz_auto_value_label) {
        return;
    }
    if (s_state.tz_auto) {
        char buf[96];
        derived_tz_display(buf, sizeof(buf));
        lv_label_set_text(s_state.tz_auto_value_label, buf);
        lv_obj_clear_flag(s_state.tz_auto_value_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_state.tz_roller, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_state.tz_auto_value_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_state.tz_roller, LV_OBJ_FLAG_HIDDEN);
        // Keep the roller aligned with the active timezone.
        s_state.current_tz_index = find_current_tz_index();
        lv_roller_set_selected(s_state.tz_roller, s_state.current_tz_index, LV_ANIM_OFF);
    }
}

static void update_location_label(void)
{
    if (!s_state.location_value_label) {
        return;
    }
    const location_t* loc = location_mgr_get();
    lv_label_set_text(s_state.location_value_label,
                      loc->name[0] ? loc->name : i18n_get(STR_LOCATION_NONE));
}

static void tz_auto_switch_cb(lv_event_t* e)
{
    (void)e;
    s_state.tz_auto = lv_obj_has_state(s_state.tz_auto_switch, LV_STATE_CHECKED);
    location_mgr_set_tz_auto(s_state.tz_auto);
    update_tz_display();
    update_preview();
    ESP_LOGI(TAG, "Timezone auto-mode: %d", s_state.tz_auto);
}

static void change_location_btn_cb(lv_event_t* e)
{
    (void)e;
    open_search_dialog();
}

// ============================================================================
// Location search dialog
// ============================================================================

static void open_search_dialog(void)
{
    if (s_state.search_dialog) {
        return;  // already open
    }
    s_search_gen++;  // invalidate any late worker from a previous session

    // Full-screen overlay
    s_state.search_dialog = lv_obj_create(s_state.screen);
    lv_obj_set_size(s_state.search_dialog, LV_PCT(100), LV_PCT(100));
    lv_obj_center(s_state.search_dialog);
    lv_obj_set_style_bg_color(s_state.search_dialog, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_state.search_dialog, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_state.search_dialog, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_state.search_dialog, 0, LV_PART_MAIN);
    disable_scrolling(s_state.search_dialog);

    // Top row: Cancel (left) + search textarea (fills remaining width)
    lv_obj_t* top = lv_obj_create(s_state.search_dialog);
    lv_obj_set_size(top, LV_PCT(96), 90);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(top, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(top, 0, LV_PART_MAIN);
    disable_scrolling(top);

    lv_obj_t* cancel_btn = lv_btn_create(top);
    lv_obj_set_size(cancel_btn, 150, 70);
    lv_obj_align(cancel_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_radius(cancel_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(cancel_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(cancel_btn, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(cancel_btn, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_add_event_cb(cancel_btn, search_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_user_data(cancel_btn, (void*)"location_search_cancel");
    lv_obj_t* cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, i18n_get(STR_CANCEL));
    lv_obj_set_style_text_font(cancel_lbl, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(cancel_lbl, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_center(cancel_lbl);

    s_state.search_textarea = lv_textarea_create(top);
    lv_obj_set_size(s_state.search_textarea, LV_PCT(70), 80);
    lv_obj_align(s_state.search_textarea, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_textarea_set_one_line(s_state.search_textarea, true);
    lv_textarea_set_placeholder_text(s_state.search_textarea, i18n_get(STR_LOCATION_SEARCH_HINT));
    lv_obj_set_style_text_font(s_state.search_textarea, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_add_event_cb(s_state.search_textarea, search_textarea_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_user_data(s_state.search_textarea, (void*)"location_search_input");

    // Status line
    s_state.search_status_label = lv_label_create(s_state.search_dialog);
    lv_label_set_text(s_state.search_status_label, i18n_get(STR_LOCATION_SEARCH_HINT));
    lv_obj_set_style_text_font(s_state.search_status_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_state.search_status_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(s_state.search_status_label, LV_ALIGN_TOP_MID, 0, 130);
    lv_obj_set_user_data(s_state.search_status_label, (void*)"location_search_status");

    // Results list (scrollable flex column)
    s_state.search_results_list = lv_obj_create(s_state.search_dialog);
    lv_obj_set_size(s_state.search_results_list, LV_PCT(96), LV_PCT(48));
    lv_obj_align(s_state.search_results_list, LV_ALIGN_TOP_MID, 0, 175);
    lv_obj_set_style_bg_color(s_state.search_results_list, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_state.search_results_list, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_state.search_results_list, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_state.search_results_list, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_state.search_results_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_state.search_results_list, 8, LV_PART_MAIN);

    // Keyboard at bottom - 25% height
    s_state.search_keyboard = lv_keyboard_create(s_state.search_dialog);
    lv_obj_set_size(s_state.search_keyboard, LV_PCT(100), LV_PCT(25));
    lv_obj_align(s_state.search_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_state.search_keyboard, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_state.search_keyboard, FONT_NORMAL, LV_PART_ITEMS);
    lv_keyboard_set_textarea(s_state.search_keyboard, s_state.search_textarea);

    // Debounce timer (paused; restarted on each keystroke)
    s_state.search_debounce_timer = lv_timer_create(search_debounce_cb, 450, NULL);
    lv_timer_pause(s_state.search_debounce_timer);
}

static void close_search_dialog(void)
{
    if (!s_state.search_dialog) {
        return;
    }
    s_search_gen++;  // invalidate any in-flight worker's pending UI update
    if (s_state.search_debounce_timer) {
        lv_timer_delete(s_state.search_debounce_timer);
        s_state.search_debounce_timer = NULL;
    }
    lv_obj_delete(s_state.search_dialog);
    s_state.search_dialog = NULL;
    s_state.search_textarea = NULL;
    s_state.search_keyboard = NULL;
    s_state.search_results_list = NULL;
    s_state.search_status_label = NULL;
}

static void search_cancel_cb(lv_event_t* e)
{
    (void)e;
    close_search_dialog();
}

static void search_textarea_cb(lv_event_t* e)
{
    (void)e;
    if (s_state.search_debounce_timer) {
        lv_timer_reset(s_state.search_debounce_timer);
        lv_timer_resume(s_state.search_debounce_timer);
    }
}

static void search_debounce_cb(lv_timer_t* timer)
{
    lv_timer_pause(timer);
    if (!s_state.search_textarea || !s_state.search_status_label) {
        return;
    }
    const char* q = lv_textarea_get_text(s_state.search_textarea);
    if (!q || strlen(q) < 3) {
        lv_label_set_text(s_state.search_status_label, i18n_get(STR_LOCATION_SEARCH_HINT));
        return;
    }
    if (s_geo_busy) {
        return;  // a search is running; the next keystroke will re-trigger
    }
    strncpy(s_geo_query, q, sizeof(s_geo_query) - 1);
    s_geo_query[sizeof(s_geo_query) - 1] = '\0';
    s_geo_busy = true;
    lv_label_set_text(s_state.search_status_label, i18n_get(STR_LOCATION_SEARCHING));

    uint32_t gen = s_search_gen;
    // 12 KB stack: HTTPS/TLS handshake via the ESP certificate bundle is heavy.
    if (xTaskCreate(geocoding_worker, "geo_search", 12288,
                    (void*)(intptr_t)gen, 5, NULL) != pdPASS) {
        s_geo_busy = false;
        lv_label_set_text(s_state.search_status_label, i18n_get(STR_LOCATION_SEARCH_FAILED));
    }
}

static void geocoding_worker(void* arg)
{
    uint32_t gen = (uint32_t)(intptr_t)arg;
    s_geo_count = geocoding_search(s_geo_query, s_geo_results, MAX_GEO_RESULTS);
    lv_async_call(populate_results_cb, (void*)(intptr_t)gen);
    vTaskDelete(NULL);
}

static void populate_results_cb(void* arg)
{
    uint32_t gen = (uint32_t)(intptr_t)arg;
    s_geo_busy = false;

    // Bail if the dialog closed or was reopened since this search launched.
    if (gen != s_search_gen || !s_state.search_dialog || !s_state.search_results_list) {
        return;
    }

    lv_obj_clean(s_state.search_results_list);

    if (s_geo_count < 0) {
        lv_label_set_text(s_state.search_status_label, i18n_get(STR_LOCATION_SEARCH_FAILED));
        return;
    }
    if (s_geo_count == 0) {
        lv_label_set_text(s_state.search_status_label, i18n_get(STR_LOCATION_NO_MATCHES));
        return;
    }
    lv_label_set_text(s_state.search_status_label, i18n_get(STR_LOCATION_SELECT));

    for (int i = 0; i < s_geo_count && i < MAX_GEO_RESULTS; i++) {
        char label[160];
        geocoding_format_label(&s_geo_results[i], label, sizeof(label));

        lv_obj_t* btn = lv_btn_create(s_state.search_results_list);
        lv_obj_set_size(btn, LV_PCT(100), 72);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1a1f26), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(btn, result_selected_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_set_user_data(btn, (void*)k_result_tags[i]);

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, label);
        lv_obj_set_style_text_font(lbl, FONT_NORMAL, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, COLOR_TEXT, LV_PART_MAIN);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, LV_PCT(94));
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
    }

    // If the user kept typing during the search, re-run for the latest text.
    const char* cur = lv_textarea_get_text(s_state.search_textarea);
    if (cur && strcmp(cur, s_geo_query) != 0 && strlen(cur) >= 3 &&
        s_state.search_debounce_timer) {
        lv_timer_reset(s_state.search_debounce_timer);
        lv_timer_resume(s_state.search_debounce_timer);
    }
}

static void result_selected_cb(lv_event_t* e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_geo_count || idx >= MAX_GEO_RESULTS) {
        return;
    }
    const geo_result_t* r = &s_geo_results[idx];
    char label[160];
    geocoding_format_label(r, label, sizeof(label));
    ESP_LOGI(TAG, "Location selected: %s (%.4f, %.4f) tz=%s",
             label, r->latitude, r->longitude, r->timezone);

    location_mgr_set(r->latitude, r->longitude, label, r->timezone);

    update_location_label();
    update_tz_display();
    update_preview();
    close_search_dialog();

    // The location drives the weather query; refresh now that it changed.
    weather_service_refresh();
}

// ============================================================================
// Event Handlers
// ============================================================================

static void back_btn_cb(lv_event_t* e)
{
    (void)e;
    ESP_LOGI(TAG, "Back button clicked");
    
    // Stop the preview timer FIRST to prevent use-after-free
    time_screen_close();
    
    void (*on_back)(void) = s_state.config.on_back;
    if (on_back) {
        on_back();
    } else {
        settings_menu_show();
    }
}

static void format_switch_cb(lv_event_t* e)
{
    (void)e;
    s_state.use_24h = lv_obj_has_state(s_state.format_switch, LV_STATE_CHECKED);
    save_settings();
    update_preview();
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
    // Delegate to time_manager which has the authoritative state
    return time_mgr_get_24h_format();
}
