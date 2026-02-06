/*
 * Arctic Heat Pump Controller
 * Settings Screen - Language Panel Implementation
 */
#include "settings_language_panel.h"
#include "settings_common.h"
#include "i18n/i18n.h"
#include <cstdio>
#include <esp_log.h>

static const char* TAG = "settings_language";

// ============================================================================
// Forward Declarations
// ============================================================================

static void language_option_clicked_cb(lv_event_t* e);
static void update_selection_ui(void);
static void async_refresh_settings_cb(void* arg);

// ============================================================================
// Local State
// ============================================================================

static lv_obj_t* s_lang_buttons[LANG_COUNT] = {NULL};
static lv_obj_t* s_current_label = NULL;

// ============================================================================
// Async Refresh Handler
// ============================================================================

static void async_refresh_settings_cb(void* arg)
{
    (void)arg;
    
    // Use the safe refresh function that properly handles screen recreation
    settings_screen_refresh_for_language();
}

// ============================================================================
// Event Handlers
// ============================================================================

static void language_option_clicked_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    
    language_t lang = (language_t)(uintptr_t)lv_event_get_user_data(e);
    
    if (lang < LANG_COUNT && lang != i18n_get_language()) {
        ESP_LOGI(TAG, "Language selected: %s", i18n_get_language_name(lang));
        i18n_set_language(lang);
        
        // Defer screen recreation to next LVGL tick (safe to delete objects)
        lv_async_call(async_refresh_settings_cb, NULL);
    }
}

static void update_selection_ui(void)
{
    language_t current = i18n_get_language();
    
    // Update current language label
    if (s_current_label) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s: %s", 
                 i18n_get(STR_LANG_CURRENT), 
                 i18n_get_language_name(current));
        lv_label_set_text(s_current_label, buf);
    }
    
    // Update button styles
    for (int i = 0; i < LANG_COUNT; i++) {
        if (s_lang_buttons[i]) {
            if ((language_t)i == current) {
                lv_obj_set_style_border_width(s_lang_buttons[i], 3, LV_PART_MAIN);
                lv_obj_set_style_border_color(s_lang_buttons[i], COLOR_ACCENT, LV_PART_MAIN);
            } else {
                lv_obj_set_style_border_width(s_lang_buttons[i], 0, LV_PART_MAIN);
            }
        }
    }
}

// ============================================================================
// Public Functions
// ============================================================================

void language_panel_create(lv_obj_t* parent)
{
    settings_state_t* state = settings_get_state();
    
    state->lang_panel = lv_obj_create(parent);
    lv_obj_set_size(state->lang_panel, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(state->lang_panel, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state->lang_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(state->lang_panel, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(state->lang_panel, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state->lang_panel, 25, LV_PART_MAIN);
    lv_obj_set_flex_flow(state->lang_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(state->lang_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(state->lang_panel, 20, LV_PART_MAIN);
    disable_scrolling(state->lang_panel);
    lv_obj_add_flag(state->lang_panel, LV_OBJ_FLAG_HIDDEN);
    
    // Title
    lv_obj_t* title = lv_label_create(state->lang_panel);
    lv_label_set_text(title, LV_SYMBOL_SETTINGS " Language / Langue / Idioma");
    lv_obj_set_style_text_font(title, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_ACCENT, LV_PART_MAIN);
    
    // Current language
    s_current_label = lv_label_create(state->lang_panel);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s: %s", 
             i18n_get(STR_LANG_CURRENT), 
             i18n_get_language_name(i18n_get_language()));
    lv_label_set_text(s_current_label, buf);
    lv_obj_set_style_text_font(s_current_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_current_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    
    // Language options container
    lv_obj_t* options_container = lv_obj_create(state->lang_panel);
    lv_obj_set_size(options_container, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(options_container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(options_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(options_container, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(options_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(options_container, 12, LV_PART_MAIN);
    disable_scrolling(options_container);
    
    // Create a button for each language
    language_t current_lang = i18n_get_language();
    
    for (int i = 0; i < LANG_COUNT; i++) {
        lv_obj_t* btn = lv_btn_create(options_container);
        lv_obj_set_size(btn, LV_PCT(100), 60);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1a2a4e), LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);
        lv_obj_add_event_cb(btn, language_option_clicked_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)i);
        
        // Highlight current selection
        if ((language_t)i == current_lang) {
            lv_obj_set_style_border_width(btn, 3, LV_PART_MAIN);
            lv_obj_set_style_border_color(btn, COLOR_ACCENT, LV_PART_MAIN);
        }
        
        s_lang_buttons[i] = btn;
        
        // Language name (native) - e.g., "Français"
        lv_obj_t* native_name = lv_label_create(btn);
        lv_label_set_text(native_name, i18n_get_language_name((language_t)i));
        lv_obj_set_style_text_font(native_name, FONT_LARGE, LV_PART_MAIN);
        lv_obj_set_style_text_color(native_name, COLOR_TEXT, LV_PART_MAIN);
        lv_obj_align(native_name, LV_ALIGN_LEFT_MID, 15, 0);
        
        // Checkmark for selected language
        if ((language_t)i == current_lang) {
            lv_obj_t* check = lv_label_create(btn);
            lv_label_set_text(check, LV_SYMBOL_OK);
            lv_obj_set_style_text_font(check, FONT_LARGE, LV_PART_MAIN);
            lv_obj_set_style_text_color(check, COLOR_SUCCESS, LV_PART_MAIN);
            lv_obj_align(check, LV_ALIGN_RIGHT_MID, -15, 0);
        }
    }
    
    // Note about restart
    lv_obj_t* note = lv_label_create(state->lang_panel);
    lv_label_set_text(note, i18n_get(STR_LANG_RESTART_REQUIRED));
    lv_obj_set_style_text_font(note, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(note, COLOR_TEXT_DIM, LV_PART_MAIN);
}

void language_panel_cleanup(void)
{
    for (int i = 0; i < LANG_COUNT; i++) {
        s_lang_buttons[i] = NULL;
    }
    s_current_label = NULL;
}

void language_panel_refresh(void)
{
    update_selection_ui();
}
