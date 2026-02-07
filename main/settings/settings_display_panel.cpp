/*
 * Arctic Heat Pump Controller
 * Settings Screen - Display Panel Implementation
 * 
 * Provides display brightness settings with NVS persistence.
 */
#include "settings_display_panel.h"
#include "settings_common.h"
#include "i18n/i18n.h"
#include <bsp/display.h>
#include <nvs_flash.h>
#include <nvs.h>

static const char* TAG = "display_panel";

// NVS namespace and keys
#define NVS_NAMESPACE    "display_cfg"
#define NVS_KEY_BRIGHTNESS "brightness"

// Default brightness value
#define DEFAULT_BRIGHTNESS 80

// ============================================================================
// Local State
// ============================================================================

static struct {
    lv_obj_t* panel;
    lv_obj_t* brightness_slider;
    lv_obj_t* brightness_value_label;
    int current_brightness;
} panel_state = {};

// ============================================================================
// Forward Declarations
// ============================================================================

static void load_settings(void);
static void save_settings(void);
static void apply_brightness(int brightness);
static void slider_event_cb(lv_event_t* e);

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
        panel_state.current_brightness = val;
        nvs_close(nvs);
    } else {
        panel_state.current_brightness = DEFAULT_BRIGHTNESS;
    }
    ESP_LOGI(TAG, "Loaded brightness: %d%%", panel_state.current_brightness);
}

static void save_settings(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        nvs_set_u8(nvs, NVS_KEY_BRIGHTNESS, (uint8_t)panel_state.current_brightness);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Saved brightness: %d%%", panel_state.current_brightness);
    }
}

static void apply_brightness(int brightness)
{
    if (brightness < 5) brightness = 5;  // Minimum brightness to ensure screen is visible
    if (brightness > 100) brightness = 100;
    
    esp_err_t err = bsp_display_brightness_set(brightness);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set brightness: %s", esp_err_to_name(err));
    }
}

// ============================================================================
// Event Callbacks
// ============================================================================

static void slider_event_cb(lv_event_t* e)
{
    lv_obj_t* slider = (lv_obj_t*)lv_event_get_target(e);
    int value = lv_slider_get_value(slider);
    
    panel_state.current_brightness = value;
    
    // Update value label
    if (panel_state.brightness_value_label) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", value);
        lv_label_set_text(panel_state.brightness_value_label, buf);
    }
    
    // Apply brightness immediately for real-time feedback
    apply_brightness(value);
    
    // Save on release only to reduce NVS writes
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        save_settings();
        ESP_LOGI(TAG, "Brightness changed to: %d%%", value);
    }
}

// ============================================================================
// Public API
// ============================================================================

void display_panel_create(lv_obj_t* parent)
{
    settings_state_t* state = settings_get_state();
    load_settings();
    
    // Main panel container
    panel_state.panel = lv_obj_create(parent);
    state->display_panel = panel_state.panel;  // Store in shared state
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
    lv_label_set_text(title, i18n_get(STR_DISPLAY_TITLE));
    lv_obj_set_style_text_font(title, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_ACCENT, LV_PART_MAIN);
    
    // ===== BRIGHTNESS SECTION =====
    lv_obj_t* brightness_card = lv_obj_create(panel_state.panel);
    lv_obj_set_size(brightness_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(brightness_card, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_width(brightness_card, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(brightness_card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(brightness_card, 20, LV_PART_MAIN);
    lv_obj_set_flex_flow(brightness_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(brightness_card, 15, LV_PART_MAIN);
    disable_scrolling(brightness_card);
    
    // Brightness label row
    lv_obj_t* label_row = lv_obj_create(brightness_card);
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
    panel_state.brightness_value_label = lv_label_create(label_row);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", panel_state.current_brightness);
    lv_label_set_text(panel_state.brightness_value_label, buf);
    lv_obj_set_style_text_font(panel_state.brightness_value_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(panel_state.brightness_value_label, COLOR_ACCENT, LV_PART_MAIN);
    
    // Slider row with Low/High labels
    lv_obj_t* slider_row = lv_obj_create(brightness_card);
    lv_obj_set_size(slider_row, LV_PCT(100), 50);
    lv_obj_set_style_bg_opa(slider_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(slider_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_top(slider_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(slider_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(slider_row, 20, LV_PART_MAIN);   // Buffer for knob overhang
    lv_obj_set_style_pad_right(slider_row, 20, LV_PART_MAIN);  // Buffer for knob overhang
    lv_obj_set_flex_flow(slider_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(slider_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(slider_row, 25, LV_PART_MAIN);
    disable_scrolling(slider_row);
    
    // Low label - natural width based on content
    lv_obj_t* low_label = lv_label_create(slider_row);
    lv_label_set_text(low_label, i18n_get(STR_DISPLAY_BRIGHTNESS_LOW));
    lv_obj_set_style_text_font(low_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(low_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    
    // Slider - flex grow to fill remaining space
    panel_state.brightness_slider = lv_slider_create(slider_row);
    state->brightness_slider = panel_state.brightness_slider;  // Store in shared state
    lv_obj_set_flex_grow(panel_state.brightness_slider, 1);
    lv_obj_set_height(panel_state.brightness_slider, 12);
    lv_slider_set_range(panel_state.brightness_slider, 5, 100);  // Min 5% to keep screen visible
    lv_slider_set_value(panel_state.brightness_slider, panel_state.current_brightness, LV_ANIM_OFF);
    
    // Slider styling
    lv_obj_set_style_bg_color(panel_state.brightness_slider, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_color(panel_state.brightness_slider, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(panel_state.brightness_slider, COLOR_ACCENT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(panel_state.brightness_slider, 12, LV_PART_KNOB);  // Larger knob
    lv_obj_set_ext_click_area(panel_state.brightness_slider, 20);  // Extended touch area
    
    // Add event handlers
    lv_obj_add_event_cb(panel_state.brightness_slider, slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(panel_state.brightness_slider, slider_event_cb, LV_EVENT_RELEASED, NULL);
    
    // High label - natural width based on content
    lv_obj_t* high_label = lv_label_create(slider_row);
    lv_label_set_text(high_label, i18n_get(STR_DISPLAY_BRIGHTNESS_HIGH));
    lv_obj_set_style_text_font(high_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(high_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    
    // Start hidden
    lv_obj_add_flag(panel_state.panel, LV_OBJ_FLAG_HIDDEN);
    
    ESP_LOGI(TAG, "Display panel created with brightness: %d%%", panel_state.current_brightness);
}

void display_panel_delete(void)
{
    panel_state.panel = NULL;
    panel_state.brightness_slider = NULL;
    panel_state.brightness_value_label = NULL;
}

void display_panel_show(void)
{
    if (panel_state.panel) {
        lv_obj_clear_flag(panel_state.panel, LV_OBJ_FLAG_HIDDEN);
    }
}

void display_panel_hide(void)
{
    if (panel_state.panel) {
        lv_obj_add_flag(panel_state.panel, LV_OBJ_FLAG_HIDDEN);
    }
}

int display_panel_get_brightness(void)
{
    return panel_state.current_brightness;
}

void display_panel_init_brightness(void)
{
    // Load and apply saved brightness at startup
    load_settings();
    apply_brightness(panel_state.current_brightness);
    ESP_LOGI(TAG, "Initialized display brightness to %d%%", panel_state.current_brightness);
}
