/*
 * Arctic Heat Pump Controller
 * Time Settings Screen Implementation
 */
#include "time_screen.h"
#include "time_manager.h"
#include "ui_common.h"
#include <esp_log.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <string.h>

static const char* TAG = "time_screen";

// NVS namespace and keys
#define NVS_NAMESPACE    "time_cfg"
#define NVS_KEY_24H      "use_24h"

// UI Colors (matching app theme)
#define COLOR_BG         0x0d1117
#define COLOR_PANEL      0x161b22
#define COLOR_TEXT       0xc9d1d9
#define COLOR_ACCENT     0x00d4ff
#define COLOR_BORDER     0x30363d

// Common timezone definitions
typedef struct {
    const char* name;       // Display name
    const char* tz_string;  // POSIX TZ string
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

// Internal state
static struct {
    lv_obj_t* container;
    lv_obj_t* tz_dropdown;
    lv_obj_t* format_switch;
    lv_obj_t* preview_label;
    lv_timer_t* preview_timer;
    time_screen_close_cb_t on_close;
    bool use_24h;
    int current_tz_index;
} screen_state = {};

// Forward declarations
static void load_settings(void);
static void save_settings(void);
static void update_preview(void);
static void close_btn_event_cb(lv_event_t* e);
static void tz_dropdown_event_cb(lv_event_t* e);
static void format_switch_event_cb(lv_event_t* e);
static void preview_timer_cb(lv_timer_t* timer);
static int find_current_tz_index(void);

// Load settings from NVS
static void load_settings(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        uint8_t val = 1;  // Default to 24h
        nvs_get_u8(nvs, NVS_KEY_24H, &val);
        screen_state.use_24h = (val != 0);
        nvs_close(nvs);
    } else {
        screen_state.use_24h = true;  // Default
    }
    
    screen_state.current_tz_index = find_current_tz_index();
    ESP_LOGI(TAG, "Loaded settings: 24h=%d, tz_idx=%d", 
             screen_state.use_24h, screen_state.current_tz_index);
}

// Save settings to NVS
static void save_settings(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        nvs_set_u8(nvs, NVS_KEY_24H, screen_state.use_24h ? 1 : 0);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Saved 24h format: %d", screen_state.use_24h);
    }
}

// Find index of current timezone in list
static int find_current_tz_index(void)
{
    const char* current_tz = time_mgr_get_timezone();
    ESP_LOGI(TAG, "Looking for timezone: '%s'", current_tz);
    for (int i = 0; i < (int)NUM_TIMEZONES; i++) {
        if (strcmp(timezones[i].tz_string, current_tz) == 0) {
            ESP_LOGI(TAG, "Found timezone at index %d: %s", i, timezones[i].name);
            return i;
        }
    }
    ESP_LOGW(TAG, "Timezone not found in list, defaulting to 0");
    return 0;  // Default to first (US Eastern)
}

// Update time preview
static void update_preview(void)
{
    if (!screen_state.preview_label) return;
    
    char time_str[64];
    const char* format = screen_state.use_24h ? "%H:%M:%S" : "%I:%M:%S %p";
    
    if (time_mgr_is_synced() && time_mgr_get_time_str(time_str, sizeof(time_str), format)) {
        lv_label_set_text(screen_state.preview_label, time_str);
    } else {
        lv_label_set_text(screen_state.preview_label, 
                          screen_state.use_24h ? "--:--:--" : "--:--:-- --");
    }
}

