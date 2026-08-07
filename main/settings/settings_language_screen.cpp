/*
 * Arctic Heat Pump Controller
 * Settings - Language Screen Implementation (iOS-style full screen)
 * 
 * Full-screen language selection with back navigation.
 * Portrait mode: 720x1280
 */
#include "settings_language_screen.h"
#include "settings_menu.h"
#include "settings_common.h"
#include "nav_bar.h"
#include "i18n/i18n.h"
#include "fonts/fonts.h"
#include <esp_log.h>
#include <string.h>
#include <stdio.h>

static const char* TAG = "language_screen";

// ============================================================================
// Screen State
// ============================================================================

typedef struct {
    bool visible;
    language_screen_config_t config;
    
    // Screen objects
    lv_obj_t* screen;
    lv_obj_t* header;
    lv_obj_t* back_btn;
    lv_obj_t* content;
    
    // Language buttons
    lv_obj_t* lang_buttons[LANG_COUNT];
    lv_obj_t* current_label;
    lv_obj_t* title_label;
    
} language_screen_state_t;

static language_screen_state_t s_state = {};

// String tags for language buttons (indexed by language_t)
static const char* lang_button_tags[] = {"lang_english", "lang_french", "lang_spanish"};

// ============================================================================
// Forward Declarations
// ============================================================================

static void create_header(void);
static void create_content(void);
static void back_btn_cb(lv_event_t* e);
static void language_btn_cb(lv_event_t* e);
static void update_selection_ui(void);

// ============================================================================
// Public API
// ============================================================================

void language_screen_create(const language_screen_config_t* config)
{
    if (s_state.visible) {
        ESP_LOGW(TAG, "Language screen already visible");
        return;
    }
    
    ESP_LOGI(TAG, "Creating Language screen");
    
    memset(&s_state, 0, sizeof(s_state));
    if (config) {
        s_state.config = *config;
    }
    
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

void language_screen_close(void)
{
    if (!s_state.visible) return;
    
    ESP_LOGI(TAG, "Closing Language screen");
    
    s_state.visible = false;
    s_state.screen = NULL;
}

bool language_screen_is_visible(void)
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
    lv_obj_set_user_data(s_state.back_btn, (void*)"language_back");
    
    lv_obj_t* back_icon = lv_label_create(s_state.back_btn);
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_icon, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(back_icon, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_center(back_icon);
    
    // Title
    s_state.title_label = lv_label_create(s_state.header);
    lv_label_set_text(s_state.title_label, i18n_get(STR_SETTINGS_LANGUAGE));
    lv_obj_set_style_text_font(s_state.title_label, UI_FONT_HEADER, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_state.title_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(s_state.title_label, LV_ALIGN_CENTER, 0, 0);
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
    
    // Language card
    lv_obj_t* card = lv_obj_create(s_state.content);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 25, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, 15, LV_PART_MAIN);
    disable_scrolling(card);
    
    // Title
    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, LV_SYMBOL_SETTINGS " Language / Langue / Idioma");
    lv_obj_set_style_text_font(title, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_ACCENT, LV_PART_MAIN);
    
    // Current language
    s_state.current_label = lv_label_create(card);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s: %s", 
             i18n_get(STR_LANG_CURRENT), 
             i18n_get_language_name(i18n_get_language()));
    lv_label_set_text(s_state.current_label, buf);
    lv_obj_set_style_text_font(s_state.current_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_state.current_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    
    // Create a button for each language directly in the card
    language_t current_lang = i18n_get_language();
    
    for (int i = 0; i < LANG_COUNT; i++) {
        lv_obj_t* btn = lv_btn_create(card);
        lv_obj_set_size(btn, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(btn, 70, LV_PART_MAIN);
        lv_obj_set_style_pad_all(btn, 20, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2a3a5e), LV_PART_MAIN);  // Lighter than card
        lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);
        lv_obj_add_event_cb(btn, language_btn_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)i);
        lv_obj_set_user_data(btn, (void*)lang_button_tags[i]);
        
        // Highlight current selection
        if ((language_t)i == current_lang) {
            lv_obj_set_style_border_width(btn, 3, LV_PART_MAIN);
            lv_obj_set_style_border_color(btn, COLOR_ACCENT, LV_PART_MAIN);
        } else {
            lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
        }
        
        s_state.lang_buttons[i] = btn;
        
        // Language name (native)
        lv_obj_t* native_name = lv_label_create(btn);
        lv_label_set_text(native_name, i18n_get_language_name((language_t)i));
        lv_obj_set_style_text_font(native_name, FONT_LARGE, LV_PART_MAIN);
        lv_obj_set_style_text_color(native_name, COLOR_TEXT, LV_PART_MAIN);
        lv_obj_align(native_name, LV_ALIGN_LEFT_MID, 0, 0);
        
        // Checkmark for selected language
        if ((language_t)i == current_lang) {
            lv_obj_t* check = lv_label_create(btn);
            lv_label_set_text(check, LV_SYMBOL_OK);
            lv_obj_set_style_text_font(check, FONT_LARGE, LV_PART_MAIN);
            lv_obj_set_style_text_color(check, COLOR_SUCCESS, LV_PART_MAIN);
            lv_obj_align(check, LV_ALIGN_RIGHT_MID, 0, 0);
        }
    }
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
    
    language_screen_close();
}

static void language_btn_cb(lv_event_t* e)
{
    language_t lang = (language_t)(uintptr_t)lv_event_get_user_data(e);
    
    if (lang < LANG_COUNT && lang != i18n_get_language()) {
        ESP_LOGI(TAG, "Language selected: %s", i18n_get_language_name(lang));
        i18n_set_language(lang);
        nav_bar_refresh_labels();
        
        // Update UI in place - just update the visual selection
        update_selection_ui();
    }
}

static void update_selection_ui(void)
{
    language_t current = i18n_get_language();
    
    // Update header title
    if (s_state.title_label) {
        lv_label_set_text(s_state.title_label, i18n_get(STR_SETTINGS_LANGUAGE));
    }
    
    // Update current language label
    if (s_state.current_label) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s: %s", 
                 i18n_get(STR_LANG_CURRENT), 
                 i18n_get_language_name(current));
        lv_label_set_text(s_state.current_label, buf);
    }
    
    // Update button styles and checkmarks
    for (int i = 0; i < LANG_COUNT; i++) {
        if (s_state.lang_buttons[i]) {
            lv_obj_t* btn = s_state.lang_buttons[i];
            
            if ((language_t)i == current) {
                lv_obj_set_style_border_width(btn, 3, LV_PART_MAIN);
                lv_obj_set_style_border_color(btn, COLOR_ACCENT, LV_PART_MAIN);
                
                // Check if checkmark already exists (look for it as second child)
                uint32_t child_count = lv_obj_get_child_count(btn);
                bool has_check = (child_count >= 2);
                
                if (!has_check) {
                    // Add checkmark
                    lv_obj_t* check = lv_label_create(btn);
                    lv_label_set_text(check, LV_SYMBOL_OK);
                    lv_obj_set_style_text_font(check, FONT_LARGE, LV_PART_MAIN);
                    lv_obj_set_style_text_color(check, COLOR_SUCCESS, LV_PART_MAIN);
                    lv_obj_align(check, LV_ALIGN_RIGHT_MID, 0, 0);
                }
            } else {
                lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
                
                // Remove checkmark if exists (second child)
                uint32_t child_count = lv_obj_get_child_count(btn);
                if (child_count >= 2) {
                    lv_obj_t* check = lv_obj_get_child(btn, 1);
                    if (check) {
                        lv_obj_delete(check);
                    }
                }
            }
        }
    }
}
