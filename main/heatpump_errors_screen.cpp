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
#include "time_manager.h"
#include "i18n/i18n.h"
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

// Toggle resolution visibility when card is tapped
static void error_card_tap_cb(lv_event_t* e) {
    lv_obj_t* card = (lv_obj_t*)lv_event_get_target(e);
    lv_obj_t* resolution_cont = (lv_obj_t*)lv_event_get_user_data(e);
    
    if (resolution_cont) {
        bool is_hidden = lv_obj_has_flag(resolution_cont, LV_OBJ_FLAG_HIDDEN);
        if (is_hidden) {
            lv_obj_clear_flag(resolution_cont, LV_OBJ_FLAG_HIDDEN);
            // Update border to show expanded state
            lv_obj_set_style_border_width(card, 3, LV_PART_MAIN);
        } else {
            lv_obj_add_flag(resolution_cont, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
        }
    }
}

static lv_obj_t* create_error_card(lv_obj_t* parent, const arctic::ActiveError* error) {
    lv_color_t severity_color = severity_to_color(error->severity);
    
    // Card container - active errors have more prominent styling
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    
    if (error->active) {
        // Active error: prominent background with full border
        lv_obj_set_style_bg_color(card, lv_color_hex(0x2a1a1a), LV_PART_MAIN);  // Dark red tint
        lv_obj_set_style_border_width(card, 3, LV_PART_MAIN);
        lv_obj_set_style_border_color(card, severity_color, LV_PART_MAIN);
        lv_obj_set_style_border_side(card, LV_BORDER_SIDE_FULL, LV_PART_MAIN);
    } else {
        // Cleared error: normal card styling with left border only
        lv_obj_set_style_bg_color(card, COLOR_CARD_BG, LV_PART_MAIN);
        lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(card, lv_color_hex(0x444444), LV_PART_MAIN);  // Dimmer border
        lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
    }
    
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
    
    // Error code (e.g., "P02") with tap hint
    lv_obj_t* code_label = lv_label_create(top_row);
    char code_buf[32];
    snprintf(code_buf, sizeof(code_buf), "%s %s  " LV_SYMBOL_DOWN, severity_to_icon(error->severity), error->code);
    lv_label_set_text(code_label, code_buf);
    lv_obj_set_style_text_font(code_label, &montserrat_32_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(code_label, severity_color, LV_PART_MAIN);
    lv_obj_align(code_label, LV_ALIGN_LEFT_MID, 0, 0);
    
    // Severity label
    lv_obj_t* severity_label = lv_label_create(top_row);
    lv_label_set_text(severity_label, arctic::severityToString(error->severity));
    lv_obj_set_style_text_font(severity_label, &montserrat_24_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(severity_label, severity_color, LV_PART_MAIN);
    lv_obj_align(severity_label, LV_ALIGN_RIGHT_MID, 0, 0);
    
    // Description (no snake_case name - just the human readable description)
    lv_obj_t* desc_label = lv_label_create(card);
    lv_label_set_text(desc_label, error->description);
    lv_label_set_long_mode(desc_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc_label, LV_PCT(100));
    lv_obj_set_style_text_font(desc_label, &montserrat_24_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(desc_label, COLOR_TEXT, LV_PART_MAIN);
    
    // Duration row (how long error has been active or was active)
    lv_obj_t* duration_label = lv_label_create(card);
    char duration_buf[96];
    
    // Format the start time as date/time (respect 12/24h setting)
    char time_str[32] = "Unknown";
    if (error->first_seen > 0) {
        struct tm* tm_info = localtime(&error->first_seen);
        if (tm_info) {
            const char* fmt = time_mgr_get_24h_format() ? "%b %d, %H:%M" : "%b %d, %I:%M %p";
            strftime(time_str, sizeof(time_str), fmt, tm_info);  // e.g., "Feb 09, 14:35" or "Feb 09, 2:35 PM"
        }
    }
    
    if (error->active && error->first_seen > 0) {
        // Error is still active - show start time and duration
        snprintf(duration_buf, sizeof(duration_buf), LV_SYMBOL_REFRESH " %s %s | %s %s", 
                 i18n_get(STR_HP_STARTED), time_str, i18n_get(STR_HP_ACTIVE_FOR),
                 arctic::formatDuration(error->first_seen, 0));
    } else if (error->first_seen > 0) {
        // Error was cleared - show start time and duration
        snprintf(duration_buf, sizeof(duration_buf), LV_SYMBOL_OK " %s %s | %s %s", 
                 i18n_get(STR_HP_STARTED), time_str, i18n_get(STR_HP_DURATION),
                 arctic::formatDuration(error->first_seen, error->last_seen));
    } else {
        snprintf(duration_buf, sizeof(duration_buf), "%s", i18n_get(STR_HP_JUST_DETECTED));
    }
    lv_label_set_text(duration_label, duration_buf);
    lv_obj_set_style_text_font(duration_label, &montserrat_24_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(duration_label, error->active ? COLOR_WARNING : COLOR_TEXT_DIM, LV_PART_MAIN);
    
    // Resolution container (hidden by default, shown on tap)
    lv_obj_t* resolution_cont = lv_obj_create(card);
    lv_obj_set_size(resolution_cont, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(resolution_cont, lv_color_hex(0x0f3460), LV_PART_MAIN);
    lv_obj_set_style_border_width(resolution_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(resolution_cont, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(resolution_cont, 16, LV_PART_MAIN);
    lv_obj_set_layout(resolution_cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(resolution_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(resolution_cont, 8, LV_PART_MAIN);
    lv_obj_clear_flag(resolution_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(resolution_cont, LV_OBJ_FLAG_HIDDEN);  // Hidden by default
    
    // Resolution header
    lv_obj_t* res_header = lv_label_create(resolution_cont);
    lv_label_set_text(res_header, i18n_get(STR_HP_RESOLUTION));
    lv_obj_set_style_text_font(res_header, &montserrat_24_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(res_header, COLOR_ACCENT, LV_PART_MAIN);
    
    // Resolution text
    lv_obj_t* res_label = lv_label_create(resolution_cont);
    lv_label_set_text(res_label, error->resolution ? error->resolution : i18n_get(STR_HP_CONTACT_DEALER));
    lv_label_set_long_mode(res_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(res_label, LV_PCT(100));
    lv_obj_set_style_text_font(res_label, &montserrat_24_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(res_label, COLOR_TEXT, LV_PART_MAIN);
    
    // Make card clickable to expand/collapse resolution
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, error_card_tap_cb, LV_EVENT_CLICKED, resolution_cont);
    
    return card;
}

static void update_error_list() {
    if (!state.error_list) return;
    
    // Clear existing error cards
    lv_obj_clean(state.error_list);
    
    // Get active errors and connection state
    arctic::ActiveError errors[32];
    int count = arctic::getActiveErrors(errors, 32);
    arctic::HeatPumpState hp = arctic::getState();
    
    // Check demo mode
    bool demo_mode = app_prefs_is_demo_mode();
    
    if (demo_mode) {
        // Show sample demo errors to demonstrate UI with Arctic error codes
        // Use current time minus some offset for demo durations
        time_t now = time(nullptr);
        
        // Create demo errors with realistic timestamps
        arctic::ActiveError demo_errors[3];
        
        // P02 - High pressure, ACTIVE for 2 hours 15 minutes (still ongoing)
        demo_errors[0] = { "P02", "HIGH_PRESSURE", "High pressure protection activated",
            "1) Check whether the water temperature is too high or blocked. 2) Check whether the fan blades are blocked or if evaporator fins are blocked. 3) Check whether snow or ice has built up inside the unit. 4) Check that the water tank temperature setting is not too high.",
            arctic::ErrorSeverity::CRITICAL, 2, 0x0040, now - 8100, now, true };
        
        // E26 - Low ambient, CLEARED after 45 minutes (was active, now resolved)
        demo_errors[1] = { "E26", "LOW_AMBIENT", "Low ambient temperature protection",
            "Ambient temperature is too low for operation. Wait for conditions to improve.",
            arctic::ErrorSeverity::WARNING, 2, 0x0400, now - 3600, now - 900, false };  // Started 1h ago, cleared 15m ago
        
        // E19 - Inlet sensor, CLEARED after 12 minutes
        demo_errors[2] = { "E19", "INLET_SENS", "Inlet water temperature sensor fault",
            "Check the inlet water temperature sensor at the heat exchanger for a short or open circuit and correct or replace.",
            arctic::ErrorSeverity::ERROR, 1, 0x0004, now - 1800, now - 1080, false };  // Started 30m ago, cleared 18m ago (duration: 12m)
        
        for (int i = 0; i < 3; i++) {
            create_error_card(state.error_list, &demo_errors[i]);
        }
        return;
    }
    
    // Check if disconnected
    if (!hp.connected) {
        lv_obj_t* disconn_card = lv_obj_create(state.error_list);
        lv_obj_set_size(disconn_card, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(disconn_card, COLOR_CARD_BG, LV_PART_MAIN);
        lv_obj_set_style_border_width(disconn_card, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(disconn_card, COLOR_WARNING, LV_PART_MAIN);
        lv_obj_set_style_border_side(disconn_card, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
        lv_obj_set_style_radius(disconn_card, 12, LV_PART_MAIN);
        lv_obj_set_style_pad_all(disconn_card, 25, LV_PART_MAIN);
        lv_obj_clear_flag(disconn_card, LV_OBJ_FLAG_SCROLLABLE);
        
        lv_obj_t* disconn_label = lv_label_create(disconn_card);
        lv_label_set_text(disconn_label, i18n_get(STR_HP_DISCONNECTED_MSG));
        lv_label_set_long_mode(disconn_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(disconn_label, LV_PCT(100));
        lv_obj_set_style_text_font(disconn_label, UI_FONT_BODY, LV_PART_MAIN);
        lv_obj_set_style_text_color(disconn_label, COLOR_WARNING, LV_PART_MAIN);
        lv_obj_set_style_text_align(disconn_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
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
        lv_label_set_text(ok_label, i18n_get(STR_HP_NO_ERRORS));
        lv_label_set_long_mode(ok_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(ok_label, LV_PCT(100));
        lv_obj_set_style_text_font(ok_label, UI_FONT_BODY, LV_PART_MAIN);
        lv_obj_set_style_text_color(ok_label, COLOR_SUCCESS, LV_PART_MAIN);
        lv_obj_set_style_text_align(ok_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    } else {
        // Create a card for each error
        for (int i = 0; i < count; i++) {
            create_error_card(state.error_list, &errors[i]);
        }
    }
}

static void back_btn_cb(lv_event_t* e) {
    (void)e;
    ESP_LOGI(TAG, "Back button clicked");
    
    // Stop timer first to prevent use-after-free
    if (state.update_timer) {
        lv_timer_del(state.update_timer);
        state.update_timer = nullptr;
    }
    
    // Save and clear callback
    heatpump_errors_close_cb_t cb = state.on_close;
    state.on_close = nullptr;
    state.shown = false;
    state.screen = nullptr;
    state.error_list = nullptr;
    
    // Call callback - it will load the previous screen with auto_del=true
    if (cb) {
        cb();
    }
}

// ============================================================================
// Public Functions
// ============================================================================

void heatpump_errors_show(heatpump_errors_close_cb_t on_close) {
    if (state.shown) return;
    
    ESP_LOGI(TAG, "Showing error details screen");
    state.on_close = on_close;
    state.shown = true;
    
    // Create screen
    state.screen = lv_obj_create(NULL);
    lv_obj_set_size(state.screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(state.screen, COLOR_BG, LV_PART_MAIN);
    lv_obj_clear_flag(state.screen, LV_OBJ_FLAG_SCROLLABLE);
    
    // Header (8% height)
    lv_obj_t* header = lv_obj_create(state.screen);
    lv_obj_set_size(header, LV_PCT(100), LV_PCT(8));
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, COLOR_HEADER_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(header, 15, LV_PART_MAIN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    
    // Close button (X on right) with circular background - matches system screen
    lv_obj_t* back_btn = lv_btn_create(header);
    lv_obj_set_size(back_btn, 50, 50);
    lv_obj_align(back_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x3d4f6f), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(back_btn, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(back_btn, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_border_opa(back_btn, LV_OPA_50, LV_PART_MAIN);
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_CLICKED, nullptr);
    
    lv_obj_t* back_icon = lv_label_create(back_btn);
    lv_label_set_text(back_icon, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(back_icon, UI_FONT_ICON, LV_PART_MAIN);
    lv_obj_set_style_text_color(back_icon, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_center(back_icon);
    
    // Title
    lv_obj_t* title = lv_label_create(header);
    if (app_prefs_is_demo_mode()) {
        lv_label_set_text(title, i18n_get(STR_HP_DEMO_ERRORS));
        lv_obj_set_style_text_color(title, COLOR_WARNING, LV_PART_MAIN);
    } else {
        lv_label_set_text(title, i18n_get(STR_HP_ERROR_STATUS));
        lv_obj_set_style_text_color(title, COLOR_TEXT, LV_PART_MAIN);
    }
    lv_obj_set_style_text_font(title, UI_FONT_HEADER, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
    
    // Note: Error count label removed - visual distinction between active/cleared is sufficient
    
    // Scrollable content (92% height)
    lv_obj_t* content = lv_obj_create(state.screen);
    lv_obj_set_size(content, LV_PCT(100), LV_PCT(92));
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(content, 20, LV_PART_MAIN);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 15, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
    
    // Error list container
    state.error_list = content;
    
    // Initial update (no auto-refresh timer - errors don't change frequently
    // and rebuilding would collapse any expanded cards)
    update_error_list();
    
    // Load screen with animation
    lv_screen_load_anim(state.screen, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, false);
    
    ESP_LOGI(TAG, "Error details screen shown");
}

void heatpump_errors_hide(void) {
    if (!state.shown) return;
    
    if (state.update_timer) {
        lv_timer_del(state.update_timer);
        state.update_timer = nullptr;
    }
    
    // Save and clear callback
    heatpump_errors_close_cb_t cb = state.on_close;
    state.on_close = nullptr;
    state.shown = false;
    state.error_list = nullptr;
    
    // Call callback - it will load the previous screen with auto_del=true
    if (cb) {
        cb();
    }
    
    ESP_LOGI(TAG, "Error details screen hidden");
}

bool heatpump_errors_is_shown(void) {
    return state.shown;
}
