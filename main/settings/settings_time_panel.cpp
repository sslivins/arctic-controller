/*
 * Arctic Heat Pump Controller
 * Settings Screen - Time Panel Implementation
 * 
 * Provides timezone and time format (12h/24h) settings.
 */
#include "settings_time_panel.h"
#include "settings_common.h"
#include "time_manager.h"
#include "i18n/i18n.h"
#include <nvs_flash.h>
#include <nvs.h>
#include <string.h>

static const char* TAG = "time_panel";

// NVS namespace and keys
#define NVS_NAMESPACE    "time_cfg"
#define NVS_KEY_24H      "use_24h"

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

// ============================================================================
// Local State
// ============================================================================

static struct {
    lv_obj_t* panel;
    lv_obj_t* preview_label;
    lv_obj_t* sync_status_label;
    lv_obj_t* format_switch;
    lv_obj_t* tz_roller;
    lv_obj_t* format_12h_label;
    lv_obj_t* format_24h_label;
    lv_timer_t* preview_timer;
    bool use_24h;
    int current_tz_index;
} panel_state = {};

// ============================================================================
// Forward Declarations
// ============================================================================

static void load_settings(void);
static void save_settings(void);
static void update_preview(void);
static void update_sync_status(void);
static int find_current_tz_index(void);
static void format_switch_event_cb(lv_event_t* e);
static void tz_roller_event_cb(lv_event_t* e);
static void preview_timer_cb(lv_timer_t* timer);

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
        panel_state.use_24h = (val != 0);
        nvs_close(nvs);
    } else {
        panel_state.use_24h = true;  // Default
    }
    
    panel_state.current_tz_index = find_current_tz_index();
    ESP_LOGI(TAG, "Loaded settings: 24h=%d, tz_idx=%d", 
             panel_state.use_24h, panel_state.current_tz_index);
}

static void save_settings(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        nvs_set_u8(nvs, NVS_KEY_24H, panel_state.use_24h ? 1 : 0);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Saved 24h format: %d", panel_state.use_24h);
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
    return 0;  // Default to first (US Eastern)
}

// ============================================================================
// UI Updates
// ============================================================================

static void update_preview(void)
{
    if (!panel_state.preview_label) return;
    
    char time_str[64];
    const char* format = panel_state.use_24h ? "%H:%M:%S" : "%I:%M:%S %p";
    
    if (time_mgr_is_synced() && time_mgr_get_time_str(time_str, sizeof(time_str), format)) {
        lv_label_set_text(panel_state.preview_label, time_str);
    } else {
        lv_label_set_text(panel_state.preview_label, 
                          panel_state.use_24h ? "--:--:--" : "--:--:-- --");
    }
}

static void update_sync_status(void)
{
    if (!panel_state.sync_status_label) return;
    
    if (time_mgr_is_synced()) {
        char buf[48];
        snprintf(buf, sizeof(buf), LV_SYMBOL_OK " %s", i18n_get(STR_TIME_SYNCED));
        lv_label_set_text(panel_state.sync_status_label, buf);
        lv_obj_set_style_text_color(panel_state.sync_status_label, COLOR_SUCCESS, LV_PART_MAIN);
    } else {
        char buf[48];
        snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING " %s", i18n_get(STR_TIME_NOT_SYNCED));
        lv_label_set_text(panel_state.sync_status_label, buf);
        lv_obj_set_style_text_color(panel_state.sync_status_label, COLOR_WARNING, LV_PART_MAIN);
    }
}

// ============================================================================
// Event Callbacks
// ============================================================================

