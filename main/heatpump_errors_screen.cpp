/*
 * Arctic Heat Pump Controller
 * Heat Pump Error Details Screen Implementation
 * 
 * Full-screen display of active errors with descriptions and severity
 */

#include "heatpump_errors_screen.h"
#include "heatpump_errors.h"
#include "modbus/arctic_heatpump.h"
#include "ui_common.h"
#include "fonts/fonts.h"
#include "app_preferences.h"
#include <esp_log.h>
#include <stdio.h>

static const char* TAG = "hp_errors_scr";

// ============================================================================
// Colors
// ============================================================================
#define COLOR_BG            lv_color_hex(0x1a1a2e)
#define COLOR_CARD_BG       lv_color_hex(0x16213e)
#define COLOR_HEADER_BG     lv_color_hex(0x0f1a2e)
#define COLOR_TEXT          lv_color_hex(0xeaeaea)
#define COLOR_TEXT_DIM      lv_color_hex(0x888888)
#define COLOR_ACCENT        lv_color_hex(0x00d4ff)
#define COLOR_SUCCESS       lv_color_hex(0x4ade80)
#define COLOR_WARNING       lv_color_hex(0xfbbf24)
#define COLOR_ERROR         lv_color_hex(0xef4444)
#define COLOR_CRITICAL      lv_color_hex(0xdc2626)

// ============================================================================
// State
// ============================================================================
static struct {
    bool shown = false;
    lv_obj_t* screen = nullptr;
    heatpump_errors_close_cb_t on_close = nullptr;
    lv_timer_t* update_timer = nullptr;
    
    // Error list container
    lv_obj_t* error_list = nullptr;
    lv_obj_t* no_errors_label = nullptr;
    lv_obj_t* error_count_label = nullptr;
} state;

// ============================================================================
// Helper Functions
// ============================================================================

static lv_color_t severity_to_color(arctic::ErrorSeverity severity) {
    switch (severity) {
        case arctic::ErrorSeverity::CRITICAL: return COLOR_CRITICAL;
        case arctic::ErrorSeverity::ERROR:    return COLOR_ERROR;
        case arctic::ErrorSeverity::WARNING:  return COLOR_WARNING;
        case arctic::ErrorSeverity::INFO:
        default:                              return COLOR_ACCENT;
    }
}

static const char* severity_to_icon(arctic::ErrorSeverity severity) {
    switch (severity) {
        case arctic::ErrorSeverity::CRITICAL: return LV_SYMBOL_WARNING;
        case arctic::ErrorSeverity::ERROR:    return LV_SYMBOL_WARNING;
        case arctic::ErrorSeverity::WARNING:  return LV_SYMBOL_WARNING;
        case arctic::ErrorSeverity::INFO:
        default:                              return LV_SYMBOL_OK;
    }
}

