/*
 * Arctic Heat Pump Controller
 * Heat Pump Status Display Screen
 */

#include "heatpump_screen.h"
#include "heatpump_temps_screen.h"
#include "heatpump_system_screen.h"
#include "heatpump_control_screen.h"
#include "heatpump_errors_screen.h"
#include "heatpump_errors.h"
#include "modbus/arctic_heatpump.h"
#include "modbus/arctic_registers.h"
#include "app_preferences.h"
#include "i18n/i18n.h"
#include "ui_common.h"
#include <esp_log.h>
#include <stdio.h>
#include <string.h>

static const char* TAG = "hp_screen";

// Format error summary for the main screen error card.
// Shows first error with code, truncates if needed, appends "+ N more" for additional errors.
// max_width_px is the usable pixel width of the label.
static void format_error_card_text(char* buf, size_t buf_size, const lv_font_t* font, lv_coord_t max_width_px) {
    arctic::ActiveError errors[16];
    int count = arctic::getActiveErrors(errors, 16);
    if (count <= 0) {
        buf[0] = '\0';
        return;
    }
    
    // Build first error: "P02: High pressure protection"
    char first[128];
    snprintf(first, sizeof(first), "\xEF\x81\xB1 %s: %s", errors[0].code, errors[0].description);
    
    if (count == 1) {
        // Single error — truncate with ellipsis if too wide
        lv_point_t size;
        lv_txt_get_size(&size, first, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x <= max_width_px) {
            strncpy(buf, first, buf_size - 1);
            buf[buf_size - 1] = '\0';
        } else {
            // Truncate with "..."
            for (int len = (int)strlen(first) - 1; len > 10; len--) {
                first[len] = '\0';
                char trial[140];
                snprintf(trial, sizeof(trial), "%s...", first);
                lv_txt_get_size(&size, trial, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                if (size.x <= max_width_px) {
                    strncpy(buf, trial, buf_size - 1);
                    buf[buf_size - 1] = '\0';
                    return;
                }
            }
            strncpy(buf, first, buf_size - 1);
            buf[buf_size - 1] = '\0';
        }
        return;
    }
    
    // Multiple errors — need "+ N more" suffix
    char suffix[24];
    snprintf(suffix, sizeof(suffix), " + %d more", count - 1);
    
    // Measure suffix width
    lv_point_t suffix_size;
    lv_txt_get_size(&suffix_size, suffix, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_coord_t available = max_width_px - suffix_size.x;
    
    // Check if first error fits with suffix
    lv_point_t first_size;
    lv_txt_get_size(&first_size, first, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    
    if (first_size.x <= available) {
        snprintf(buf, buf_size, "%s%s", first, suffix);
    } else {
        // Truncate first error to fit
        for (int len = (int)strlen(first) - 1; len > 10; len--) {
            first[len] = '\0';
            char trial[140];
            snprintf(trial, sizeof(trial), "%s...", first);
            lv_point_t trial_size;
            lv_txt_get_size(&trial_size, trial, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            if (trial_size.x <= available) {
                snprintf(buf, buf_size, "%s%s", trial, suffix);
                return;
            }
        }
        // Fallback: just show code + suffix
        snprintf(buf, buf_size, "\xEF\x81\xB1 %s%s", errors[0].code, suffix);
    }
}

// ============================================================================
// UI Elements
// ============================================================================
static struct {
    bool created = false;
    lv_obj_t* container = nullptr;
    
    // Hero state card (color-coded background)
    lv_obj_t* hero_card = nullptr;
    lv_obj_t* hero_state_label = nullptr;
    lv_obj_t* hero_tank_label = nullptr;
    lv_obj_t* hero_tank_desc = nullptr;
    
    // Performance strip (COP | Power | Fan)
    lv_obj_t* perf_card = nullptr;
    lv_obj_t* perf_cop_value = nullptr;
    lv_obj_t* perf_power_value = nullptr;
    lv_obj_t* perf_fan_value = nullptr;
    
    // Component dots row (Comp | Fan | Pump | Aux)
    lv_obj_t* comp_dot = nullptr;
    lv_obj_t* comp_dot_label = nullptr;
    lv_obj_t* fan_dot = nullptr;
    lv_obj_t* fan_dot_label = nullptr;
    lv_obj_t* pump_dot = nullptr;
    lv_obj_t* pump_dot_label = nullptr;
    lv_obj_t* heater_dot = nullptr;
    lv_obj_t* heater_dot_label = nullptr;
    
    // Error card
    lv_obj_t* error_card = nullptr;
    lv_obj_t* error_label = nullptr;
    lv_obj_t* error_chevron = nullptr;
    
    // Expandable: Temperatures
    bool temps_expanded = false;
    lv_obj_t* temps_chevron = nullptr;
    lv_obj_t* temps_content = nullptr;
    lv_obj_t* inlet_value = nullptr;
    lv_obj_t* outlet_value = nullptr;
    lv_obj_t* ambient_value = nullptr;
    lv_obj_t* coil_value = nullptr;
    
    // Expandable: Compressor Details
    bool comp_expanded = false;
    lv_obj_t* comp_chevron = nullptr;
    lv_obj_t* comp_content = nullptr;
    lv_obj_t* comp_status_label = nullptr;
    lv_obj_t* comp_freq_bar = nullptr;
    lv_obj_t* comp_freq_label = nullptr;
    lv_obj_t* comp_discharge_value = nullptr;
    lv_obj_t* comp_suction_value = nullptr;
    lv_obj_t* comp_eev_value = nullptr;
    lv_obj_t* comp_hi_press_value = nullptr;
    lv_obj_t* comp_lo_press_value = nullptr;
    lv_obj_t* comp_dt_value = nullptr;
    lv_obj_t* comp_dt_label = nullptr;
    
    // Expandable: Energy
    bool energy_expanded = false;
    lv_obj_t* energy_chevron = nullptr;
    lv_obj_t* energy_content = nullptr;
    lv_obj_t* energy_in_value = nullptr;
    lv_obj_t* energy_out_value = nullptr;
    lv_obj_t* energy_cop_value = nullptr;
    
    // Bottom button bar
    lv_obj_t* temps_btn = nullptr;
    lv_obj_t* system_btn = nullptr;
    lv_obj_t* controls_btn = nullptr;
    lv_obj_t* temps_btn_label = nullptr;
    lv_obj_t* system_btn_label = nullptr;
    lv_obj_t* controls_btn_label = nullptr;
    
    // Saved screen for returning from details/controls
    lv_obj_t* saved_screen = nullptr;
    
    // Update timer
    lv_timer_t* update_timer = nullptr;
} state;

// ============================================================================
// Colors
// ============================================================================
#define COLOR_CARD_BG       lv_color_hex(0x16213e)
#define COLOR_CARD_BORDER   lv_color_hex(0x0f3460)
#define COLOR_TEXT          lv_color_hex(0xeaeaea)
#define COLOR_TEXT_DIM      lv_color_hex(0x888888)
#define COLOR_ACCENT        lv_color_hex(0x00d4ff)
#define COLOR_SUCCESS       lv_color_hex(0x4ade80)
#define COLOR_WARNING       lv_color_hex(0xfbbf24)
#define COLOR_ERROR         lv_color_hex(0xef4444)
#define COLOR_INACTIVE      lv_color_hex(0x444444)

// Mode colors
#define COLOR_HEATING       lv_color_hex(0xf97316)  // Orange
#define COLOR_COOLING       lv_color_hex(0x3b82f6)  // Blue
#define COLOR_HOT_WATER     lv_color_hex(0xef4444)  // Red
#define COLOR_DEFROST       lv_color_hex(0x8b5cf6)  // Purple

// ============================================================================
// Helper Functions
// ============================================================================

static const char* getModeText(arctic::WorkingMode mode, bool defrosting) {
    if (defrosting) {
        return i18n_get(STR_HP_MODE_DEFROST);
    }
    switch (mode) {
        case arctic::WorkingMode::COOLING:           return i18n_get(STR_HP_MODE_COOLING);
        case arctic::WorkingMode::FLOOR_HEATING:     return i18n_get(STR_HP_MODE_FLOOR_HEAT);
        case arctic::WorkingMode::FAN_COIL_HEATING:  return i18n_get(STR_HP_MODE_FAN_HEAT);
        case arctic::WorkingMode::HOT_WATER:         return i18n_get(STR_HP_MODE_HOT_WATER);
        case arctic::WorkingMode::AUTO:              return i18n_get(STR_HP_MODE_AUTO);
        default:                                      return i18n_get(STR_HP_MODE_UNKNOWN);
    }
}

// Create a value column (number on top, label below) for dashboard cards
static lv_obj_t* create_value_column(lv_obj_t* parent, const char* label_text,
                                      lv_obj_t** value_out, const lv_font_t* value_font = UI_FONT_TITLE) {
    lv_obj_t* col = lv_obj_create(parent);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(col, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(col, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(col, 2, LV_PART_MAIN);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    // Value on top (large)
    lv_obj_t* val = lv_label_create(col);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_font(val, value_font, LV_PART_MAIN);
    lv_obj_set_style_text_color(val, COLOR_TEXT, LV_PART_MAIN);
    
    // Label below (small)
    lv_obj_t* lbl = lv_label_create(col);
    lv_label_set_text(lbl, label_text);
    lv_obj_set_style_text_font(lbl, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, COLOR_TEXT_DIM, LV_PART_MAIN);
    
    *value_out = val;
    return col;
}

static void set_indicator_active(lv_obj_t* indicator, bool active, lv_color_t active_color) {
    if (indicator) {
        lv_obj_set_style_bg_color(indicator, active ? active_color : COLOR_INACTIVE, LV_PART_MAIN);
    }
}

// ============================================================================
// Hero State Logic
// ============================================================================

enum class HeroState {
    DISCONNECTED,
    FAULT,
    STANDBY,
    DEFROST,
    HEATING,
    COOLING,
    HOT_WATER,
    IDLE
};

static HeroState getHeroState(const arctic::HeatPumpState& hp) {
    if (!hp.connected) return HeroState::DISCONNECTED;
    if (hp.hasAnyError()) return HeroState::FAULT;
    if (!hp.unit_on) return HeroState::STANDBY;
    if (hp.isDefrosting()) return HeroState::DEFROST;
    if (hp.isCompressorRunning()) {
        switch (hp.working_mode) {
            case arctic::WorkingMode::COOLING: return HeroState::COOLING;
            case arctic::WorkingMode::HOT_WATER: return HeroState::HOT_WATER;
            default: return HeroState::HEATING;
        }
    }
    return HeroState::IDLE;
}

static lv_color_t getHeroBgColor(HeroState s) {
    switch (s) {
        case HeroState::HEATING:
            return lv_color_mix(COLOR_HEATING, COLOR_CARD_BG, 64);   // 25% orange tint
        case HeroState::COOLING:
            return lv_color_mix(COLOR_COOLING, COLOR_CARD_BG, 64);   // 25% blue tint
        case HeroState::HOT_WATER:
            return lv_color_mix(COLOR_HOT_WATER, COLOR_CARD_BG, 50); // 20% red tint
        case HeroState::DEFROST:
            return lv_color_mix(COLOR_DEFROST, COLOR_CARD_BG, 64);   // 25% purple tint
        case HeroState::FAULT:
            return lv_color_mix(COLOR_ERROR, COLOR_CARD_BG, 96);     // 37% deep red
        default: // DISCONNECTED, IDLE, STANDBY
            return COLOR_CARD_BG;
    }
}

static lv_color_t getHeroBorderColor(HeroState s) {
    switch (s) {
        case HeroState::DISCONNECTED:
            return COLOR_ERROR;
        case HeroState::FAULT:
            return COLOR_ERROR;
        case HeroState::HEATING:
            return COLOR_HEATING;
        case HeroState::COOLING:
            return COLOR_COOLING;
        case HeroState::HOT_WATER:
            return COLOR_HOT_WATER;
        case HeroState::DEFROST:
            return COLOR_DEFROST;
        default:
            return COLOR_CARD_BORDER;
    }
}

static const char* getHeroStateText(HeroState s, const arctic::HeatPumpState& hp) {
    switch (s) {
        case HeroState::DISCONNECTED: return i18n_get(STR_HP_DISCONNECTED);
        case HeroState::FAULT:        return i18n_get(STR_HP_STATE_FAULT);
        case HeroState::STANDBY:      return i18n_get(STR_HP_STANDBY);
        case HeroState::DEFROST:      return i18n_get(STR_HP_MODE_DEFROST);
        case HeroState::COOLING:      return i18n_get(STR_HP_MODE_COOLING);
        case HeroState::HOT_WATER:    return i18n_get(STR_HP_MODE_HOT_WATER);
        case HeroState::HEATING:      return getModeText(hp.working_mode, false);
        case HeroState::IDLE:         return i18n_get(STR_HP_COMP_IDLE);
    }
    return "---";
}

static lv_color_t getHeroTextColor(HeroState s) {
    switch (s) {
        case HeroState::DISCONNECTED: return COLOR_ERROR;
        case HeroState::FAULT:        return COLOR_ERROR;
        case HeroState::STANDBY:      return COLOR_TEXT_DIM;
        case HeroState::IDLE:         return COLOR_TEXT_DIM;
        default:                       return COLOR_TEXT;
    }
}

// ============================================================================
// Expandable Panel Helper
// ============================================================================

static void toggle_panel(lv_obj_t* content, lv_obj_t* chevron, bool& expanded) {
    expanded = !expanded;
    if (expanded) {
        lv_obj_clear_flag(content, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(chevron, LV_SYMBOL_DOWN);
    } else {
        lv_obj_add_flag(content, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
    }
}

static void temps_panel_cb(lv_event_t* e) {
    (void)e;
    toggle_panel(state.temps_content, state.temps_chevron, state.temps_expanded);
}

static void comp_panel_cb(lv_event_t* e) {
    (void)e;
    toggle_panel(state.comp_content, state.comp_chevron, state.comp_expanded);
}

static void energy_panel_cb(lv_event_t* e) {
    (void)e;
    toggle_panel(state.energy_content, state.energy_chevron, state.energy_expanded);
}

// Create an expandable panel with header + hidden content area.
// Returns the content container to populate with widgets.
// Stores chevron pointer in *chevron_out for state tracking.
static lv_obj_t* create_expandable_panel(lv_obj_t* parent, const char* title,
                                          lv_obj_t** chevron_out, lv_obj_t** content_out,
                                          lv_event_cb_t toggle_cb) {
    // Outer panel
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_size(panel, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(panel, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, COLOR_CARD_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, 0, LV_PART_MAIN);
    
    // Header row (clickable)
    lv_obj_t* header = lv_obj_create(panel);
    lv_obj_set_size(header, LV_PCT(100), 70);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x1e2d4e), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(header, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_right(header, 16, LV_PART_MAIN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(header, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(header, toggle_cb, LV_EVENT_CLICKED, nullptr);
    
    lv_obj_t* lbl = lv_label_create(header);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    
    lv_obj_t* chevron = lv_label_create(header);
    lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(chevron, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(chevron, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, 0, 0);
    *chevron_out = chevron;
    
    // Content area (hidden initially)
    lv_obj_t* content = lv_obj_create(panel);
    lv_obj_set_size(content, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(content, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_top(content, 0, LV_PART_MAIN);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(content, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 8, LV_PART_MAIN);
    *content_out = content;
    
    return panel;
}

static void on_temps_close(void) {
    // Load saved screen back with fade animation
    // auto_del=true - LVGL will delete the sub-screen after animation
    if (state.saved_screen) {
        lv_screen_load_anim(state.saved_screen, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, true);
        state.saved_screen = nullptr;
    }
}

static void temps_btn_cb(lv_event_t* e) {
    // Save current screen
    state.saved_screen = lv_scr_act();
    
    // Show temperatures screen
    heatpump_temps_show(on_temps_close);
}

static void on_system_close(void) {
    // Load saved screen back with fade animation
    // auto_del=true - LVGL will delete the sub-screen after animation
    if (state.saved_screen) {
        lv_screen_load_anim(state.saved_screen, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, true);
        state.saved_screen = nullptr;
    }
}

static void system_btn_cb(lv_event_t* e) {
    // Save current screen
    state.saved_screen = lv_scr_act();
    
    // Show system readings screen
    heatpump_system_show(on_system_close);
}

static void on_errors_close(void) {
    // Load saved screen back with fade animation
    // auto_del=true - LVGL will delete the sub-screen after animation
    if (state.saved_screen) {
        lv_screen_load_anim(state.saved_screen, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, true);
        state.saved_screen = nullptr;
    }
}

static void error_card_cb(lv_event_t* e) {
    (void)e;
    // Save current screen
    state.saved_screen = lv_scr_act();
    
    // Show error details screen
    heatpump_errors_show(on_errors_close);
}

static void on_control_close(void) {
    // Load saved screen back with fade animation
    // auto_del=true - LVGL will delete the sub-screen after animation
    if (state.saved_screen) {
        lv_screen_load_anim(state.saved_screen, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, true);
        state.saved_screen = nullptr;
    }
}

static void controls_btn_cb(lv_event_t* e) {
    // Save current screen
    state.saved_screen = lv_scr_act();
    
    // Show control screen
    heatpump_control_show(on_control_close);
}

// ============================================================================
// Timer Callback
// ============================================================================

static void update_timer_cb(lv_timer_t* timer) {
    heatpump_screen_update();
}

// ============================================================================
// Public Functions
// ============================================================================

void heatpump_screen_create(lv_obj_t* parent, int y_offset) {
    if (state.created) {
        ESP_LOGW(TAG, "Screen already created");
        return;
    }
    
    ESP_LOGI(TAG, "Creating heat pump status display v2");
    
    // Fixed footer height for nav buttons
    static const int FOOTER_H = 84;
    
    // Main container - scrollable flex column (leaves room for footer)
    state.container = lv_obj_create(parent);
    lv_obj_set_size(state.container, 700, 1280 - y_offset - 40 - FOOTER_H);
    lv_obj_align(state.container, LV_ALIGN_TOP_MID, 0, y_offset);
    lv_obj_set_style_bg_opa(state.container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.container, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(state.container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(state.container, 12, LV_PART_MAIN);
    lv_obj_set_flex_align(state.container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(state.container, LV_SCROLLBAR_MODE_AUTO);
    
    // =========================================================================
    // DEMO MODE BANNER (shown only in demo mode)
    // =========================================================================
    if (app_prefs_is_demo_mode()) {
        lv_obj_t* demo_banner = lv_obj_create(state.container);
        lv_obj_set_size(demo_banner, LV_PCT(100), 40);
        lv_obj_set_style_bg_color(demo_banner, COLOR_WARNING, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(demo_banner, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(demo_banner, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(demo_banner, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_all(demo_banner, 0, LV_PART_MAIN);
        lv_obj_clear_flag(demo_banner, LV_OBJ_FLAG_SCROLLABLE);
        
        lv_obj_t* demo_label = lv_label_create(demo_banner);
        lv_label_set_text(demo_label, i18n_get(STR_HP_DEMO_MODE_ENABLED));
        lv_obj_set_style_text_font(demo_label, UI_FONT_BODY, LV_PART_MAIN);
        lv_obj_set_style_text_color(demo_label, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_center(demo_label);
    }
    
    // =========================================================================
    // HERO STATE CARD: Color-coded background, state text, tank temperature
    // Readable from across the room — primary system state indicator
    // =========================================================================
    state.hero_card = lv_obj_create(state.container);
    lv_obj_set_size(state.hero_card, LV_PCT(100), 200);
    lv_obj_set_style_bg_color(state.hero_card, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state.hero_card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(state.hero_card, COLOR_CARD_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.hero_card, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(state.hero_card, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.hero_card, 15, LV_PART_MAIN);
    lv_obj_clear_flag(state.hero_card, LV_OBJ_FLAG_SCROLLABLE);
    
    // State text: "HEATING", "COOLING", "IDLE", "FAULT", etc.
    state.hero_state_label = lv_label_create(state.hero_card);
    lv_label_set_text(state.hero_state_label, i18n_get(STR_HP_DISCONNECTED));
    lv_obj_set_style_text_font(state.hero_state_label, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.hero_state_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(state.hero_state_label, LV_ALIGN_CENTER, 0, -40);
    
    // Tank temperature (large)
    state.hero_tank_label = lv_label_create(state.hero_card);
    lv_label_set_text(state.hero_tank_label, "--");
    lv_obj_set_style_text_font(state.hero_tank_label, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.hero_tank_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(state.hero_tank_label, LV_ALIGN_CENTER, 0, 10);
    
    // "Tank Temperature" descriptor
    state.hero_tank_desc = lv_label_create(state.hero_card);
    lv_label_set_text(state.hero_tank_desc, i18n_get(STR_HP_TANK_TEMPERATURE));
    lv_obj_set_style_text_font(state.hero_tank_desc, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.hero_tank_desc, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(state.hero_tank_desc, LV_ALIGN_CENTER, 0, 45);
    
    // =========================================================================
    // COMPONENT DOTS: Comp | Fan | Pump | Aux Heat
    // =========================================================================
    lv_obj_t* dots_row = lv_obj_create(state.container);
    lv_obj_set_size(dots_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(dots_row, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_border_color(dots_row, COLOR_CARD_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dots_row, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(dots_row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dots_row, 12, LV_PART_MAIN);
    lv_obj_clear_flag(dots_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(dots_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    // Helper lambda: dot above label, both centered in a column
    auto make_dot = [&](lv_obj_t* parent_row, const char* text, lv_obj_t** dot_out, lv_obj_t** label_out) {
        lv_obj_t* cont = lv_obj_create(parent_row);
        lv_obj_set_size(cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(cont, 4, LV_PART_MAIN);
        lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(cont, 6, LV_PART_MAIN);
        lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        
        *dot_out = lv_obj_create(cont);
        lv_obj_set_size(*dot_out, 18, 18);
        lv_obj_set_style_radius(*dot_out, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(*dot_out, COLOR_INACTIVE, LV_PART_MAIN);
        lv_obj_set_style_border_width(*dot_out, 0, LV_PART_MAIN);
        lv_obj_clear_flag(*dot_out, LV_OBJ_FLAG_SCROLLABLE);
        
        *label_out = lv_label_create(cont);
        lv_label_set_text(*label_out, text);
        lv_obj_set_style_text_font(*label_out, UI_FONT_BODY, LV_PART_MAIN);
        lv_obj_set_style_text_color(*label_out, COLOR_TEXT_DIM, LV_PART_MAIN);
    };
    
    make_dot(dots_row, i18n_get(STR_HP_COMPRESSOR), &state.comp_dot, &state.comp_dot_label);
    make_dot(dots_row, i18n_get(STR_HP_FAN), &state.fan_dot, &state.fan_dot_label);
    make_dot(dots_row, i18n_get(STR_HP_PUMP), &state.pump_dot, &state.pump_dot_label);
    make_dot(dots_row, i18n_get(STR_HP_AUX_HEAT), &state.heater_dot, &state.heater_dot_label);
    
    // =========================================================================
    // PERFORMANCE STRIP: COP | Power | Fan (dimmed when idle)
    // =========================================================================
    state.perf_card = lv_obj_create(state.container);
    lv_obj_set_size(state.perf_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(state.perf_card, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_border_color(state.perf_card, COLOR_CARD_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.perf_card, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(state.perf_card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.perf_card, 12, LV_PART_MAIN);
    lv_obj_clear_flag(state.perf_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(state.perf_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(state.perf_card, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    create_value_column(state.perf_card, i18n_get(STR_HP_LABEL_COP), &state.perf_cop_value);
    create_value_column(state.perf_card, i18n_get(STR_HP_LABEL_POWER), &state.perf_power_value);
    create_value_column(state.perf_card, i18n_get(STR_HP_LABEL_FAN), &state.perf_fan_value);
    
    // =========================================================================
    // ERROR CARD: Prominent error/status display (tap for details)
    // =========================================================================
    state.error_card = lv_obj_create(state.container);
    lv_obj_set_size(state.error_card, LV_PCT(100), 70);
    lv_obj_set_style_bg_color(state.error_card, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(state.error_card, lv_color_hex(0x1e2d4e), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(state.error_card, COLOR_CARD_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.error_card, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(state.error_card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.error_card, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_right(state.error_card, 40, LV_PART_MAIN);
    lv_obj_clear_flag(state.error_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(state.error_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(state.error_card, error_card_cb, LV_EVENT_CLICKED, nullptr);
    
    state.error_label = lv_label_create(state.error_card);
    lv_label_set_text(state.error_label, i18n_get(STR_HP_SYSTEM_OK));
    lv_obj_set_style_text_font(state.error_label, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.error_label, COLOR_SUCCESS, LV_PART_MAIN);
    lv_obj_set_width(state.error_label, LV_PCT(100));
    lv_obj_set_style_text_align(state.error_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(state.error_label);
    
    state.error_chevron = lv_label_create(state.error_card);
    lv_label_set_text(state.error_chevron, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(state.error_chevron, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.error_chevron, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(state.error_chevron, LV_ALIGN_RIGHT_MID, 20, 0);
    
    // =========================================================================
    // EXPANDABLE: Temperatures (Inlet | Outlet | Ambient | Coil)
    // =========================================================================
    create_expandable_panel(state.container, i18n_get(STR_HP_TEMPERATURES),
                            &state.temps_chevron, &state.temps_content, temps_panel_cb);
    
    // Temps content: row of 4 value columns
    lv_obj_t* temps_row = lv_obj_create(state.temps_content);
    lv_obj_set_size(temps_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(temps_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(temps_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(temps_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(temps_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(temps_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(temps_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    create_value_column(temps_row, i18n_get(STR_HP_LABEL_INLET), &state.inlet_value);
    create_value_column(temps_row, i18n_get(STR_HP_LABEL_OUTLET), &state.outlet_value);
    create_value_column(temps_row, i18n_get(STR_HP_LABEL_AMBIENT), &state.ambient_value);
    create_value_column(temps_row, i18n_get(STR_HP_LABEL_COIL), &state.coil_value);
    
    // =========================================================================
    // EXPANDABLE: Compressor Details
    // =========================================================================
    create_expandable_panel(state.container, i18n_get(STR_HP_COMPRESSOR),
                            &state.comp_chevron, &state.comp_content, comp_panel_cb);
    
    // Compressor status line
    state.comp_status_label = lv_label_create(state.comp_content);
    lv_label_set_text(state.comp_status_label, i18n_get(STR_HP_COMPRESSOR));
    lv_obj_set_style_text_font(state.comp_status_label, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.comp_status_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    
    // Frequency row: label + bar
    lv_obj_t* freq_row = lv_obj_create(state.comp_content);
    lv_obj_set_size(freq_row, LV_PCT(100), 30);
    lv_obj_set_style_bg_opa(freq_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(freq_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(freq_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(freq_row, LV_OBJ_FLAG_SCROLLABLE);
    
    state.comp_freq_label = lv_label_create(freq_row);
    lv_label_set_text(state.comp_freq_label, "0 Hz");
    lv_obj_set_style_text_font(state.comp_freq_label, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.comp_freq_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(state.comp_freq_label, LV_ALIGN_LEFT_MID, 0, 0);
    
    state.comp_freq_bar = lv_bar_create(freq_row);
    lv_obj_set_size(state.comp_freq_bar, 450, 18);
    lv_obj_align(state.comp_freq_bar, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_bar_set_range(state.comp_freq_bar, 0, 120);
    lv_bar_set_value(state.comp_freq_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(state.comp_freq_bar, lv_color_hex(0x0a1628), LV_PART_MAIN);
    lv_obj_set_style_bg_color(state.comp_freq_bar, COLOR_SUCCESS, LV_PART_INDICATOR);
    lv_obj_set_style_radius(state.comp_freq_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(state.comp_freq_bar, 4, LV_PART_INDICATOR);
    
    // Detail row 1: Discharge | Suction | EEV
    lv_obj_t* detail_row1 = lv_obj_create(state.comp_content);
    lv_obj_set_size(detail_row1, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(detail_row1, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(detail_row1, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(detail_row1, 0, LV_PART_MAIN);
    lv_obj_set_style_margin_top(detail_row1, 8, LV_PART_MAIN);
    lv_obj_clear_flag(detail_row1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(detail_row1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(detail_row1, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    create_value_column(detail_row1, i18n_get(STR_HP_LABEL_DISCHARGE), &state.comp_discharge_value);
    create_value_column(detail_row1, i18n_get(STR_HP_LABEL_SUCTION), &state.comp_suction_value);
    create_value_column(detail_row1, i18n_get(STR_HP_LABEL_EEV), &state.comp_eev_value);
    
    // Detail row 2: High Press | Low Press | ΔT
    lv_obj_t* detail_row2 = lv_obj_create(state.comp_content);
    lv_obj_set_size(detail_row2, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(detail_row2, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(detail_row2, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(detail_row2, 0, LV_PART_MAIN);
    lv_obj_clear_flag(detail_row2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(detail_row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(detail_row2, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    create_value_column(detail_row2, i18n_get(STR_HP_LABEL_HI_PRESS), &state.comp_hi_press_value);
    create_value_column(detail_row2, i18n_get(STR_HP_LABEL_LO_PRESS), &state.comp_lo_press_value);
    lv_obj_t* dt_col = create_value_column(detail_row2, "\xCE\x94T", &state.comp_dt_value);
    state.comp_dt_label = lv_obj_get_child(dt_col, 1);
    
    // =========================================================================
    // EXPANDABLE: Energy (Power In | Heat Out | COP)
    // =========================================================================
    create_expandable_panel(state.container, i18n_get(STR_HP_ENERGY),
                            &state.energy_chevron, &state.energy_content, energy_panel_cb);
    
    // Energy content: row of 3 value columns
    lv_obj_t* energy_row = lv_obj_create(state.energy_content);
    lv_obj_set_size(energy_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(energy_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(energy_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(energy_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(energy_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(energy_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(energy_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    create_value_column(energy_row, i18n_get(STR_HP_LABEL_POWER_IN), &state.energy_in_value);
    create_value_column(energy_row, i18n_get(STR_HP_LABEL_HEAT_OUT), &state.energy_out_value);
    create_value_column(energy_row, i18n_get(STR_HP_LABEL_COP), &state.energy_cop_value);
    
    // =========================================================================
    // FIXED FOOTER: Three button bar - Temps | System | Advanced
    // =========================================================================
    lv_obj_t* btn_row = lv_obj_create(parent);
    lv_obj_set_size(btn_row, LV_PCT(100), FOOTER_H);
    lv_obj_align(btn_row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(btn_row, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(btn_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn_row, 10, LV_PART_MAIN);

    state.temps_btn = lv_btn_create(btn_row);
    lv_obj_set_size(state.temps_btn, 200, 60);
    lv_obj_set_style_bg_color(state.temps_btn, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(state.temps_btn, lv_color_hex(0x2a3a5e), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(state.temps_btn, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.temps_btn, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(state.temps_btn, 12, LV_PART_MAIN);
    lv_obj_add_event_cb(state.temps_btn, temps_btn_cb, LV_EVENT_CLICKED, nullptr);
    
    state.temps_btn_label = lv_label_create(state.temps_btn);
    lv_label_set_text(state.temps_btn_label, i18n_get(STR_HP_BTN_TEMPS));
    lv_obj_set_style_text_font(state.temps_btn_label, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.temps_btn_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_center(state.temps_btn_label);
    
    state.system_btn = lv_btn_create(btn_row);
    lv_obj_set_size(state.system_btn, 200, 60);
    lv_obj_set_style_bg_color(state.system_btn, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(state.system_btn, lv_color_hex(0x2a3a5e), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(state.system_btn, COLOR_WARNING, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.system_btn, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(state.system_btn, 12, LV_PART_MAIN);
    lv_obj_add_event_cb(state.system_btn, system_btn_cb, LV_EVENT_CLICKED, nullptr);
    
    state.system_btn_label = lv_label_create(state.system_btn);
    lv_label_set_text(state.system_btn_label, i18n_get(STR_HP_BTN_SYSTEM));
    lv_obj_set_style_text_font(state.system_btn_label, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.system_btn_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_center(state.system_btn_label);
    
    state.controls_btn = lv_btn_create(btn_row);
    lv_obj_set_size(state.controls_btn, 200, 60);
    lv_obj_set_style_bg_color(state.controls_btn, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(state.controls_btn, lv_color_hex(0x2a3a5e), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(state.controls_btn, COLOR_SUCCESS, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.controls_btn, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(state.controls_btn, 12, LV_PART_MAIN);
    lv_obj_add_event_cb(state.controls_btn, controls_btn_cb, LV_EVENT_CLICKED, nullptr);
    
    state.controls_btn_label = lv_label_create(state.controls_btn);
    lv_label_set_text(state.controls_btn_label, i18n_get(STR_HP_BTN_ADVANCED));
    lv_obj_set_style_text_font(state.controls_btn_label, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.controls_btn_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_center(state.controls_btn_label);

    state.created = true;
    
    // Create update timer (1 second interval)
    state.update_timer = lv_timer_create(update_timer_cb, 1000, nullptr);
    
    // Initial update
    heatpump_screen_update();
}

void heatpump_screen_update(void) {
    if (!state.created) {
        return;
    }
    
    arctic::HeatPumpState hp = arctic::getState();
    
    // Refresh i18n labels
    if (state.temps_btn_label) lv_label_set_text(state.temps_btn_label, i18n_get(STR_HP_BTN_TEMPS));
    if (state.system_btn_label) lv_label_set_text(state.system_btn_label, i18n_get(STR_HP_BTN_SYSTEM));
    if (state.controls_btn_label) lv_label_set_text(state.controls_btn_label, i18n_get(STR_HP_BTN_ADVANCED));
    if (state.hero_tank_desc) lv_label_set_text(state.hero_tank_desc, i18n_get(STR_HP_TANK_TEMPERATURE));
    
    // =====================================================================
    // HERO STATE CARD — color-coded background as primary state indicator
    // =====================================================================
    HeroState hero = getHeroState(hp);
    
    lv_obj_set_style_bg_color(state.hero_card, getHeroBgColor(hero), LV_PART_MAIN);
    lv_obj_set_style_border_color(state.hero_card, getHeroBorderColor(hero), LV_PART_MAIN);
    lv_label_set_text(state.hero_state_label, getHeroStateText(hero, hp));
    lv_obj_set_style_text_color(state.hero_state_label, getHeroTextColor(hero), LV_PART_MAIN);
    
    // Tank temperature
    char temp_buf[32];
    if (hp.connected) {
        snprintf(temp_buf, sizeof(temp_buf), "%d %s",
                 app_prefs_convert_temp(hp.water_tank_temp), app_prefs_temp_unit_str());
        lv_label_set_text(state.hero_tank_label, temp_buf);
        lv_obj_set_style_text_color(state.hero_tank_label, COLOR_TEXT, LV_PART_MAIN);
    } else {
        lv_label_set_text(state.hero_tank_label, "--");
        lv_obj_set_style_text_color(state.hero_tank_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    }
    
    // =====================================================================
    // PERFORMANCE STRIP — COP | Power | Fan (dimmed when idle)
    // =====================================================================
    bool compressor_on = hp.connected && hp.isCompressorRunning();
    lv_color_t perf_dim = compressor_on ? COLOR_TEXT : COLOR_TEXT_DIM;
    
    // Power
    uint32_t power_watts = 0;
    char power_buf[32];
    if (hp.connected) {
        power_watts = (uint32_t)hp.ac_voltage * hp.ac_current / 10;
        if (power_watts >= 1000) {
            uint32_t kw_int = power_watts / 1000;
            uint32_t kw_frac = (power_watts % 1000) / 100;
            snprintf(power_buf, sizeof(power_buf), "%lu.%lu kW", (unsigned long)kw_int, (unsigned long)kw_frac);
        } else {
            snprintf(power_buf, sizeof(power_buf), "%lu W", (unsigned long)power_watts);
        }
        lv_label_set_text(state.perf_power_value, power_buf);
    } else {
        lv_label_set_text(state.perf_power_value, "--");
        power_buf[0] = '-'; power_buf[1] = '-'; power_buf[2] = '\0';
    }
    lv_obj_set_style_text_color(state.perf_power_value, perf_dim, LV_PART_MAIN);
    
    // COP (in performance strip)
    int16_t water_dt_tenths = hp.outlet_water_temp - hp.inlet_water_temp;
    uint32_t cop_x10 = 0;
    if (compressor_on && water_dt_tenths > 0 && power_watts > 0) {
        uint32_t heat_out = (uint32_t)(4186 * water_dt_tenths) / 30;
        cop_x10 = heat_out * 10 / power_watts;
        char cop_buf[16];
        snprintf(cop_buf, sizeof(cop_buf), "%lu.%lu", (unsigned long)(cop_x10 / 10), (unsigned long)(cop_x10 % 10));
        lv_label_set_text(state.perf_cop_value, cop_buf);
        // Color-code: green >= 3.0, yellow >= 2.0, red < 2.0
        if (cop_x10 >= 30) {
            lv_obj_set_style_text_color(state.perf_cop_value, COLOR_SUCCESS, LV_PART_MAIN);
        } else if (cop_x10 >= 20) {
            lv_obj_set_style_text_color(state.perf_cop_value, COLOR_WARNING, LV_PART_MAIN);
        } else {
            lv_obj_set_style_text_color(state.perf_cop_value, COLOR_ERROR, LV_PART_MAIN);
        }
    } else {
        lv_label_set_text(state.perf_cop_value, "--");
        lv_obj_set_style_text_color(state.perf_cop_value, COLOR_TEXT_DIM, LV_PART_MAIN);
    }
    
    // Fan speed
    if (hp.connected && hp.fan_speed > 0) {
        char fan_buf[16];
        snprintf(fan_buf, sizeof(fan_buf), "%u RPM", hp.fan_speed);
        lv_label_set_text(state.perf_fan_value, fan_buf);
        lv_obj_set_style_text_color(state.perf_fan_value, COLOR_TEXT, LV_PART_MAIN);
    } else {
        lv_label_set_text(state.perf_fan_value, "--");
        lv_obj_set_style_text_color(state.perf_fan_value, COLOR_TEXT_DIM, LV_PART_MAIN);
    }
    
    // =====================================================================
    // COMPONENT DOTS ROW
    // =====================================================================
    set_indicator_active(state.comp_dot, hp.connected && hp.isCompressorRunning(), COLOR_SUCCESS);
    set_indicator_active(state.fan_dot, hp.connected && hp.isFanRunning(), COLOR_SUCCESS);
    set_indicator_active(state.pump_dot, hp.connected && hp.isWaterPumpRunning(), COLOR_ACCENT);
    set_indicator_active(state.heater_dot, hp.connected && hp.isBackupHeaterOn(), COLOR_WARNING);
    
    // =====================================================================
    // ERROR CARD
    // =====================================================================
    if (!hp.connected) {
        lv_label_set_text(state.error_label, i18n_get(STR_HP_NOT_CONNECTED));
        lv_obj_set_style_text_color(state.error_label, COLOR_ERROR, LV_PART_MAIN);
        lv_obj_set_style_border_color(state.error_card, COLOR_ERROR, LV_PART_MAIN);
        lv_obj_set_style_text_color(state.error_chevron, COLOR_ERROR, LV_PART_MAIN);
    } else if (hp.hasAnyError()) {
        char error_buf[256];
        format_error_card_text(error_buf, sizeof(error_buf), UI_FONT_BODY, 620);
        lv_label_set_text(state.error_label, error_buf);
        lv_obj_set_style_text_color(state.error_label, COLOR_ERROR, LV_PART_MAIN);
        lv_obj_set_style_border_color(state.error_card, COLOR_ERROR, LV_PART_MAIN);
        lv_obj_set_style_text_color(state.error_chevron, COLOR_ERROR, LV_PART_MAIN);
    } else {
        lv_label_set_text(state.error_label, i18n_get(STR_HP_SYSTEM_OK));
        lv_obj_set_style_text_color(state.error_label, COLOR_SUCCESS, LV_PART_MAIN);
        lv_obj_set_style_border_color(state.error_card, COLOR_CARD_BORDER, LV_PART_MAIN);
        lv_obj_set_style_text_color(state.error_chevron, COLOR_TEXT_DIM, LV_PART_MAIN);
    }
    
    // =====================================================================
    // EXPANDABLE: Temperatures (updated even when collapsed)
    // =====================================================================
    if (hp.connected) {
        snprintf(temp_buf, sizeof(temp_buf), "%d%s",
                 app_prefs_convert_temp(hp.inlet_water_temp), app_prefs_temp_unit_str());
        lv_label_set_text(state.inlet_value, temp_buf);
        
        snprintf(temp_buf, sizeof(temp_buf), "%d%s",
                 app_prefs_convert_temp(hp.outlet_water_temp), app_prefs_temp_unit_str());
        lv_label_set_text(state.outlet_value, temp_buf);
        
        snprintf(temp_buf, sizeof(temp_buf), "%d%s",
                 app_prefs_convert_temp(hp.outdoor_ambient_temp), app_prefs_temp_unit_str());
        lv_label_set_text(state.ambient_value, temp_buf);
        
        snprintf(temp_buf, sizeof(temp_buf), "%d%s",
                 app_prefs_convert_temp(hp.outdoor_coil_temp), app_prefs_temp_unit_str());
        lv_label_set_text(state.coil_value, temp_buf);
    } else {
        lv_label_set_text(state.inlet_value, "--");
        lv_label_set_text(state.outlet_value, "--");
        lv_label_set_text(state.ambient_value, "--");
        lv_label_set_text(state.coil_value, "--");
    }
    
    // =====================================================================
    // EXPANDABLE: Compressor Details (updated even when collapsed)
    // =====================================================================
    bool defrosting = hp.connected && hp.isDefrosting();
    char comp_buf[80];
    
    if (compressor_on) {
        const char* comp_state_text = defrosting
            ? i18n_get(STR_HP_MODE_DEFROST)
            : i18n_get(STR_HP_COMP_RUNNING);
        lv_color_t comp_color = defrosting ? COLOR_DEFROST : COLOR_SUCCESS;
        
        snprintf(comp_buf, sizeof(comp_buf), "%s  \xE2\x97\x8F  %s",
                 i18n_get(STR_HP_COMPRESSOR), comp_state_text);
        lv_label_set_text(state.comp_status_label, comp_buf);
        lv_obj_set_style_text_color(state.comp_status_label, comp_color, LV_PART_MAIN);
        
        lv_bar_set_value(state.comp_freq_bar, hp.compressor_freq, LV_ANIM_ON);
        snprintf(comp_buf, sizeof(comp_buf), "%u Hz", hp.compressor_freq);
        lv_label_set_text(state.comp_freq_label, comp_buf);
        
        snprintf(comp_buf, sizeof(comp_buf), "%d%s",
                 app_prefs_convert_temp(hp.discharge_temp), app_prefs_temp_unit_str());
        lv_label_set_text(state.comp_discharge_value, comp_buf);
        
        snprintf(comp_buf, sizeof(comp_buf), "%d%s",
                 app_prefs_convert_temp(hp.suction_temp), app_prefs_temp_unit_str());
        lv_label_set_text(state.comp_suction_value, comp_buf);
        
        snprintf(comp_buf, sizeof(comp_buf), "%u steps", hp.primary_eev_opening);
        lv_label_set_text(state.comp_eev_value, comp_buf);
        
        snprintf(comp_buf, sizeof(comp_buf), "%u.%02u MPa",
                 hp.high_pressure / 100, hp.high_pressure % 100);
        lv_label_set_text(state.comp_hi_press_value, comp_buf);
        
        snprintf(comp_buf, sizeof(comp_buf), "%u.%02u MPa",
                 hp.low_pressure / 100, hp.low_pressure % 100);
        lv_label_set_text(state.comp_lo_press_value, comp_buf);
        
        if (defrosting) {
            lv_label_set_text(state.comp_dt_label, "COIL");
            snprintf(comp_buf, sizeof(comp_buf), "%d%s",
                     app_prefs_convert_temp(hp.outdoor_coil_temp), app_prefs_temp_unit_str());
        } else {
            lv_label_set_text(state.comp_dt_label, "\xCE\x94T");
            int16_t dt = app_prefs_convert_temp(hp.outlet_water_temp)
                       - app_prefs_convert_temp(hp.inlet_water_temp);
            snprintf(comp_buf, sizeof(comp_buf), "%d%s", dt, app_prefs_temp_unit_str());
        }
        lv_label_set_text(state.comp_dt_value, comp_buf);
        
        lv_obj_set_style_bg_color(state.comp_freq_bar, defrosting ? COLOR_DEFROST : COLOR_SUCCESS, LV_PART_INDICATOR);
    } else {
        snprintf(comp_buf, sizeof(comp_buf), "%s  \xE2\x97\x8B  %s",
                 i18n_get(STR_HP_COMPRESSOR),
                 hp.connected ? i18n_get(STR_HP_COMP_IDLE) : "--");
        lv_label_set_text(state.comp_status_label, comp_buf);
        lv_obj_set_style_text_color(state.comp_status_label, COLOR_TEXT_DIM, LV_PART_MAIN);
        
        lv_bar_set_value(state.comp_freq_bar, 0, LV_ANIM_ON);
        lv_label_set_text(state.comp_freq_label, "0 Hz");
        
        lv_label_set_text(state.comp_discharge_value, "--");
        lv_label_set_text(state.comp_suction_value, "--");
        lv_label_set_text(state.comp_eev_value, "--");
        lv_label_set_text(state.comp_hi_press_value, "--");
        lv_label_set_text(state.comp_lo_press_value, "--");
        lv_label_set_text(state.comp_dt_value, "--");
    }
    
    // =====================================================================
    // EXPANDABLE: Energy (updated even when collapsed)
    // =====================================================================
    lv_label_set_text(state.energy_in_value, hp.connected ? power_buf : "--");
    
    if (compressor_on && water_dt_tenths > 0) {
        uint32_t heat_out = (uint32_t)(4186 * water_dt_tenths) / 30;
        char heat_buf[32];
        if (heat_out >= 1000) {
            uint32_t kw_int = heat_out / 1000;
            uint32_t kw_frac = (heat_out % 1000) / 100;
            snprintf(heat_buf, sizeof(heat_buf), "%lu.%lu kW", (unsigned long)kw_int, (unsigned long)kw_frac);
        } else {
            snprintf(heat_buf, sizeof(heat_buf), "%lu W", (unsigned long)heat_out);
        }
        lv_label_set_text(state.energy_out_value, heat_buf);
        
        if (power_watts > 0) {
            char cop_buf[16];
            snprintf(cop_buf, sizeof(cop_buf), "%lu.%lu", (unsigned long)(cop_x10 / 10), (unsigned long)(cop_x10 % 10));
            lv_label_set_text(state.energy_cop_value, cop_buf);
            if (cop_x10 >= 30) {
                lv_obj_set_style_text_color(state.energy_cop_value, COLOR_SUCCESS, LV_PART_MAIN);
            } else if (cop_x10 >= 20) {
                lv_obj_set_style_text_color(state.energy_cop_value, COLOR_WARNING, LV_PART_MAIN);
            } else {
                lv_obj_set_style_text_color(state.energy_cop_value, COLOR_ERROR, LV_PART_MAIN);
            }
        } else {
            lv_label_set_text(state.energy_cop_value, "--");
            lv_obj_set_style_text_color(state.energy_cop_value, COLOR_TEXT_DIM, LV_PART_MAIN);
        }
    } else {
        lv_label_set_text(state.energy_out_value, "--");
        lv_label_set_text(state.energy_cop_value, "--");
        lv_obj_set_style_text_color(state.energy_cop_value, COLOR_TEXT_DIM, LV_PART_MAIN);
    }
}

void heatpump_screen_delete(void) {
    if (!state.created) {
        return;
    }
    
    if (state.update_timer) {
        lv_timer_del(state.update_timer);
        state.update_timer = nullptr;
    }
    
    if (state.container) {
        lv_obj_del(state.container);
        state.container = nullptr;
    }
    
    state = {};  // Reset all state
}

bool heatpump_screen_is_created(void) {
    return state.created;
}