lv_obj_t* time_screen_create(const time_screen_config_t* config)
{
    if (!config) {
        ESP_LOGE(TAG, "Invalid config");
        return NULL;
    }
    
    screen_state.on_close = config->on_close;
    load_settings();
    
    // Create a new screen
    screen_state.container = lv_obj_create(NULL);
    lv_obj_set_size(screen_state.container, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(screen_state.container, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen_state.container, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen_state.container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen_state.container, 25, LV_PART_MAIN);
    lv_obj_clear_flag(screen_state.container, LV_OBJ_FLAG_SCROLLABLE);
    
    // ===== HEADER ROW: Title + Close Button =====
    lv_obj_t* header = lv_obj_create(screen_state.container);
    lv_obj_set_size(header, LV_PCT(100), 50);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_0, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(header, 0, LV_PART_MAIN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "Time Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);
    
    ui_create_close_button(header, close_btn_event_cb);
    
    // ===== TIME ROW: Current Time + Sync Status =====
    lv_obj_t* time_row = lv_obj_create(screen_state.container);
    lv_obj_set_size(time_row, LV_PCT(100), 80);
    lv_obj_align(time_row, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_bg_color(time_row, lv_color_hex(COLOR_PANEL), LV_PART_MAIN);
    lv_obj_set_style_border_color(time_row, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(time_row, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(time_row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(time_row, 15, LV_PART_MAIN);
    lv_obj_clear_flag(time_row, LV_OBJ_FLAG_SCROLLABLE);
    
    // Current time (left)
    screen_state.preview_label = lv_label_create(time_row);
    lv_obj_set_style_text_font(screen_state.preview_label, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_set_style_text_color(screen_state.preview_label, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    lv_obj_align(screen_state.preview_label, LV_ALIGN_LEFT_MID, 10, 0);
    update_preview();
    
    // Sync status (right)
    lv_obj_t* sync_status = lv_label_create(time_row);
    lv_obj_set_style_text_font(sync_status, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(sync_status, LV_ALIGN_RIGHT_MID, -10, 0);
    if (time_mgr_is_synced()) {
        lv_label_set_text(sync_status, LV_SYMBOL_OK " Synced");
        lv_obj_set_style_text_color(sync_status, lv_color_hex(0x3fb950), LV_PART_MAIN);
    } else {
        lv_label_set_text(sync_status, LV_SYMBOL_WARNING " Not synced");
        lv_obj_set_style_text_color(sync_status, lv_color_hex(0xf0883e), LV_PART_MAIN);
    }
    
    // ===== MAIN CONTENT: Two Columns =====
    lv_obj_t* main_content = lv_obj_create(screen_state.container);
    lv_obj_set_size(main_content, LV_PCT(100), 400);
    lv_obj_align(main_content, LV_ALIGN_TOP_MID, 0, 165);
    lv_obj_set_style_bg_opa(main_content, LV_OPA_0, LV_PART_MAIN);
    lv_obj_set_style_border_width(main_content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(main_content, 0, LV_PART_MAIN);
    lv_obj_clear_flag(main_content, LV_OBJ_FLAG_SCROLLABLE);
    
    // ===== LEFT COLUMN: Format Settings =====
    lv_obj_t* left_col = lv_obj_create(main_content);
    lv_obj_set_size(left_col, LV_PCT(48), LV_PCT(100));
    lv_obj_align(left_col, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(left_col, lv_color_hex(COLOR_PANEL), LV_PART_MAIN);
    lv_obj_set_style_border_color(left_col, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(left_col, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(left_col, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(left_col, 20, LV_PART_MAIN);
    lv_obj_clear_flag(left_col, LV_OBJ_FLAG_SCROLLABLE);
    
    // Format section title
    lv_obj_t* format_title = lv_label_create(left_col);
    lv_label_set_text(format_title, "Display Format");
    lv_obj_set_style_text_font(format_title, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(format_title, lv_color_hex(COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_align(format_title, LV_ALIGN_TOP_MID, 0, 0);
    
    // 12h / 24h toggle
    lv_obj_t* toggle_container = lv_obj_create(left_col);
    lv_obj_set_size(toggle_container, LV_PCT(100), 80);
    lv_obj_align(toggle_container, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_bg_opa(toggle_container, LV_OPA_0, LV_PART_MAIN);
    lv_obj_set_style_border_width(toggle_container, 0, LV_PART_MAIN);
    lv_obj_clear_flag(toggle_container, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t* lbl_12h = lv_label_create(toggle_container);
    lv_label_set_text(lbl_12h, "12h");
    lv_obj_set_style_text_font(lbl_12h, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_12h, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    lv_obj_align(lbl_12h, LV_ALIGN_LEFT_MID, 20, 0);
    
    screen_state.format_switch = lv_switch_create(toggle_container);
    lv_obj_set_size(screen_state.format_switch, 80, 45);
    lv_obj_align(screen_state.format_switch, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(screen_state.format_switch, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_bg_color(screen_state.format_switch, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR | (lv_style_selector_t)LV_STATE_CHECKED);
    if (screen_state.use_24h) {
        lv_obj_add_state(screen_state.format_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(screen_state.format_switch, format_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    
    lv_obj_t* lbl_24h = lv_label_create(toggle_container);
    lv_label_set_text(lbl_24h, "24h");
    lv_obj_set_style_text_font(lbl_24h, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_24h, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
    lv_obj_align(lbl_24h, LV_ALIGN_RIGHT_MID, -20, 0);
    
    // Info text
    lv_obj_t* info_text = lv_label_create(left_col);
    lv_label_set_text(info_text, "Choose how time is\ndisplayed in the UI");
    lv_obj_set_style_text_font(info_text, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(info_text, lv_color_hex(0x666666), LV_PART_MAIN);
    lv_obj_set_style_text_align(info_text, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(info_text, LV_ALIGN_BOTTOM_MID, 0, -20);
    
    // ===== RIGHT COLUMN: Timezone Picker =====
    lv_obj_t* right_col = lv_obj_create(main_content);
    lv_obj_set_size(right_col, LV_PCT(48), LV_PCT(100));
    lv_obj_align(right_col, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(right_col, lv_color_hex(COLOR_PANEL), LV_PART_MAIN);
    lv_obj_set_style_border_color(right_col, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(right_col, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(right_col, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(right_col, 20, LV_PART_MAIN);
    lv_obj_clear_flag(right_col, LV_OBJ_FLAG_SCROLLABLE);
    
    // Timezone section title
    lv_obj_t* tz_title = lv_label_create(right_col);
    lv_label_set_text(tz_title, "Timezone");
    lv_obj_set_style_text_font(tz_title, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(tz_title, lv_color_hex(COLOR_ACCENT), LV_PART_MAIN);
    lv_obj_align(tz_title, LV_ALIGN_TOP_MID, 0, 0);
    
    // Build roller options
    static char tz_options[2048] = {0};
    tz_options[0] = '\0';
    for (size_t i = 0; i < NUM_TIMEZONES; i++) {
        if (i > 0) strcat(tz_options, "\n");
        strcat(tz_options, timezones[i].name);
    }
    
    // Timezone roller - bigger and more visible
    screen_state.tz_dropdown = lv_roller_create(right_col);
    lv_roller_set_options(screen_state.tz_dropdown, tz_options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_selected(screen_state.tz_dropdown, screen_state.current_tz_index, LV_ANIM_OFF);
    lv_roller_set_visible_row_count(screen_state.tz_dropdown, 5);
    lv_obj_set_size(screen_state.tz_dropdown, LV_PCT(100), 320);
    lv_obj_align(screen_state.tz_dropdown, LV_ALIGN_BOTTOM_MID, 0, 0);
    
    // Style the roller - bigger text
    lv_obj_set_style_bg_color(screen_state.tz_dropdown, lv_color_hex(0x1a1f26), LV_PART_MAIN);
    lv_obj_set_style_text_color(screen_state.tz_dropdown, lv_color_hex(0x666666), LV_PART_MAIN);
    lv_obj_set_style_border_color(screen_state.tz_dropdown, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(screen_state.tz_dropdown, 1, LV_PART_MAIN);
    lv_obj_set_style_text_font(screen_state.tz_dropdown, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_radius(screen_state.tz_dropdown, 8, LV_PART_MAIN);
    lv_obj_set_style_line_width(screen_state.tz_dropdown, 0, LV_PART_MAIN);
    
    // Style the selected item
    lv_obj_set_style_bg_color(screen_state.tz_dropdown, lv_color_hex(COLOR_ACCENT), LV_PART_SELECTED);
    lv_obj_set_style_text_color(screen_state.tz_dropdown, lv_color_hex(0xffffff), LV_PART_SELECTED);
    
    lv_obj_add_event_cb(screen_state.tz_dropdown, tz_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    
    // Create preview update timer (every second)
    screen_state.preview_timer = lv_timer_create(preview_timer_cb, 1000, NULL);
    
    // Load the screen
    lv_scr_load(screen_state.container);
    
    ESP_LOGI(TAG, "Time settings screen created");
    return screen_state.container;
}

void time_screen_delete(void)
{
    if (screen_state.preview_timer) {
        lv_timer_del(screen_state.preview_timer);
        screen_state.preview_timer = NULL;
    }
    
    if (screen_state.container) {
        lv_obj_del(screen_state.container);
        screen_state.container = NULL;
    }
    
    screen_state.tz_dropdown = NULL;
    screen_state.format_switch = NULL;
    screen_state.preview_label = NULL;
    screen_state.on_close = NULL;
    
    ESP_LOGI(TAG, "Time settings screen deleted");
}

bool time_screen_is_shown(void)
{
    return screen_state.container != NULL;
}

bool time_screen_get_24h_format(void)
{
    // Load from NVS if not initialized
    static bool loaded = false;
    if (!loaded) {
        nvs_handle_t nvs;
        esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
        if (err == ESP_OK) {
            uint8_t val = 1;
            nvs_get_u8(nvs, NVS_KEY_24H, &val);
            screen_state.use_24h = (val != 0);
            nvs_close(nvs);
        } else {
            screen_state.use_24h = true;
        }
        loaded = true;
    }
    return screen_state.use_24h;
}

// Event callbacks
static void close_btn_event_cb(lv_event_t* e)
{
    (void)e;
    ESP_LOGI(TAG, "Close button clicked");
    
    if (screen_state.on_close) {
        screen_state.on_close();
    }
}

static void tz_dropdown_event_cb(lv_event_t* e)
{
    (void)e;
    uint32_t sel = lv_roller_get_selected(screen_state.tz_dropdown);
    if (sel < NUM_TIMEZONES) {
        screen_state.current_tz_index = sel;
        time_mgr_set_timezone(timezones[sel].tz_string);
        ESP_LOGI(TAG, "Timezone changed to: %s", timezones[sel].name);
        update_preview();
    }
}

static void format_switch_event_cb(lv_event_t* e)
{
    (void)e;
    screen_state.use_24h = lv_obj_has_state(screen_state.format_switch, LV_STATE_CHECKED);
    save_settings();
    update_preview();
    ESP_LOGI(TAG, "Time format changed to: %s", screen_state.use_24h ? "24h" : "12h");
}

static void preview_timer_cb(lv_timer_t* timer)
{
    (void)timer;
    update_preview();
}
