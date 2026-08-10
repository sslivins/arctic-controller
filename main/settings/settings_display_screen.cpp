/*
 * Arctic Heat Pump Controller
 * Settings - Display Screen Implementation (iOS-style full screen)
 * 
 * Full-screen display brightness settings with back navigation.
 * Portrait mode: 720x1280
 */
#include "settings_display_screen.h"
#include "settings_menu.h"
#include "settings_common.h"
#include "display_idle.h"
#include "i18n/i18n.h"
#include "fonts/fonts.h"
#include <bsp/display.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <string.h>
#include <stdio.h>

static const char* TAG = "display_screen";

// NVS namespace and keys
#define NVS_NAMESPACE    "display_cfg"
#define NVS_KEY_BRIGHTNESS "brightness"
#define DEFAULT_BRIGHTNESS 80

// ============================================================================
// Screen State
// ============================================================================

typedef struct {
    bool visible;
    display_screen_config_t config;
    
    // Screen objects
    lv_obj_t* screen;
    lv_obj_t* header;
    lv_obj_t* back_btn;
    lv_obj_t* content;
    
    // Brightness controls
    lv_obj_t* brightness_slider;
    lv_obj_t* brightness_value_label;
    lv_obj_t* dim_roller;
    lv_obj_t* off_roller;
    
    // State
    int current_brightness;
    
} display_screen_state_t;

static display_screen_state_t s_state = {};

// ============================================================================
// Forward Declarations
// ============================================================================

static void create_header(void);
static void create_content(void);
static void back_btn_cb(lv_event_t* e);
static void slider_cb(lv_event_t* e);
static void idle_roller_cb(lv_event_t* e);
static void load_settings(void);
static bool save_settings(void);
static bool apply_brightness(int brightness);

static void build_timeout_options(char* options, size_t size)
{
    snprintf(options, size, "%s\n1 %s\n2 %s\n3 %s\n4 %s\n5 %s",
             i18n_get(STR_DISPLAY_NEVER),
             i18n_get(STR_DISPLAY_MINUTE),
             i18n_get(STR_DISPLAY_MINUTES),
             i18n_get(STR_DISPLAY_MINUTES),
             i18n_get(STR_DISPLAY_MINUTES),
             i18n_get(STR_DISPLAY_MINUTES));
}

static lv_obj_t* create_timeout_row(lv_obj_t* parent, const char* label_text,
                                    const char* tag, uint8_t selected)
{
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 170);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    disable_scrolling(row);

    lv_obj_t* label = lv_label_create(row);
    lv_label_set_text(label, label_text);
    lv_obj_set_width(label, 300);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, COLOR_TEXT, LV_PART_MAIN);

    lv_obj_t* roller = lv_roller_create(row);
    char options[160];
    build_timeout_options(options, sizeof(options));
    lv_roller_set_options(roller, options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_selected(roller, selected, LV_ANIM_OFF);
    lv_roller_set_visible_row_count(roller, 3);
    lv_obj_set_size(roller, 300, 150);
    lv_obj_set_style_bg_color(roller, lv_color_hex(0x1a1f26), LV_PART_MAIN);
    lv_obj_set_style_text_color(roller, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_border_color(roller, lv_color_hex(0x30363d), LV_PART_MAIN);
    lv_obj_set_style_border_width(roller, 1, LV_PART_MAIN);
    lv_obj_set_style_text_font(roller, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_radius(roller, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(roller, COLOR_ACCENT, LV_PART_SELECTED);
    lv_obj_set_style_text_color(roller, lv_color_hex(0x0d1117), LV_PART_SELECTED);
    lv_obj_set_user_data(roller, (void*)tag);
    lv_obj_add_event_cb(roller, idle_roller_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return roller;
}

// ============================================================================
// Settings Persistence
// ============================================================================

static void load_settings(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        uint8_t val = DEFAULT_BRIGHTNESS;
        nvs_get_u8(nvs, NVS_KEY_BRIGHTNESS, &val);
        s_state.current_brightness = val;
        nvs_close(nvs);
    } else {
        s_state.current_brightness = DEFAULT_BRIGHTNESS;
    }
    if (s_state.current_brightness < 5) s_state.current_brightness = 5;
    if (s_state.current_brightness > 100) s_state.current_brightness = 100;
    ESP_LOGI(TAG, "Loaded brightness: %d%%", s_state.current_brightness);
}

static bool save_settings(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, NVS_KEY_BRIGHTNESS, (uint8_t)s_state.current_brightness);
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Saved brightness: %d%%", s_state.current_brightness);
            return true;
        }
    }
    ESP_LOGE(TAG, "Failed to save brightness: %s", esp_err_to_name(err));
    return false;
}