static void format_switch_event_cb(lv_event_t* e)
{
    (void)e;
    panel_state.use_24h = lv_obj_has_state(panel_state.format_switch, LV_STATE_CHECKED);
    save_settings();
    update_preview();
    
    // Update label colors to indicate selected format
    if (panel_state.use_24h) {
        lv_obj_set_style_text_color(panel_state.format_12h_label, COLOR_TEXT_DIM, LV_PART_MAIN);
        lv_obj_set_style_text_color(panel_state.format_24h_label, COLOR_TEXT, LV_PART_MAIN);
    } else {
        lv_obj_set_style_text_color(panel_state.format_12h_label, COLOR_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(panel_state.format_24h_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    }
    
    ESP_LOGI(TAG, "Time format changed to: %s", panel_state.use_24h ? "24h" : "12h");
}

static void tz_roller_event_cb(lv_event_t* e)
{
    (void)e;
    uint32_t sel = lv_roller_get_selected(panel_state.tz_roller);
    if (sel < NUM_TIMEZONES) {
        panel_state.current_tz_index = sel;
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
// Public API
// ============================================================================

void time_panel_create(lv_obj_t* parent)
{
    settings_state_t* state = settings_get_state();
    load_settings();
    
    // Main panel container
    panel_state.panel = lv_obj_create(parent);
    state->time_panel = panel_state.panel;  // Store in shared state
    lv_obj_set_size(panel_state.panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(panel_state.panel, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel_state.panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel_state.panel, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(panel_state.panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel_state.panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(panel_state.panel, 15, LV_PART_MAIN);
    disable_scrolling(panel_state.panel);
    
    // ===== TITLE =====
    lv_obj_t* title = lv_label_create(panel_state.panel);
    lv_label_set_text(title, i18n_get(STR_TIME_TITLE));
    lv_obj_set_style_text_font(title, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_ACCENT, LV_PART_MAIN);
    
    // ===== CURRENT TIME ROW =====
    lv_obj_t* time_row = lv_obj_create(panel_state.panel);
    lv_obj_set_size(time_row, LV_PCT(100), 70);
    lv_obj_set_style_bg_color(time_row, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_width(time_row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(time_row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(time_row, 15, LV_PART_MAIN);
    disable_scrolling(time_row);
    
    // Current time preview (left)
    panel_state.preview_label = lv_label_create(time_row);
    lv_obj_set_style_text_font(panel_state.preview_label, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_set_style_text_color(panel_state.preview_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(panel_state.preview_label, LV_ALIGN_LEFT_MID, 10, 0);
    
    // Sync status (right)
    panel_state.sync_status_label = lv_label_create(time_row);
    lv_obj_set_style_text_font(panel_state.sync_status_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_align(panel_state.sync_status_label, LV_ALIGN_RIGHT_MID, -10, 0);
    
    update_preview();
    update_sync_status();
    
    // ===== TWO COLUMN LAYOUT =====
    lv_obj_t* columns = lv_obj_create(panel_state.panel);
    lv_obj_set_size(columns, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(columns, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(columns, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(columns, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(columns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(columns, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(columns, 15, LV_PART_MAIN);
    disable_scrolling(columns);
    
    // ===== LEFT COLUMN: Format Settings =====
    lv_obj_t* left_col = lv_obj_create(columns);
    lv_obj_set_size(left_col, LV_PCT(48), 280);
    lv_obj_set_style_bg_color(left_col, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_width(left_col, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(left_col, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(left_col, 20, LV_PART_MAIN);
    disable_scrolling(left_col);
    
    // Format section title
    lv_obj_t* format_title = lv_label_create(left_col);
    lv_label_set_text(format_title, i18n_get(STR_TIME_DISPLAY_FORMAT));
    lv_obj_set_style_text_font(format_title, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(format_title, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(format_title, LV_ALIGN_TOP_MID, 0, 0);
    
    // 12h / 24h toggle container
    lv_obj_t* toggle_container = lv_obj_create(left_col);
    lv_obj_set_size(toggle_container, LV_PCT(100), 80);
    lv_obj_align(toggle_container, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_bg_opa(toggle_container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(toggle_container, 0, LV_PART_MAIN);
    disable_scrolling(toggle_container);
    
    // 12h label
    panel_state.format_12h_label = lv_label_create(toggle_container);
    lv_label_set_text(panel_state.format_12h_label, i18n_get(STR_TIME_FORMAT_12H));
    lv_obj_set_style_text_font(panel_state.format_12h_label, FONT_LARGE, LV_PART_MAIN);
    lv_obj_align(panel_state.format_12h_label, LV_ALIGN_LEFT_MID, 10, 0);
    
    // Switch
    panel_state.format_switch = lv_switch_create(toggle_container);
    lv_obj_set_size(panel_state.format_switch, 80, 45);
    lv_obj_align(panel_state.format_switch, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(panel_state.format_switch, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_bg_color(panel_state.format_switch, COLOR_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (panel_state.use_24h) {
        lv_obj_add_state(panel_state.format_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(panel_state.format_switch, format_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    
    // 24h label
    panel_state.format_24h_label = lv_label_create(toggle_container);
    lv_label_set_text(panel_state.format_24h_label, i18n_get(STR_TIME_FORMAT_24H));
    lv_obj_set_style_text_font(panel_state.format_24h_label, FONT_LARGE, LV_PART_MAIN);
    lv_obj_align(panel_state.format_24h_label, LV_ALIGN_RIGHT_MID, -10, 0);
    
    // Set initial label colors
    if (panel_state.use_24h) {
        lv_obj_set_style_text_color(panel_state.format_12h_label, COLOR_TEXT_DIM, LV_PART_MAIN);
        lv_obj_set_style_text_color(panel_state.format_24h_label, COLOR_TEXT, LV_PART_MAIN);
    } else {
        lv_obj_set_style_text_color(panel_state.format_12h_label, COLOR_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(panel_state.format_24h_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    }
    
    // Info text
    lv_obj_t* info_text = lv_label_create(left_col);
    lv_label_set_text(info_text, i18n_get(STR_TIME_FORMAT_INFO));
    lv_obj_set_style_text_font(info_text, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(info_text, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_align(info_text, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(info_text, LV_PCT(100));
    lv_obj_align(info_text, LV_ALIGN_BOTTOM_MID, 0, -10);
    
    // ===== RIGHT COLUMN: Timezone Picker =====
    lv_obj_t* right_col = lv_obj_create(columns);
    lv_obj_set_size(right_col, LV_PCT(48), 280);
    lv_obj_set_style_bg_color(right_col, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_width(right_col, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(right_col, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(right_col, 20, LV_PART_MAIN);
    disable_scrolling(right_col);
    
    // Timezone section title
    lv_obj_t* tz_title = lv_label_create(right_col);
    lv_label_set_text(tz_title, i18n_get(STR_TIME_TIMEZONE));
    lv_obj_set_style_text_font(tz_title, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(tz_title, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(tz_title, LV_ALIGN_TOP_MID, 0, 0);
    
    // Build roller options
    static char tz_options[2048] = {0};
    tz_options[0] = '\0';
    for (size_t i = 0; i < NUM_TIMEZONES; i++) {
        if (i > 0) strcat(tz_options, "\n");
        strcat(tz_options, timezones[i].name);
    }
    
    // Timezone roller
    panel_state.tz_roller = lv_roller_create(right_col);
    lv_roller_set_options(panel_state.tz_roller, tz_options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_selected(panel_state.tz_roller, panel_state.current_tz_index, LV_ANIM_OFF);
    lv_roller_set_visible_row_count(panel_state.tz_roller, 5);
    lv_obj_set_size(panel_state.tz_roller, LV_PCT(100), 200);
    lv_obj_align(panel_state.tz_roller, LV_ALIGN_BOTTOM_MID, 0, 0);
    
    // Style the roller
    lv_obj_set_style_bg_color(panel_state.tz_roller, lv_color_hex(0x1a1f26), LV_PART_MAIN);
    lv_obj_set_style_text_color(panel_state.tz_roller, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel_state.tz_roller, lv_color_hex(0x30363d), LV_PART_MAIN);
    lv_obj_set_style_border_width(panel_state.tz_roller, 1, LV_PART_MAIN);
    lv_obj_set_style_text_font(panel_state.tz_roller, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_radius(panel_state.tz_roller, 8, LV_PART_MAIN);
    lv_obj_set_style_line_width(panel_state.tz_roller, 0, LV_PART_MAIN);
    
    // Style the selected item - dark text on accent for better readability
    lv_obj_set_style_bg_color(panel_state.tz_roller, COLOR_ACCENT, LV_PART_SELECTED);
    lv_obj_set_style_text_color(panel_state.tz_roller, lv_color_hex(0x0d1117), LV_PART_SELECTED);
    
    lv_obj_add_event_cb(panel_state.tz_roller, tz_roller_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    
    // Create preview update timer (every second)
    panel_state.preview_timer = lv_timer_create(preview_timer_cb, 1000, NULL);
    
    // Hide by default
    lv_obj_add_flag(panel_state.panel, LV_OBJ_FLAG_HIDDEN);
    
    ESP_LOGI(TAG, "Time panel created");
}

void time_panel_delete(void)
{
    if (panel_state.preview_timer) {
        lv_timer_delete(panel_state.preview_timer);
        panel_state.preview_timer = NULL;
    }
    
    panel_state.panel = NULL;
    panel_state.preview_label = NULL;
    panel_state.sync_status_label = NULL;
    panel_state.format_switch = NULL;
    panel_state.tz_roller = NULL;
    panel_state.format_12h_label = NULL;
    panel_state.format_24h_label = NULL;
}

void time_panel_show(void)
{
    settings_state_t* state = settings_get_state();
    
    // Hide all panels first
    if (state->wifi_panel) lv_obj_add_flag(state->wifi_panel, LV_OBJ_FLAG_HIDDEN);
    if (state->fw_panel) lv_obj_add_flag(state->fw_panel, LV_OBJ_FLAG_HIDDEN);
    if (state->lang_panel) lv_obj_add_flag(state->lang_panel, LV_OBJ_FLAG_HIDDEN);
    if (panel_state.panel) lv_obj_remove_flag(panel_state.panel, LV_OBJ_FLAG_HIDDEN);
    
    // Update preview
    update_preview();
    update_sync_status();
}

void time_panel_hide(void)
{
    if (panel_state.panel) {
        lv_obj_add_flag(panel_state.panel, LV_OBJ_FLAG_HIDDEN);
    }
}

bool time_panel_get_24h_format(void)
{
    // Load from NVS if not initialized
    static bool loaded = false;
    if (!loaded) {
        nvs_handle_t nvs;
        esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
        if (err == ESP_OK) {
            uint8_t val = 1;
            nvs_get_u8(nvs, NVS_KEY_24H, &val);
            panel_state.use_24h = (val != 0);
            nvs_close(nvs);
        } else {
            panel_state.use_24h = true;
        }
        loaded = true;
    }
    return panel_state.use_24h;
}