static lv_obj_t* create_error_card(lv_obj_t* parent, const arctic::ActiveError* error) {
    lv_color_t severity_color = severity_to_color(error->severity);
    
    // Card container
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, severity_color, LV_PART_MAIN);
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 15, LV_PART_MAIN);
    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, 8, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    
    // Top row: icon, code, severity
    lv_obj_t* top_row = lv_obj_create(card);
    lv_obj_set_size(top_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(top_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(top_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(top_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(top_row, LV_OBJ_FLAG_SCROLLABLE);
    
    // Error code (e.g., "E23")
    lv_obj_t* code_label = lv_label_create(top_row);
    char code_buf[16];
    snprintf(code_buf, sizeof(code_buf), "%s %s", severity_to_icon(error->severity), error->code);
    lv_label_set_text(code_label, code_buf);
    lv_obj_set_style_text_font(code_label, &montserrat_24_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(code_label, severity_color, LV_PART_MAIN);
    lv_obj_align(code_label, LV_ALIGN_LEFT_MID, 0, 0);
    
    // Severity label
    lv_obj_t* severity_label = lv_label_create(top_row);
    lv_label_set_text(severity_label, arctic::severityToString(error->severity));
    lv_obj_set_style_text_font(severity_label, &montserrat_16_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(severity_label, severity_color, LV_PART_MAIN);
    lv_obj_align(severity_label, LV_ALIGN_RIGHT_MID, 0, 0);
    
    // Error name (short name like "HIGH_PRESSURE")
    lv_obj_t* name_label = lv_label_create(card);
    lv_label_set_text(name_label, error->name);
    lv_obj_set_style_text_font(name_label, &montserrat_16_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(name_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    
    // Description
    lv_obj_t* desc_label = lv_label_create(card);
    lv_label_set_text(desc_label, error->description);
    lv_label_set_long_mode(desc_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc_label, LV_PCT(100));
    lv_obj_set_style_text_font(desc_label, &montserrat_16_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(desc_label, COLOR_TEXT, LV_PART_MAIN);
    
    return card;
}

static void update_error_list() {
    if (!state.error_list) return;
    
    // Clear existing error cards
    lv_obj_clean(state.error_list);
    
    // Get active errors
    arctic::ActiveError errors[32];
    int count = arctic::getActiveErrors(errors, 32);
    
    // Check demo mode
    bool demo_mode = app_prefs_is_demo_mode();
    
    if (demo_mode) {
        // Show demo mode message
        lv_obj_t* demo_card = lv_obj_create(state.error_list);
        lv_obj_set_size(demo_card, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(demo_card, COLOR_CARD_BG, LV_PART_MAIN);
        lv_obj_set_style_border_width(demo_card, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(demo_card, COLOR_WARNING, LV_PART_MAIN);
        lv_obj_set_style_border_side(demo_card, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
        lv_obj_set_style_radius(demo_card, 12, LV_PART_MAIN);
        lv_obj_set_style_pad_all(demo_card, 20, LV_PART_MAIN);
        lv_obj_clear_flag(demo_card, LV_OBJ_FLAG_SCROLLABLE);
        
        lv_obj_t* demo_label = lv_label_create(demo_card);
        lv_label_set_text(demo_label, LV_SYMBOL_WARNING " Demo Mode Active\n\nNo real error data available.\nErrors will appear here when connected to a heat pump.");
        lv_label_set_long_mode(demo_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(demo_label, LV_PCT(100));
        lv_obj_set_style_text_font(demo_label, &montserrat_16_latin, LV_PART_MAIN);
        lv_obj_set_style_text_color(demo_label, COLOR_WARNING, LV_PART_MAIN);
        lv_obj_set_style_text_align(demo_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        
        if (state.error_count_label) {
            lv_label_set_text(state.error_count_label, "Demo Mode");
        }
        return;
    }
    
    if (count == 0) {
        // No errors - show success message
        lv_obj_t* ok_card = lv_obj_create(state.error_list);
        lv_obj_set_size(ok_card, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(ok_card, COLOR_CARD_BG, LV_PART_MAIN);
        lv_obj_set_style_border_width(ok_card, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(ok_card, COLOR_SUCCESS, LV_PART_MAIN);
        lv_obj_set_style_border_side(ok_card, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
        lv_obj_set_style_radius(ok_card, 12, LV_PART_MAIN);
        lv_obj_set_style_pad_all(ok_card, 25, LV_PART_MAIN);
        lv_obj_clear_flag(ok_card, LV_OBJ_FLAG_SCROLLABLE);
        
        lv_obj_t* ok_label = lv_label_create(ok_card);
        lv_label_set_text(ok_label, LV_SYMBOL_OK " No Active Errors\n\nAll systems operating normally.");
        lv_label_set_long_mode(ok_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(ok_label, LV_PCT(100));
        lv_obj_set_style_text_font(ok_label, &montserrat_24_latin, LV_PART_MAIN);
        lv_obj_set_style_text_color(ok_label, COLOR_SUCCESS, LV_PART_MAIN);
        lv_obj_set_style_text_align(ok_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        
        if (state.error_count_label) {
            lv_label_set_text(state.error_count_label, "No Errors");
        }
    } else {
        // Create a card for each error
        for (int i = 0; i < count; i++) {
            create_error_card(state.error_list, &errors[i]);
        }
        
        // Update count label
        if (state.error_count_label) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d Active Error%s", count, count == 1 ? "" : "s");
            lv_label_set_text(state.error_count_label, buf);
            
            // Color based on highest severity
            arctic::ErrorSeverity highest = arctic::getHighestSeverity();
            lv_obj_set_style_text_color(state.error_count_label, severity_to_color(highest), LV_PART_MAIN);
        }
    }
}

static void update_timer_cb(lv_timer_t* timer) {
    (void)timer;
    if (state.shown) {
        update_error_list();
    }
}

static void back_btn_cb(lv_event_t* e) {
    (void)e;
    heatpump_errors_hide();
}

// ============================================================================
// Public Functions
// ============================================================================

void heatpump_errors_show(heatpump_errors_close_cb_t on_close) {
    if (state.shown) return;
    
    state.on_close = on_close;
    state.shown = true;
    
    // Create screen
    state.screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(state.screen, COLOR_BG, LV_PART_MAIN);
    
    // Header bar
    lv_obj_t* header = lv_obj_create(state.screen);
    lv_obj_set_size(header, LV_PCT(100), 70);
    lv_obj_set_style_bg_color(header, COLOR_HEADER_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    
    // Back button
    lv_obj_t* back_btn = lv_btn_create(header);
    lv_obj_set_size(back_btn, 100, 50);
    lv_obj_set_style_bg_color(back_btn, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_radius(back_btn, 8, LV_PART_MAIN);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_CLICKED, nullptr);
    
    lv_obj_t* back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_font(back_lbl, &montserrat_16_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(back_lbl, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_center(back_lbl);
    
    // Title
    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "Error Status");
    lv_obj_set_style_text_font(title, &montserrat_24_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
    
    // Error count label
    state.error_count_label = lv_label_create(header);
    lv_label_set_text(state.error_count_label, "...");
    lv_obj_set_style_text_font(state.error_count_label, &montserrat_16_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.error_count_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(state.error_count_label, LV_ALIGN_RIGHT_MID, -15, 0);
    
    // Scrollable content area
    lv_obj_t* content = lv_obj_create(state.screen);
    lv_obj_set_size(content, LV_PCT(100), lv_pct(100));
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_top(content, 80, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(content, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(content, 15, LV_PART_MAIN);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_layout(content, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(content, 15, LV_PART_MAIN);
    
    // Error list container
    state.error_list = content;
    
    // Initial update
    update_error_list();
    
    // Create update timer (2 second interval)
    state.update_timer = lv_timer_create(update_timer_cb, 2000, nullptr);
    
    // Load screen with animation
    lv_screen_load_anim(state.screen, LV_SCR_LOAD_ANIM_MOVE_TOP, 400, 0, false);
    
    ESP_LOGI(TAG, "Error details screen shown");
}

void heatpump_errors_hide(void) {
    if (!state.shown) return;
    
    if (state.update_timer) {
        lv_timer_delete(state.update_timer);
        state.update_timer = nullptr;
    }
    
    // Call close callback which will handle screen transition
    if (state.on_close) {
        state.on_close();
    }
    
    state.shown = false;
    state.error_list = nullptr;
    state.error_count_label = nullptr;
    
    ESP_LOGI(TAG, "Error details screen hidden");
}

bool heatpump_errors_is_shown(void) {
    return state.shown;
}