static bool apply_brightness(int brightness)
{
    if (brightness < 5) brightness = 5;
    if (brightness > 100) brightness = 100;
    
    esp_err_t err = bsp_display_brightness_set(brightness);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set brightness: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

// ============================================================================
// Public API
// ============================================================================

void display_screen_create(const display_screen_config_t* config)
{
    if (s_state.visible) {
        ESP_LOGW(TAG, "Display screen already visible");
        return;
    }
    
    ESP_LOGI(TAG, "Creating Display screen");
    
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
    
    // Load screen with slide-left animation
    lv_screen_load_anim(s_state.screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
    s_state.visible = true;
}

void display_screen_close(void)
{
    if (!s_state.visible) return;
    
    ESP_LOGI(TAG, "Closing Display screen");
    
    s_state.visible = false;
    s_state.screen = NULL;
}

bool display_screen_is_visible(void)
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
    lv_obj_set_user_data(s_state.back_btn, (void*)"display_back");
    
    lv_obj_t* back_icon = lv_label_create(s_state.back_btn);
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_icon, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(back_icon, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_center(back_icon);
    
    // Title
    lv_obj_t* title = lv_label_create(s_state.header);
    lv_label_set_text(title, i18n_get(STR_SETTINGS_DISPLAY));
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
    lv_obj_set_style_pad_row(s_state.content, 20, LV_PART_MAIN);
    disable_scrolling(s_state.content);
    
    // Brightness card
    lv_obj_t* card = lv_obj_create(s_state.content);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 25, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 20, LV_PART_MAIN);
    disable_scrolling(card);
    
    // Title
    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, i18n_get(STR_DISPLAY_TITLE));
    lv_obj_set_style_text_font(title, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_ACCENT, LV_PART_MAIN);
    
    // Brightness label row
    lv_obj_t* label_row = lv_obj_create(card);
    lv_obj_set_size(label_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(label_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(label_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(label_row, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(label_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(label_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    disable_scrolling(label_row);
    
    // Brightness label
    lv_obj_t* brightness_label = lv_label_create(label_row);
    lv_label_set_text(brightness_label, i18n_get(STR_DISPLAY_BRIGHTNESS));
    lv_obj_set_style_text_font(brightness_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(brightness_label, COLOR_TEXT, LV_PART_MAIN);
    
    // Current value label
    s_state.brightness_value_label = lv_label_create(label_row);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", s_state.current_brightness);
    lv_label_set_text(s_state.brightness_value_label, buf);
    lv_obj_set_style_text_font(s_state.brightness_value_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_state.brightness_value_label, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_user_data(s_state.brightness_value_label, (void*)"brightness_value");
    
    // Slider row with Low/High labels
    lv_obj_t* slider_row = lv_obj_create(card);
    lv_obj_set_size(slider_row, LV_PCT(100), 60);
    lv_obj_set_style_bg_opa(slider_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(slider_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_top(slider_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(slider_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(slider_row, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_right(slider_row, 20, LV_PART_MAIN);
    lv_obj_set_flex_flow(slider_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(slider_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(slider_row, 25, LV_PART_MAIN);
    disable_scrolling(slider_row);
    
    // Low label
    lv_obj_t* low_label = lv_label_create(slider_row);
    lv_label_set_text(low_label, i18n_get(STR_DISPLAY_BRIGHTNESS_LOW));
    lv_obj_set_style_text_font(low_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(low_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    
    // Slider
    s_state.brightness_slider = lv_slider_create(slider_row);
    lv_obj_set_flex_grow(s_state.brightness_slider, 1);
    lv_obj_set_height(s_state.brightness_slider, 12);
    lv_slider_set_range(s_state.brightness_slider, 5, 100);
    lv_slider_set_value(s_state.brightness_slider, s_state.current_brightness, LV_ANIM_OFF);
    
    // Slider styling
    lv_obj_set_style_bg_color(s_state.brightness_slider, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_state.brightness_slider, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_state.brightness_slider, COLOR_ACCENT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_state.brightness_slider, 12, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s_state.brightness_slider, 20);
    lv_obj_set_user_data(s_state.brightness_slider, (void*)"brightness_slider");
    
    // Add event handlers
    lv_obj_add_event_cb(s_state.brightness_slider, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_state.brightness_slider, slider_cb, LV_EVENT_RELEASED, NULL);
    
    // High label
    lv_obj_t* high_label = lv_label_create(slider_row);
    lv_label_set_text(high_label, i18n_get(STR_DISPLAY_BRIGHTNESS_HIGH));
    lv_obj_set_style_text_font(high_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(high_label, COLOR_TEXT_DIM, LV_PART_MAIN);

    lv_obj_t* idle_card = lv_obj_create(s_state.content);
    lv_obj_set_size(idle_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(idle_card, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_width(idle_card, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(idle_card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(idle_card, 25, LV_PART_MAIN);
    lv_obj_set_flex_flow(idle_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(idle_card, 10, LV_PART_MAIN);
    disable_scrolling(idle_card);

    lv_obj_t* idle_title = lv_label_create(idle_card);
    lv_label_set_text(idle_title, i18n_get(STR_DISPLAY_IDLE_TITLE));
    lv_obj_set_style_text_font(idle_title, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(idle_title, COLOR_ACCENT, LV_PART_MAIN);

    lv_obj_t* description = lv_label_create(idle_card);
    lv_label_set_text(description, i18n_get(STR_DISPLAY_IDLE_DESCRIPTION));
    lv_obj_set_width(description, LV_PCT(100));
    lv_label_set_long_mode(description, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(description, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(description, COLOR_TEXT_DIM, LV_PART_MAIN);

    s_state.dim_roller = create_timeout_row(
        idle_card, i18n_get(STR_DISPLAY_DIM_AFTER), "display_dim_timeout",
        display_idle_get_dim_minutes());
    s_state.off_roller = create_timeout_row(
        idle_card, i18n_get(STR_DISPLAY_OFF_AFTER_DIM), "display_off_timeout",
        display_idle_get_off_minutes());
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
    
    display_screen_close();
}

static void slider_cb(lv_event_t* e)
{
    lv_obj_t* slider = (lv_obj_t*)lv_event_get_target(e);
    int value = lv_slider_get_value(slider);
    
    s_state.current_brightness = value;
    
    // Update value label
    if (s_state.brightness_value_label) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", value);
        lv_label_set_text(s_state.brightness_value_label, buf);
    }

    // Apply brightness immediately for real-time feedback
    apply_brightness(value);
    
    // Save on release only to reduce NVS writes
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        save_settings();
        ESP_LOGI(TAG, "Brightness changed to: %d%%", value);
    }
}

static void idle_roller_cb(lv_event_t* e)
{
    (void)e;
    const uint8_t dim_minutes =
        static_cast<uint8_t>(lv_roller_get_selected(s_state.dim_roller));
    const uint8_t off_minutes =
        static_cast<uint8_t>(lv_roller_get_selected(s_state.off_roller));
    if (!display_idle_set_timeouts(dim_minutes, off_minutes)) {
        ESP_LOGE(TAG, "Failed to save display idle timeouts");
    }
}

// ============================================================================
// Startup Initialization
// ============================================================================

void display_screen_init_brightness(void)
{
    // Load and apply saved brightness at startup (before screen is created)
    nvs_handle_t nvs;
    int brightness = DEFAULT_BRIGHTNESS;
    
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        uint8_t val = DEFAULT_BRIGHTNESS;
        nvs_get_u8(nvs, NVS_KEY_BRIGHTNESS, &val);
        brightness = val;
        nvs_close(nvs);
    }
    
    // Apply brightness
    if (brightness < 5) brightness = 5;
    if (brightness > 100) brightness = 100;
    bsp_display_brightness_set(brightness);
    
    ESP_LOGI(TAG, "Initialized display brightness to %d%%", brightness);
}

int display_screen_get_brightness(void)
{
    // If screen has been created, use live state
    if (s_state.current_brightness > 0) {
        return s_state.current_brightness;
    }
    // Otherwise read from NVS
    nvs_handle_t nvs;
    int brightness = DEFAULT_BRIGHTNESS;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        uint8_t val = DEFAULT_BRIGHTNESS;
        nvs_get_u8(nvs, NVS_KEY_BRIGHTNESS, &val);
        brightness = val;
        nvs_close(nvs);
    }
    if (brightness < 5) brightness = 5;
    if (brightness > 100) brightness = 100;
    return brightness;
}

bool display_screen_set_brightness(int brightness)
{
    if (brightness < 5 || brightness > 100) return false;

    s_state.current_brightness = brightness;
    if (!apply_brightness(brightness) || !save_settings()) return false;

    return true;
}
