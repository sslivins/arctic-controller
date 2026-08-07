/*
 * Arctic Heat Pump Controller
 * Heat Pump Parameters Screen - Technician P-Parameters
 * 
 * Display-only rows with click-to-edit dialogs.
 * Portrait mode: 720x1280
 */

#include "heatpump_control_screen.h"
#include "nav_bar.h"
#include "modbus/arctic_heatpump.h"
#include "modbus/arctic_registers.h"
#include "macon_state.h"  // arctic::setpoint_limits / SetpointKind
#include "macon_advanced_params.h"  // arctic::AdvancedParam, AP reg map
#include "advanced_params.h"        // advanced_param_read/write (controller IO)

#include "ui_common.h"
#include "fonts/fonts.h"
#include "app_preferences.h"
#include "i18n/i18n.h"
#include <esp_log.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

static const char* TAG = "hp_params";

// ============================================================================
// Colors (matching other screens)
// ============================================================================
#define COLOR_BG            lv_color_hex(0x1a1a2e)
#define COLOR_CARD_BG       lv_color_hex(0x16213e)
#define COLOR_CARD_BORDER   lv_color_hex(0x0f3460)
#define COLOR_TEXT          lv_color_hex(0xeaeaea)
#define COLOR_TEXT_DIM      lv_color_hex(0x888888)
#define COLOR_ACCENT        lv_color_hex(0x00d4ff)
#define COLOR_SUCCESS       lv_color_hex(0x4ade80)
#define COLOR_WARNING       lv_color_hex(0xfbbf24)
#define COLOR_ERROR         lv_color_hex(0xef4444)

// Map advanced-parameter category string (from arctic-macon) to i18n string ID
static const char* get_category_i18n(const char* category) {
    if (strcmp(category, "EEV") == 0)          return i18n_get(STR_HP_CAT_EEV);
    if (strcmp(category, "Defrost") == 0)      return i18n_get(STR_HP_CAT_DEFROST);
    if (strcmp(category, "Protection") == 0)   return i18n_get(STR_HP_CAT_PROTECTION);
    if (strcmp(category, "Auto Mode") == 0)    return i18n_get(STR_HP_CAT_AUTO_MODE);
    if (strcmp(category, "Pump & Valve") == 0) return i18n_get(STR_HP_CAT_PUMP_VALVE);
    return category;  // Fallback to original
}

// ============================================================================
// State
// ============================================================================
static struct {
    bool shown = false;
    lv_obj_t* screen = nullptr;
    lv_obj_t* scroll_container = nullptr;
    lv_obj_t* disconnected_banner = nullptr;
    heatpump_control_close_cb_t on_close = nullptr;
    
    // Setpoint rows (Basic Settings section)
    lv_obj_t* cooling_value_label = nullptr;
    lv_obj_t* heating_value_label = nullptr;
    lv_obj_t* hotwater_value_label = nullptr;
    
    // Power button
    lv_obj_t* power_btn = nullptr;
    lv_obj_t* power_btn_label = nullptr;
    lv_obj_t* power_hold_bar = nullptr;
    lv_timer_t* power_update_timer = nullptr;
    lv_timer_t* power_hold_timer = nullptr;
    uint32_t power_hold_start = 0;
    bool power_holding = false;
    bool power_hold_completed = false;  // Suppress CLICKED after successful hold
    
    // Mode selector
    lv_obj_t* mode_btns[5] = {};
    lv_obj_t* mode_labels[5] = {};
    int active_mode_idx = 0;
    

    // Advanced ("AP") parameter rows (arctic-macon table, verified regs only).
    // Parallel arrays: ap number <-> its live-value label. Populated by
    // create_ap_section() in canonical category order.
    lv_obj_t* ap_value_labels[64] = {};
    uint8_t   ap_display_nums[64] = {};
    int       ap_display_count = 0;
    int       current_ap = -1;   // AP being edited (-1 = not editing a AP param)
    
    // Staggered loading
    lv_timer_t* load_timer = nullptr;
    int load_index = 0;
    
    // Edit dialog
    lv_obj_t* edit_dialog = nullptr;
    lv_obj_t* edit_title = nullptr;
    lv_obj_t* edit_detail = nullptr;
    lv_obj_t* edit_description = nullptr;
    lv_obj_t* edit_range_label = nullptr;
    lv_obj_t* edit_value_label = nullptr;
    lv_obj_t* edit_minus_btn = nullptr;
    lv_obj_t* edit_plus_btn = nullptr;
    lv_obj_t* edit_save_btn = nullptr;
    float edit_value = 0;       // Float for F mode precision
    int16_t edit_value_celsius = 0;  // Original value in Celsius (for non-temp params too)
    bool edit_value_unknown = false; // true when disconnected: show "--", no live value
    
    // Setpoint editing (uses same dialog, different handling)
    // -1 = editing P-parameter, 0/1/2 = editing cooling/heating/hotwater
    int current_setpoint_type = -1;
} state;

// Setpoint definitions are initialized lazily to pick up i18n language.
// min_val/max_val are owned by the shared arctic-macon library (setpoint_limits)
// and synced in via sync_setpoint_limits() — never hardcode ranges here.
struct SetpointDef {
    string_id_t name_id;
    const char* description;  // Technical - not translated
    arctic::SetpointKind kind;
    int16_t min_val;    // In Celsius (synced from library; see sync_setpoint_limits)
    int16_t max_val;    // In Celsius (synced from library; see sync_setpoint_limits)
};

static SetpointDef s_setpoints[] = {
    {STR_HP_COOLING_SETPOINT, "Target water temperature for cooling mode.", arctic::SetpointKind::Cooling, 0, 0},
    {STR_HP_HEATING_SETPOINT, "Target water temperature for floor/fan heating mode.", arctic::SetpointKind::Heating, 0, 0},
    {STR_HP_HOT_WATER_SETPOINT, "Target temperature for hot water tank.", arctic::SetpointKind::HotWater, 0, 0},
};

// Pull the current min/max for every setpoint from the shared library so the
// Tab5 UI enforces/displays the exact same guardrails as the web UI and the
// REST write path. Cheap; call before any read of s_setpoints[*].min/max_val.
static void sync_setpoint_limits(void) {
    for (auto& sp : s_setpoints) {
        const arctic::SetpointLimits lim = arctic::setpoint_limits(sp.kind);
        sp.min_val = static_cast<int16_t>(lim.min_c);
        sp.max_val = static_cast<int16_t>(lim.max_c);
    }
}

// ============================================================================
// Forward Declarations
// ============================================================================
static void close_btn_cb(lv_event_t* e);
static void edit_cancel_cb(lv_event_t* e);
static void edit_save_cb(lv_event_t* e);
static void edit_minus_cb(lv_event_t* e);
static void edit_plus_cb(lv_event_t* e);
static void hide_edit_dialog(void);
static void update_edit_value_display(void);
static void load_timer_cb(lv_timer_t* timer);
static void setpoint_row_cb(lv_event_t* e);
static void show_setpoint_edit(int setpoint_type);  // 0=cooling, 1=heating, 2=hotwater
static void power_btn_event_cb(lv_event_t* e);
static void power_hold_timer_cb(lv_timer_t* timer);
static void power_hold_cancel(void);
static void power_update_timer_cb(lv_timer_t* timer);
static void update_power_btn_appearance(bool power_on);
static void mode_btn_event_cb(lv_event_t* e);
static void update_mode_btn_styles(int selected_idx);
static void set_mode_controls_enabled(bool enabled);
// Advanced ("AP") parameter section
static void ap_row_cb(lv_event_t* e);
static void show_ap_edit_dialog(uint8_t ap);
static void show_ap_trigger_confirm(uint8_t ap);
static void ap_trigger_run_cb(lv_event_t* e);
static void create_ap_section(lv_obj_t* parent);
static void ap_update_display(int slot);
static const char* kratio_label(uint8_t ap, int wire);
static void kratio_desc(uint8_t ap, int wire, char* buf, size_t n);

// ============================================================================
// Demo Setpoints (screen-local)
// ============================================================================
static int16_t s_demo_setpoints[3] = {-1, -1, -1};  // Demo setpoints: cooling, heating, hot water (-1 = not set)
static bool s_demo_setpoints_initialized = false;

static void init_demo_setpoints(void) {
    if (s_demo_setpoints_initialized) return;
    sync_setpoint_limits();
    
    // Initialize setpoints to midpoint of range
    for (int i = 0; i < 3; i++) {
        s_demo_setpoints[i] = (s_setpoints[i].min_val + s_setpoints[i].max_val) / 2;
    }
    
    s_demo_setpoints_initialized = true;
}

static int16_t get_demo_setpoint(int setpoint_type) {
    if (setpoint_type < 0 || setpoint_type > 2) return 0;
    init_demo_setpoints();
    return s_demo_setpoints[setpoint_type];
}

static void set_demo_setpoint(int setpoint_type, int16_t value) {
    if (setpoint_type < 0 || setpoint_type > 2) return;
    init_demo_setpoints();
    s_demo_setpoints[setpoint_type] = value;
}

// Helper: show error popup for write failures
static void show_settings_write_error(const char* message) {
    lv_obj_t* msgbox = lv_msgbox_create(lv_layer_top());
    lv_msgbox_add_title(msgbox, i18n_get(STR_HP_COMMUNICATION_ERROR));
    lv_msgbox_add_text(msgbox, message);
    lv_msgbox_add_close_button(msgbox);
    lv_obj_center(msgbox);
    lv_obj_set_width(msgbox, 400);
}

// ============================================================================
// Power Button
// ============================================================================

#define POWER_HOLD_DURATION_MS 3000
#define POWER_HOLD_TICK_MS     100

static void update_power_btn_appearance(bool power_on) {
    if (!state.power_btn || !state.power_btn_label) return;

    // Disconnected (and not demo): the pump state is unknown, so avoid the
    // misleading red "POWERED OFF". Show a neutral, disabled-looking button —
    // the separate disconnected banner already explains why controls are off.
    arctic::HeatPumpState hp = arctic::getState();
    if (!hp.connected && !app_prefs_is_demo_mode()) {
        lv_obj_set_style_bg_color(state.power_btn, COLOR_CARD_BG, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(state.power_btn, LV_OPA_50, LV_PART_MAIN);
        lv_obj_set_style_border_width(state.power_btn, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(state.power_btn, COLOR_CARD_BORDER, LV_PART_MAIN);
        lv_label_set_text(state.power_btn_label, i18n_get(STR_HP_POWER_UNAVAILABLE));
        lv_obj_set_style_text_color(state.power_btn_label, COLOR_TEXT_DIM, LV_PART_MAIN);
        if (state.power_hold_bar) {
            lv_bar_set_value(state.power_hold_bar, 0, LV_ANIM_OFF);
            lv_obj_add_flag(state.power_hold_bar, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    // Connected (or demo): restore full-opacity coloured button.
    lv_obj_set_style_bg_opa(state.power_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.power_btn, 0, LV_PART_MAIN);
    if (power_on) {
        lv_obj_set_style_bg_color(state.power_btn, COLOR_SUCCESS, LV_PART_MAIN);
        lv_label_set_text(state.power_btn_label, i18n_get(STR_HP_POWER_ON));
        lv_obj_set_style_text_color(state.power_btn_label, lv_color_hex(0x0a2010), LV_PART_MAIN);
    } else {
        lv_obj_set_style_bg_color(state.power_btn, COLOR_ERROR, LV_PART_MAIN);
        lv_label_set_text(state.power_btn_label, i18n_get(STR_HP_POWER_OFF));
        lv_obj_set_style_text_color(state.power_btn_label, COLOR_TEXT, LV_PART_MAIN);
    }
    // Hide progress bar when not holding
    if (state.power_hold_bar) {
        lv_bar_set_value(state.power_hold_bar, 0, LV_ANIM_OFF);
        lv_obj_add_flag(state.power_hold_bar, LV_OBJ_FLAG_HIDDEN);
    }
}

// Enable/disable the working-mode buttons together. When disabled (pump
// disconnected) they are dimmed and non-interactive so the user cannot trigger
// a doomed write that just pops a communication-error modal.
static void set_mode_controls_enabled(bool enabled) {
    for (int i = 0; i < 5; i++) {
        if (!state.mode_btns[i]) continue;
        if (enabled) {
            lv_obj_add_flag(state.mode_btns[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_opa(state.mode_btns[i], LV_OPA_COVER, LV_PART_MAIN);
        } else {
            lv_obj_remove_flag(state.mode_btns[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_opa(state.mode_btns[i], LV_OPA_40, LV_PART_MAIN);
        }
    }
}

static void power_hold_cancel(void) {
    if (!state.power_holding) return;
    state.power_holding = false;
    
    if (state.power_hold_timer) {
        lv_timer_del(state.power_hold_timer);
        state.power_hold_timer = nullptr;
    }
    
    // Restore normal appearance
    arctic::HeatPumpState hp = arctic::getState();
    update_power_btn_appearance(hp.connected ? hp.unit_on : false);
}

static void power_hold_timer_cb(lv_timer_t* timer) {
    if (!state.power_holding || !state.power_btn) {
        power_hold_cancel();
        return;
    }
    
    uint32_t elapsed = lv_tick_elaps(state.power_hold_start);
    int pct = (int)((elapsed * 100) / POWER_HOLD_DURATION_MS);
    if (pct > 100) pct = 100;
    
    // Update progress bar
    if (state.power_hold_bar) {
        lv_bar_set_value(state.power_hold_bar, pct, LV_ANIM_OFF);
    }
    
    // Update countdown label: "Powering off in 3..."
    int remaining = (int)((POWER_HOLD_DURATION_MS - elapsed + 999) / 1000);
    if (remaining < 1) remaining = 1;
    char buf[64];
    snprintf(buf, sizeof(buf), i18n_get(STR_HP_HOLD_POWER_OFF), remaining);
    lv_label_set_text(state.power_btn_label, buf);
    lv_obj_set_style_text_color(state.power_btn_label, lv_color_hex(0x0a2010), LV_PART_MAIN);
    
    // Blend button color from green toward red as hold progresses
    uint8_t r = (uint8_t)(0x4a + (0xef - 0x4a) * pct / 100);
    uint8_t g = (uint8_t)(0xde - (0xde - 0x44) * pct / 100);
    uint8_t b = (uint8_t)(0x80 - (0x80 - 0x44) * pct / 100);
    lv_obj_set_style_bg_color(state.power_btn, lv_color_make(r, g, b), LV_PART_MAIN);
    
    if (elapsed >= POWER_HOLD_DURATION_MS) {
        // Hold complete — power off!
        state.power_holding = false;
        state.power_hold_completed = true;  // Suppress the upcoming CLICKED event
        if (state.power_hold_timer) {
            lv_timer_del(state.power_hold_timer);
            state.power_hold_timer = nullptr;
        }
        arctic::setUnitPower(false);
        update_power_btn_appearance(false);
    }
}

static void power_btn_event_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_PRESSED) {
        arctic::HeatPumpState hp = arctic::getState();
        
        if (!hp.connected && !app_prefs_is_demo_mode()) {
            return;  // Will show error on CLICKED
        }
        
        if (hp.unit_on) {
            // Start hold-to-off sequence
            state.power_holding = true;
            state.power_hold_start = lv_tick_get();
            
            // Show countdown immediately
            int secs = POWER_HOLD_DURATION_MS / 1000;
            char buf[64];
            snprintf(buf, sizeof(buf), i18n_get(STR_HP_HOLD_POWER_OFF), secs);
            lv_label_set_text(state.power_btn_label, buf);
            lv_obj_set_style_text_color(state.power_btn_label, lv_color_hex(0x0a2010), LV_PART_MAIN);
            if (state.power_hold_bar) {
                lv_bar_set_value(state.power_hold_bar, 0, LV_ANIM_OFF);
                lv_obj_remove_flag(state.power_hold_bar, LV_OBJ_FLAG_HIDDEN);
            }
            
            // Start tick timer
            state.power_hold_timer = lv_timer_create(power_hold_timer_cb, POWER_HOLD_TICK_MS, nullptr);
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (state.power_holding) {
            power_hold_cancel();
        }
    } else if (code == LV_EVENT_CLICKED) {
        // Suppress click after a successful hold (hold sets power_hold_completed)
        if (state.power_hold_completed) {
            state.power_hold_completed = false;
            return;
        }
        if (state.power_holding) return;  // Was a hold attempt, ignore click
        
        arctic::HeatPumpState hp = arctic::getState();
        if (!hp.connected && !app_prefs_is_demo_mode()) {
            return;  // Button is shown disabled/neutral while disconnected — no-op.
        }
        
        if (!hp.unit_on) {
            arctic::setUnitPower(true);
            update_power_btn_appearance(true);
        }
    }
}

static void power_update_timer_cb(lv_timer_t* timer) {
    if (!state.power_btn || state.power_holding) return;  // Don't overwrite hold animation
    arctic::HeatPumpState hp = arctic::getState();
    update_power_btn_appearance(hp.connected ? hp.unit_on : false);

    // Show the single disconnected banner whenever the pump is offline (demo
    // mode counts as connected for editing purposes).
    if (state.disconnected_banner) {
        bool disconnected = !hp.connected && !app_prefs_is_demo_mode();
        if (disconnected) lv_obj_remove_flag(state.disconnected_banner, LV_OBJ_FLAG_HIDDEN);
        else              lv_obj_add_flag(state.disconnected_banner, LV_OBJ_FLAG_HIDDEN);
        // Disable mode buttons while disconnected so taps can't trigger a
        // communication-error modal.
        set_mode_controls_enabled(!disconnected);
    }
    
    // Also keep mode buttons in sync
    int mode_idx = 0;
    switch (hp.working_mode) {
        case arctic::WorkingMode::COOLING:          mode_idx = 0; break;
        case arctic::WorkingMode::FLOOR_HEATING:    mode_idx = 1; break;
        case arctic::WorkingMode::FAN_COIL_HEATING: mode_idx = 2; break;
        case arctic::WorkingMode::HOT_WATER:        mode_idx = 3; break;
        case arctic::WorkingMode::AUTO:             mode_idx = 4; break;
        default: mode_idx = 0; break;
    }
    if (mode_idx != state.active_mode_idx) {
        state.active_mode_idx = mode_idx;
        update_mode_btn_styles(mode_idx);
    }
}

// ============================================================================
// Mode Selector
// ============================================================================

static const arctic::WorkingMode s_mode_values[] = {
    arctic::WorkingMode::COOLING,
    arctic::WorkingMode::FLOOR_HEATING,
    arctic::WorkingMode::FAN_COIL_HEATING,
    arctic::WorkingMode::HOT_WATER,
    arctic::WorkingMode::AUTO,
};

static const string_id_t s_mode_labels[] = {
    STR_HP_MODE_COOLING,
    STR_HP_MODE_FLOOR_HEAT,
    STR_HP_MODE_FAN_HEAT,
    STR_HP_MODE_HOT_WATER,
    STR_HP_MODE_AUTO,
};

static const uint32_t s_mode_colors[] = {
    0x3b82f6,  // Cooling - blue
    0xf97316,  // Floor heating - orange
    0xf97316,  // Fan coil heating - orange
    0xef4444,  // Hot water - red
    0x8b5cf6,  // Auto - purple
};

static void update_mode_btn_styles(int selected_idx) {
    for (int i = 0; i < 5; i++) {
        if (!state.mode_btns[i]) continue;
        if (i == selected_idx) {
            lv_obj_set_style_bg_color(state.mode_btns[i], lv_color_hex(s_mode_colors[i]), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(state.mode_btns[i], LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_color(state.mode_btns[i], lv_color_hex(s_mode_colors[i]), LV_PART_MAIN);
            if (state.mode_labels[i]) {
                lv_obj_set_style_text_color(state.mode_labels[i], lv_color_hex(0xffffff), LV_PART_MAIN);
            }
        } else {
            lv_obj_set_style_bg_color(state.mode_btns[i], COLOR_CARD_BG, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(state.mode_btns[i], LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_color(state.mode_btns[i], COLOR_CARD_BORDER, LV_PART_MAIN);
            if (state.mode_labels[i]) {
                lv_obj_set_style_text_color(state.mode_labels[i], COLOR_TEXT_DIM, LV_PART_MAIN);
            }
        }
    }
}

static void mode_btn_event_cb(lv_event_t* e) {
    lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || idx >= 5) return;
    if (idx == state.active_mode_idx) return;  // Already selected
    
    arctic::HeatPumpState hp = arctic::getState();
    if (!hp.connected && !app_prefs_is_demo_mode()) {
        return;  // Mode buttons are disabled while disconnected — no-op.
    }
    
    bool success = arctic::setWorkingMode(s_mode_values[idx]);
    if (success) {
        state.active_mode_idx = idx;
        update_mode_btn_styles(idx);
        ESP_LOGI(TAG, "Mode changed to %s", arctic::workingModeToString(s_mode_values[idx]));
    } else {
        show_settings_write_error("Failed to set operating mode");
    }
}

// ============================================================================
// Helper Functions
// ============================================================================

static void load_timer_cb(lv_timer_t* timer) {
    (void)timer;

    if (!state.shown || state.load_index >= state.ap_display_count) {
        // Done loading - delete timer
        if (state.load_timer) {
            lv_timer_del(state.load_timer);
            state.load_timer = nullptr;
        }
        return;
    }

    // Don't load if an edit dialog is open
    if (state.current_ap >= 0 || state.current_setpoint_type >= 0) return;

    // Read and display one advanced parameter.
    ap_update_display(state.load_index);

    state.load_index++;
}

static lv_obj_t* create_section_header(lv_obj_t* parent, const char* title) {
    lv_obj_t* header = lv_obj_create(parent);
    lv_obj_set_size(header, LV_PCT(100), 50);
    lv_obj_set_style_bg_color(header, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(header, 10, LV_PART_MAIN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t* lbl = lv_label_create(header);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, UI_FONT_SECTION, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    
    return header;
}

static lv_obj_t* create_setpoint_row(lv_obj_t* parent, int setpoint_type, lv_obj_t** value_label_out) {
    const SetpointDef& sp = s_setpoints[setpoint_type];
    
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 70);
    lv_obj_set_style_bg_color(row, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, COLOR_CARD_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 20, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    
    // Store setpoint type in user data (offset by 1000 to distinguish from params)
    lv_obj_set_user_data(row, (void*)(intptr_t)(1000 + setpoint_type));
    lv_obj_add_event_cb(row, setpoint_row_cb, LV_EVENT_CLICKED, nullptr);
    
    // Color based on setpoint type
    lv_color_t row_color;
    if (setpoint_type == 0) row_color = lv_color_hex(0x00bcd4);       // Cooling - cyan
    else if (setpoint_type == 2) row_color = lv_color_hex(0xef4444); // Hot water - red/orange
    else row_color = lv_color_hex(0xff9800);                          // Heating - orange
    
    // Setpoint name (left)
    lv_obj_t* name_lbl = lv_label_create(row);
    lv_label_set_text(name_lbl, i18n_get(sp.name_id));
    lv_obj_set_style_text_font(name_lbl, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(name_lbl, row_color, LV_PART_MAIN);
    lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_width(name_lbl, 400);
    
    // Value (right)
    lv_obj_t* val_lbl = lv_label_create(row);
    lv_label_set_text(val_lbl, "---");
    lv_obj_set_style_text_font(val_lbl, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(val_lbl, row_color, LV_PART_MAIN);
    lv_obj_align(val_lbl, LV_ALIGN_RIGHT_MID, -58, 0);
    
    // Arrow indicator
    lv_obj_t* arrow = lv_label_create(row);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(arrow, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(arrow, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, 0, 0);
    
    *value_label_out = val_lbl;
    return row;
}

static void create_edit_dialog(void) {
    // Full-screen overlay
    state.edit_dialog = lv_obj_create(state.screen);
    lv_obj_set_size(state.edit_dialog, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(state.edit_dialog, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state.edit_dialog, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.edit_dialog, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.edit_dialog, 0, LV_PART_MAIN);
    lv_obj_clear_flag(state.edit_dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(state.edit_dialog, LV_OBJ_FLAG_HIDDEN);
    
    // Content card — a flex column that vertically centres its whole stack and
    // auto-sizes to its content, so the dialog looks balanced no matter how
    // much explanation text a given parameter carries (no fixed height => no
    // dead space, no manual per-param tuning). Children, top→bottom:
    //   (1) title  (2) detail paragraph  (3) +/- value control
    //   (4) live value-meaning / (5) numeric range  (mutually exclusive)
    lv_obj_t* content = lv_obj_create(state.edit_dialog);
    lv_obj_set_width(content, LV_PCT(95));
    lv_obj_set_height(content, LV_SIZE_CONTENT);
    lv_obj_align(content, LV_ALIGN_CENTER, 0, -60);
    lv_obj_set_style_bg_color(content, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(content, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(content, 30, LV_PART_MAIN);
    lv_obj_set_style_pad_row(content, 22, LV_PART_MAIN);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    
    // (1) Title — the clean parameter name, e.g. "Frequency Ratio K1 (AP14)".
    state.edit_title = lv_label_create(content);
    lv_label_set_text(state.edit_title, i18n_get(STR_HP_EDIT_PARAMETER));
    lv_obj_set_style_text_font(state.edit_title, UI_FONT_DIALOG_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.edit_title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_width(state.edit_title, LV_PCT(100));
    lv_obj_set_style_text_align(state.edit_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(state.edit_title, LV_LABEL_LONG_WRAP);
    
    // (2) Detail — the full plain-language explanation of what the parameter
    // does and when it applies (sourced from the shared macon library, so an
    // installer can operate the control without the vendor manual). Hidden when
    // a dialog has no detail text (e.g. plain setpoints).
    state.edit_detail = lv_label_create(content);
    lv_label_set_text(state.edit_detail, "");
    lv_obj_set_style_text_font(state.edit_detail, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.edit_detail, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_width(state.edit_detail, LV_PCT(100));
    lv_label_set_long_mode(state.edit_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(state.edit_detail, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    
    // (3) Value display with +/- buttons — the focal control.
    lv_obj_t* value_row = lv_obj_create(content);
    lv_obj_set_size(value_row, 400, 120);
    lv_obj_set_style_bg_opa(value_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(value_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(value_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(value_row, LV_OBJ_FLAG_SCROLLABLE);
    
    // Minus button
    state.edit_minus_btn = lv_btn_create(value_row);
    lv_obj_set_size(state.edit_minus_btn, 100, 100);
    lv_obj_align(state.edit_minus_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(state.edit_minus_btn, lv_color_hex(0x1a2a4e), LV_PART_MAIN);
    lv_obj_set_style_radius(state.edit_minus_btn, 12, LV_PART_MAIN);
    lv_obj_add_event_cb(state.edit_minus_btn, edit_minus_cb, LV_EVENT_CLICKED, nullptr);
    
    lv_obj_t* minus_lbl = lv_label_create(state.edit_minus_btn);
    lv_label_set_text(minus_lbl, "-");
    lv_obj_set_style_text_font(minus_lbl, &montserrat_32_latin, LV_PART_MAIN);
    lv_obj_center(minus_lbl);
    
    // Value label
    state.edit_value_label = lv_label_create(value_row);
    lv_label_set_text(state.edit_value_label, "25");
    lv_obj_set_style_text_font(state.edit_value_label, &montserrat_32_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.edit_value_label, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(state.edit_value_label, LV_ALIGN_CENTER, 0, 0);
    
    // Plus button
    state.edit_plus_btn = lv_btn_create(value_row);
    lv_obj_set_size(state.edit_plus_btn, 100, 100);
    lv_obj_align(state.edit_plus_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(state.edit_plus_btn, lv_color_hex(0x1a2a4e), LV_PART_MAIN);
    lv_obj_set_style_radius(state.edit_plus_btn, 12, LV_PART_MAIN);
    lv_obj_add_event_cb(state.edit_plus_btn, edit_plus_cb, LV_EVENT_CLICKED, nullptr);
    
    lv_obj_t* plus_lbl = lv_label_create(state.edit_plus_btn);
    lv_label_set_text(plus_lbl, "+");
    lv_obj_set_style_text_font(plus_lbl, &montserrat_32_latin, LV_PART_MAIN);
    lv_obj_center(plus_lbl);
    
    // (4) Live value-meaning — helper text that updates with the +/- control
    // (e.g. K-ratio "Reduce 2-step opening each 4 Hz"), or a plain setpoint
    // description. Sits directly below the control. Hidden when empty.
    state.edit_description = lv_label_create(content);
    lv_label_set_text(state.edit_description, "");
    lv_obj_set_style_text_font(state.edit_description, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.edit_description, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_width(state.edit_description, LV_PCT(100));
    lv_label_set_long_mode(state.edit_description, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(state.edit_description, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    
    // (5) Range label (numeric params only; hidden for enum/setpoint as needed).
    state.edit_range_label = lv_label_create(content);
    lv_label_set_text(state.edit_range_label, "");
    lv_obj_set_style_text_font(state.edit_range_label, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.edit_range_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_width(state.edit_range_label, LV_PCT(100));
    lv_obj_set_style_text_align(state.edit_range_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // Bottom action bar — worded Cancel (left, ghost) + Save (right, accent).
    // UX convention: primary action bottom-right and visually dominant, dismiss
    // bottom-left and quiet. Committing is always an explicit Save tap; closing
    // via Cancel never writes. In view-only (disconnected) mode the Save button
    // is hidden (see set_edit_dialog_readonly) so only Cancel remains.
    lv_obj_t* action_bar = lv_obj_create(state.edit_dialog);
    lv_obj_set_size(action_bar, LV_PCT(100), 120);
    lv_obj_align(action_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(action_bar, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(action_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(action_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(action_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(action_bar, 30, LV_PART_MAIN);
    lv_obj_clear_flag(action_bar, LV_OBJ_FLAG_SCROLLABLE);

    // Cancel — ghost/outline, left, quiet.
    lv_obj_t* cancel_btn = lv_btn_create(action_bar);
    lv_obj_set_size(cancel_btn, 300, 80);
    lv_obj_align(cancel_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_radius(cancel_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(cancel_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(cancel_btn, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(cancel_btn, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_add_event_cb(cancel_btn, edit_cancel_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_user_data(cancel_btn, (void*)"edit_cancel");

    lv_obj_t* cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, i18n_get(STR_CANCEL));
    lv_obj_set_style_text_font(cancel_lbl, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(cancel_lbl, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_center(cancel_lbl);

    // Save — filled accent, right, primary/dominant.
    lv_obj_t* save_btn = lv_btn_create(action_bar);
    state.edit_save_btn = save_btn;
    lv_obj_set_size(save_btn, 300, 80);
    lv_obj_align(save_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(save_btn, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(save_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(save_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(save_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(save_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(save_btn, edit_save_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_user_data(save_btn, (void*)"edit_save");

    lv_obj_t* save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, i18n_get(STR_SAVE));
    lv_obj_set_style_text_font(save_lbl, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(save_lbl, COLOR_BG, LV_PART_MAIN);
    lv_obj_center(save_lbl);
}

// Toggle the edit dialog between interactive and view-only (read-only) modes.
// View-only hides the +/- steppers and the Save button (leaving only Cancel), so
// the user never triggers a doomed write while disconnected. The reason is shown
// once by the control-screen disconnected banner, not repeated per dialog.
static void set_edit_dialog_readonly(bool readonly) {
    if (readonly) {
        if (state.edit_minus_btn) lv_obj_add_flag(state.edit_minus_btn, LV_OBJ_FLAG_HIDDEN);
        if (state.edit_plus_btn)  lv_obj_add_flag(state.edit_plus_btn, LV_OBJ_FLAG_HIDDEN);
        if (state.edit_save_btn)  lv_obj_add_flag(state.edit_save_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (state.edit_minus_btn) lv_obj_remove_flag(state.edit_minus_btn, LV_OBJ_FLAG_HIDDEN);
        if (state.edit_plus_btn)  lv_obj_remove_flag(state.edit_plus_btn, LV_OBJ_FLAG_HIDDEN);
        if (state.edit_save_btn)  lv_obj_remove_flag(state.edit_save_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static void close_btn_cb(lv_event_t* e) {
    (void)e;
    ESP_LOGI(TAG, "Close button clicked");
    heatpump_control_hide();
}

static void setpoint_row_cb(lv_event_t* e) {
    lv_obj_t* row = (lv_obj_t*)lv_event_get_target(e);
    int user_data = (int)(intptr_t)lv_obj_get_user_data(row);
    int setpoint_type = user_data - 1000;  // Decode from 1000+ encoding
    if (setpoint_type >= 0 && setpoint_type <= 2) {
        show_setpoint_edit(setpoint_type);
    }
}

static void edit_cancel_cb(lv_event_t* e) {
    (void)e;
    hide_edit_dialog();
}

static void edit_save_cb(lv_event_t* e) {
    (void)e;

    // Advanced ("AP") parameter save — routed through the arctic-macon guardrail.
    if (state.current_ap >= 0) {
        uint8_t ap = (uint8_t)state.current_ap;
        int16_t save_val = (int16_t)roundf(state.edit_value);
        bool bus_ok = false;
        arctic::AdvWriteResult r = advanced_param_write(ap, save_val, &bus_ok);
        if (r == arctic::AdvWriteResult::OK && bus_ok) {
            ESP_LOGI(TAG, "Saved AP%u = %d", (unsigned)ap, save_val);
            // Refresh the row label for this AP.
            for (int i = 0; i < state.ap_display_count; i++) {
                if (state.ap_display_nums[i] == ap) { ap_update_display(i); break; }
            }
        } else {
            ESP_LOGE(TAG, "Failed to save AP%u: %s", (unsigned)ap,
                     arctic::adv_write_result_name(r));
            show_settings_write_error(i18n_get(STR_HP_CANNOT_SAVE));
        }
        hide_edit_dialog();
        return;
    }

    if (state.current_setpoint_type >= 0 && state.current_setpoint_type <= 2) {
        // Saving a setpoint - convert F back to C (setpoints are always TEMP_ABSOLUTE)
        int16_t celsius_val = app_prefs_temp_to_celsius_from_f(state.edit_value);
        
        arctic::HeatPumpState hp = arctic::getState();
        bool success = false;
        
        if (app_prefs_is_demo_mode()) {
            // Demo mode - store in memory
            ESP_LOGI(TAG, "[DEMO] Storing setpoint %d = %d°C (from %.0f display units)", 
                     state.current_setpoint_type, celsius_val, state.edit_value);
            set_demo_setpoint(state.current_setpoint_type, celsius_val);
            success = true;
        } else if (!hp.connected) {
            // Disconnected - show error
            show_settings_write_error(i18n_get(STR_HP_CANNOT_SAVE_SETPOINT));
            hide_edit_dialog();
            return;
        } else {
            switch (state.current_setpoint_type) {
                case 0: success = arctic::setCoolingSetpoint(celsius_val); break;
                case 1: success = arctic::setHeatingSetpoint(celsius_val); break;
                case 2: success = arctic::setHotWaterSetpoint(celsius_val); break;
            }
        }
        
        if (success) {
            ESP_LOGI(TAG, "Saved setpoint %d = %d°C", state.current_setpoint_type, celsius_val);
            
            // Update the displayed value in the list (show in display units)
            lv_obj_t** label_ptr = nullptr;
            switch (state.current_setpoint_type) {
                case 0: label_ptr = &state.cooling_value_label; break;
                case 1: label_ptr = &state.heating_value_label; break;
                case 2: label_ptr = &state.hotwater_value_label; break;
            }
            if (label_ptr && *label_ptr) {
                char buf[32];
                int display_val = (int)roundf(state.edit_value);
                snprintf(buf, sizeof(buf), "%d %s", display_val, app_prefs_temp_unit_str());
                lv_label_set_text(*label_ptr, buf);
            }
        } else {
            ESP_LOGE(TAG, "Failed to save setpoint %d", state.current_setpoint_type);
        }
        
        hide_edit_dialog();
        return;
    }
    
    hide_edit_dialog();
}

static void edit_minus_cb(lv_event_t* e) {
    (void)e;

    // Advanced ("AP") param: enum-aware / integer stepping.
    if (state.current_ap >= 0) {
        const arctic::AdvancedParam* p = arctic::advanced_param_lookup((uint8_t)state.current_ap);
        if (!p) return;
        int cur = (int)roundf(state.edit_value);
        if (p->enum_vals) {
            int idx = -1;
            for (uint8_t i = 0; i < p->enum_count; i++) {
                if ((int)p->enum_vals[i] == cur) { idx = i; break; }
            }
            if (idx > 0) state.edit_value = (float)p->enum_vals[idx - 1];
        } else if (cur > p->min_val) {
            state.edit_value -= 1.0f;
        }
        update_edit_value_display();
        return;
    }

    float min_val = 0;
    
    if (state.current_setpoint_type >= 0 && state.current_setpoint_type <= 2) {
        // Setpoints are always TEMP_ABSOLUTE - convert limit to display units
        min_val = app_prefs_convert_temp_f(s_setpoints[state.current_setpoint_type].min_val);
    } else {
        return;
    }
    
    if (state.edit_value > min_val) {
        state.edit_value -= 1.0f;
        update_edit_value_display();
    }
}

static void edit_plus_cb(lv_event_t* e) {
    (void)e;

    // Advanced ("AP") param: enum-aware / integer stepping.
    if (state.current_ap >= 0) {
        const arctic::AdvancedParam* p = arctic::advanced_param_lookup((uint8_t)state.current_ap);
        if (!p) return;
        int cur = (int)roundf(state.edit_value);
        if (p->enum_vals) {
            int idx = -1;
            for (uint8_t i = 0; i < p->enum_count; i++) {
                if ((int)p->enum_vals[i] == cur) { idx = i; break; }
            }
            if (idx >= 0 && idx < (int)p->enum_count - 1) {
                state.edit_value = (float)p->enum_vals[idx + 1];
            }
        } else if (cur < p->max_val) {
            state.edit_value += 1.0f;
        }
        update_edit_value_display();
        return;
    }

    float max_val = 0;
    
    if (state.current_setpoint_type >= 0 && state.current_setpoint_type <= 2) {
        // Setpoints are always TEMP_ABSOLUTE - convert limit to display units
        max_val = app_prefs_convert_temp_f(s_setpoints[state.current_setpoint_type].max_val);
    } else {
        return;
    }
    
    if (state.edit_value < max_val) {
        state.edit_value += 1.0f;
        update_edit_value_display();
    }
}

// Set a flex-child label's text, hiding it entirely when the text is empty so
// the flex column doesn't reserve a blank row (keeps the card tight/centred).
static void edit_label_set_or_hide(lv_obj_t* lbl, const char* text) {
    if (!lbl) return;
    if (text && text[0]) {
        lv_label_set_text(lbl, text);
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_setpoint_edit(int setpoint_type) {
    if (setpoint_type < 0 || setpoint_type > 2) return;
    sync_setpoint_limits();
    
    state.current_setpoint_type = setpoint_type;
    const SetpointDef& sp = s_setpoints[setpoint_type];
    
    // Read current value in Celsius
    int16_t celsius_val;
    arctic::HeatPumpState hp = arctic::getState();
    state.edit_value_unknown = false;
    if (app_prefs_is_demo_mode()) {
        // Demo mode - use in-memory value
        celsius_val = get_demo_setpoint(setpoint_type);
    } else if (!hp.connected) {
        // Disconnected - no live value; seed midpoint but flag as unknown so the
        // display shows "--" rather than a placeholder that looks like a reading.
        celsius_val = (sp.min_val + sp.max_val) / 2;
        state.edit_value_unknown = true;
    } else {
        switch (setpoint_type) {
            case 0: celsius_val = hp.cooling_setpoint; break;
            case 1: celsius_val = hp.heating_setpoint; break;
            case 2: celsius_val = hp.hot_water_setpoint; break;
            default: celsius_val = sp.min_val; break;
        }
    }
    
    // Store Celsius value and convert to display units
    state.edit_value_celsius = celsius_val;
    state.edit_value = app_prefs_convert_temp_f(celsius_val);
    
    // Update dialog content
    lv_label_set_text(state.edit_title, i18n_get(sp.name_id));
    edit_label_set_or_hide(state.edit_detail, "");  // setpoints have no detail paragraph
    edit_label_set_or_hide(state.edit_description, sp.description);
    
    // Range label - convert limits to display units
    char range_buf[64];
    int display_min = app_prefs_convert_temp(sp.min_val);
    int display_max = app_prefs_convert_temp(sp.max_val);
    snprintf(range_buf, sizeof(range_buf), "%s %d - %d %s", 
             i18n_get(STR_HP_RANGE_FMT), display_min, display_max, app_prefs_temp_unit_str());
    lv_label_set_text(state.edit_range_label, range_buf);
    lv_obj_remove_flag(state.edit_range_label, LV_OBJ_FLAG_HIDDEN);  // shared w/ AP enum dialog which hides it
    
    update_edit_value_display();
    
    // Disconnected heat pump: present the setpoint as view-only rather than
    // letting the user adjust it and hit a write error on Save.
    {
        arctic::HeatPumpState hp = arctic::getState();
        set_edit_dialog_readonly(!hp.connected && !app_prefs_is_demo_mode());
    }

    // Show dialog
    lv_obj_remove_flag(state.edit_dialog, LV_OBJ_FLAG_HIDDEN);
}

static void hide_edit_dialog(void) {
    lv_obj_add_flag(state.edit_dialog, LV_OBJ_FLAG_HIDDEN);
    state.current_setpoint_type = -1;
    state.current_ap = -1;
}

static void update_edit_value_display(void) {
    const char* unit = "";
    int display_val = (int)roundf(state.edit_value);  // Round float to int for display

    // Disconnected/unavailable: live values are never cached, so there is nothing
    // real to show — display "--" rather than a fabricated default, and clear any
    // enum meaning line.
    if (state.edit_value_unknown) {
        lv_label_set_text(state.edit_value_label, "--");
        if (state.current_ap >= 0 && state.edit_description) {
            const arctic::AdvancedParam* p = arctic::advanced_param_lookup((uint8_t)state.current_ap);
            if (p && p->enum_vals) lv_label_set_text(state.edit_description, "");
        }
        return;
    }

    // Advanced ("AP") param: K-ratio codes render as their display decimal,
    // with a live plain-language description below (both library-sourced).
    if (state.current_ap >= 0) {
        const arctic::AdvancedParam* p = arctic::advanced_param_lookup((uint8_t)state.current_ap);
        char val_buf[32];
        if (p && p->enum_vals) {
            const char* s = kratio_label((uint8_t)state.current_ap, display_val);
            if (s) snprintf(val_buf, sizeof(val_buf), "%s", s);
            else   snprintf(val_buf, sizeof(val_buf), "%d", display_val);
            // Update the description line to match the selected value.
            if (state.edit_description) {
                char desc_buf[96];
                kratio_desc((uint8_t)state.current_ap, display_val, desc_buf, sizeof(desc_buf));
                lv_label_set_text(state.edit_description, desc_buf);
            }
        } else if (p && p->unit && p->unit[0]) {
            snprintf(val_buf, sizeof(val_buf), "%d %s", display_val, p->unit);
        } else {
            snprintf(val_buf, sizeof(val_buf), "%d", display_val);
        }
        lv_label_set_text(state.edit_value_label, val_buf);
        return;
    }

    if (state.current_setpoint_type >= 0 && state.current_setpoint_type <= 2) {
        // Setpoints are always TEMP_ABSOLUTE
        unit = app_prefs_temp_unit_str();
    } else {
        return;
    }
    
    char val_buf[32];
    if (unit[0]) {
        snprintf(val_buf, sizeof(val_buf), "%d %s", display_val, unit);
    } else {
        snprintf(val_buf, sizeof(val_buf), "%d", display_val);
    }
    lv_label_set_text(state.edit_value_label, val_buf);
}

// ============================================================================
// Advanced ("AP") Parameter Section
// ============================================================================
// Driven entirely by the shared arctic-macon advanced-param table. Only
// change-and-capture verified registers are shown; the safe writable subset
// (AP13-20) is click-to-edit, the manual-override block (AP48-51) is displayed
// read-only. K-ratio codes render as their vendor display decimal + a live
// plain-language description; both come from the shared macon library so the
// controller never hardcodes the wire-code<->meaning mapping.

// K-ratio display label for a wire code (e.g. 12 -> "3"), sourced from the
// macon library (single source of truth). Returns nullptr for a non-option.
static const char* kratio_label(uint8_t ap, int wire) {
    const arctic::AdvEnumOption* o =
        arctic::advanced_enum_option_for_wire(ap, (int16_t)wire);
    return o ? o->label : nullptr;
}

// Localized human description of a wire code ("Reduce 6-step opening each
// 1 Hz"), built from the library's structured args (steps, Hz) + the device
// i18n catalog. Falls back to the library's canonical English if untranslated.
static void kratio_desc(uint8_t ap, int wire, char* buf, size_t n) {
    const arctic::AdvEnumOption* o =
        arctic::advanced_enum_option_for_wire(ap, (int16_t)wire);
    if (!o) { if (n) buf[0] = '\0'; return; }
    if (strcmp(o->msg_id, "kratio_none") == 0) {
        snprintf(buf, n, "%s", i18n_get(STR_HP_KRATIO_NONE));
    } else if (strcmp(o->msg_id, "kratio_reduce") == 0) {
        snprintf(buf, n, i18n_get(STR_HP_KRATIO_REDUCE), (int)o->arg_a, (int)o->arg_b);
    } else {
        snprintf(buf, n, "%s", o->en_default ? o->en_default : "");
    }
}

// Format a AP parameter's raw register value into a display string.
static void format_ap_value(const arctic::AdvancedParam* p, int16_t raw,
                            char* buf, size_t n) {
    if (p->enum_vals) {
        const char* s = kratio_label(p->ap, raw);
        if (s) snprintf(buf, n, "%s", s);
        else   snprintf(buf, n, "%d", raw);
    } else if (p->unit && p->unit[0]) {
        snprintf(buf, n, "%d %s", raw, p->unit);
    } else {
        snprintf(buf, n, "%d", raw);
    }
}

// Refresh the live value label for the AP row at display slot `slot`.
static void ap_update_display(int slot) {
    if (slot < 0 || slot >= state.ap_display_count) return;
    if (!state.ap_value_labels[slot]) return;
    uint8_t ap = state.ap_display_nums[slot];
    const arctic::AdvancedParam* p = arctic::advanced_param_lookup(ap);
    if (!p) return;

    // Momentary command registers do not latch a stored value — show a static
    // "Ready" affordance instead of the (always-0) register read-back.
    if (p->is_trigger) {
        lv_label_set_text(state.ap_value_labels[slot], "Ready");
        return;
    }

    int16_t val = 0;
    if (advanced_param_read(ap, &val)) {
        char buf[32];
        format_ap_value(p, val, buf, sizeof(buf));
        lv_label_set_text(state.ap_value_labels[slot], buf);
    } else {
        lv_label_set_text(state.ap_value_labels[slot], "--");
    }
}

static void ap_row_cb(lv_event_t* e) {
    lv_obj_t* row = (lv_obj_t*)lv_event_get_target(e);
    uint8_t ap = (uint8_t)(intptr_t)lv_obj_get_user_data(row);
    const arctic::AdvancedParam* p = arctic::advanced_param_lookup(ap);
    if (p && p->is_trigger) {
        show_ap_trigger_confirm(ap);
    } else {
        show_ap_edit_dialog(ap);
    }
}

static void create_ap_row(lv_obj_t* parent, const arctic::AdvancedParam* p) {
    if (state.ap_display_count >= 64) return;
    const int slot = state.ap_display_count;
    const bool reg_known = (p->reg != arctic::ADV_REG_UNKNOWN);
    // Three interactive kinds + a passive locked kind:
    //   trigger   -> tappable, fires a momentary command (confirm dialog)
    //   read_only -> reg known but purpose unclear: displayed, not tappable
    //   writable  -> tappable stepper edit
    //   locked    -> manual block (needs_sim_confirm): displayed, not tappable
    const bool is_trigger = reg_known && p->is_trigger;
    const bool read_only  = reg_known && p->read_only;
    const bool writable   = reg_known && !p->needs_sim_confirm && !read_only && !is_trigger;
    const bool clickable  = writable || is_trigger;
    const bool active_col = writable || is_trigger;  // accent vs dim text

    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 70);
    lv_obj_set_style_bg_color(row, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, COLOR_CARD_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 20, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    if (clickable) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void*)(intptr_t)p->ap);
        lv_obj_add_event_cb(row, ap_row_cb, LV_EVENT_CLICKED, nullptr);
    }

    // Name with AP number (left)
    char name_buf[80];
    snprintf(name_buf, sizeof(name_buf), "%s (AP%u)", i18n_get_key(p->name_msg_id, p->name), (unsigned)p->ap);
    lv_obj_t* name_lbl = lv_label_create(row);
    lv_label_set_text(name_lbl, name_buf);
    lv_obj_set_style_text_font(name_lbl, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(name_lbl, active_col ? COLOR_TEXT : COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_width(name_lbl, 430);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);

    // Value (right)
    lv_obj_t* val_lbl = lv_label_create(row);
    lv_label_set_text(val_lbl, "---");
    lv_obj_set_style_text_font(val_lbl, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(val_lbl, active_col ? COLOR_ACCENT : COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(val_lbl, LV_ALIGN_RIGHT_MID, -58, 0);

    // Indicator glyph, per kind:
    //   trigger   -> play (action), accent
    //   writable  -> right arrow (edit), dim
    //   read_only -> eye (view-only), dim
    //   locked    -> warning, warning color
    const char* glyph;
    lv_color_t  glyph_col;
    if (is_trigger)      { glyph = LV_SYMBOL_PLAY;    glyph_col = COLOR_ACCENT; }
    else if (writable)   { glyph = LV_SYMBOL_RIGHT;   glyph_col = COLOR_TEXT_DIM; }
    else if (read_only)  { glyph = LV_SYMBOL_EYE_OPEN;glyph_col = COLOR_TEXT_DIM; }
    else                 { glyph = LV_SYMBOL_WARNING; glyph_col = COLOR_WARNING; }
    lv_obj_t* ind = lv_label_create(row);
    lv_label_set_text(ind, glyph);
    lv_obj_set_style_text_font(ind, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(ind, glyph_col, LV_PART_MAIN);
    lv_obj_align(ind, LV_ALIGN_RIGHT_MID, 0, 0);

    state.ap_value_labels[slot] = val_lbl;
    state.ap_display_nums[slot] = p->ap;
    state.ap_display_count++;
}

static void create_ap_section(lv_obj_t* parent) {
    state.ap_display_count = 0;

    // One top-level header, then rows grouped in canonical category order.
    create_section_header(parent, "Advanced Parameters");

    const size_t ncat = arctic::advanced_category_count();
    const size_t nparam = arctic::advanced_param_count();
    for (size_t c = 0; c < ncat; c++) {
        const char* cat = arctic::advanced_category_at(c);
        bool header_done = false;
        for (size_t i = 0; i < nparam; i++) {
            const arctic::AdvancedParam* p = arctic::advanced_param_at(i);
            if (!p || !cat) continue;
            if (strcmp(p->category, cat) != 0) continue;
            if (p->reg == arctic::ADV_REG_UNKNOWN) continue;  // hide unverified
            if (!header_done) {
                create_section_header(parent, cat);
                header_done = true;
            }
            create_ap_row(parent, p);
        }
    }
}

// Substitute {T:<celsius>} tokens in a library detail template with the value
// converted to the user's chosen unit (°C/°F) plus the unit suffix. Canonical
// temperatures live in the library as Celsius; unit conversion is a presentation
// concern handled here so the library stays unit-agnostic. Non-token text is
// copied verbatim, so any detail without tokens passes through unchanged.
static void format_detail_temps(const char* tmpl, char* out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!tmpl) return;

    size_t o = 0;
    const char* s = tmpl;
    while (*s && o < out_sz - 1) {
        if (s[0] == '{' && s[1] == 'T' && s[2] == ':') {
            const char* p = s + 3;
            bool neg = false;
            if (*p == '-') { neg = true; p++; }
            if (*p >= '0' && *p <= '9') {
                int val = 0;
                while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); p++; }
                if (*p == '}') {
                    int16_t c = (int16_t)(neg ? -val : val);
                    int written = snprintf(out + o, out_sz - o, "%d%s",
                                           (int)app_prefs_convert_temp(c),
                                           app_prefs_temp_unit_str());
                    if (written > 0) {
                        o += ((size_t)written < out_sz - o) ? (size_t)written
                                                            : (out_sz - o - 1);
                    }
                    s = p + 1;
                    continue;
                }
            }
        }
        out[o++] = *s++;
    }
    out[o] = '\0';
}

static void show_ap_edit_dialog(uint8_t ap) {
    const arctic::AdvancedParam* p = arctic::advanced_param_lookup(ap);
    if (!p || p->reg == arctic::ADV_REG_UNKNOWN || p->needs_sim_confirm) return;

    state.current_ap = ap;
    state.current_setpoint_type = -1;

    // Seed with the live value. When the read fails (disconnected) fall back to
    // the vendor default for range/stepping math, but flag the value as unknown
    // so the display shows "--" instead of presenting the default as a reading.
    int16_t v = 0;
    bool read_ok = advanced_param_read(ap, &v);
    if (!read_ok) v = p->default_val;
    state.edit_value = (float)v;
    state.edit_value_celsius = v;
    state.edit_value_unknown = !read_ok;

    char title_buf[80];
    snprintf(title_buf, sizeof(title_buf), "%s (AP%u)", i18n_get_key(p->name_msg_id, p->name), (unsigned)ap);
    lv_label_set_text(state.edit_title, title_buf);

    // Full plain-language explanation of the parameter (library-sourced English,
    // localized via detail_msg_id where a translation exists). {T:<c>} tokens are
    // expanded to the user's chosen unit at render time.
    char detail_buf[320];
    format_detail_temps(i18n_get_key(p->detail_msg_id, p->detail), detail_buf, sizeof(detail_buf));
    edit_label_set_or_hide(state.edit_detail, detail_buf);

    // The value-meaning line is filled live by update_edit_value_display() for
    // enum (K-ratio) params; ensure it's visible. Plain numeric params have no
    // value-meaning, so hide it.
    if (p->enum_vals) {
        lv_obj_remove_flag(state.edit_description, LV_OBJ_FLAG_HIDDEN);
    } else {
        edit_label_set_or_hide(state.edit_description, "");
    }

    // Range hint. For enum (K-ratio) params a min-max range is meaningless —
    // the value is one of a few discrete options, each explained by the live
    // description line — so hide it. Numeric params keep the min-max hint.
    if (p->enum_vals) {
        lv_obj_add_flag(state.edit_range_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        char range_buf[64];
        if (p->unit && p->unit[0]) {
            snprintf(range_buf, sizeof(range_buf), "%s %d - %d %s",
                     i18n_get(STR_HP_RANGE_FMT), p->min_val, p->max_val, p->unit);
        } else {
            snprintf(range_buf, sizeof(range_buf), "%s %d - %d",
                     i18n_get(STR_HP_RANGE_FMT), p->min_val, p->max_val);
        }
        lv_label_set_text(state.edit_range_label, range_buf);
        lv_obj_remove_flag(state.edit_range_label, LV_OBJ_FLAG_HIDDEN);
    }

    update_edit_value_display();

    // Disconnected heat pump: view-only so the user can read the parameter and
    // its explanation but can't trigger a doomed write that only errors out.
    {
        arctic::HeatPumpState hp = arctic::getState();
        set_edit_dialog_readonly(!hp.connected && !app_prefs_is_demo_mode());
    }

    lv_obj_remove_flag(state.edit_dialog, LV_OBJ_FLAG_HIDDEN);
}

// Momentary "trigger" AP params (e.g. AP47 water-system cleaning) do not store
// a value — they are self-clearing command registers.  Tapping the row opens a
// confirmation before firing the command (writes the trigger value 1).
static uint8_t s_trigger_ap = 0;

static void ap_trigger_run_cb(lv_event_t* e) {
    lv_obj_t* mbox = (lv_obj_t*)lv_event_get_user_data(e);
    uint8_t ap = s_trigger_ap;
    bool bus_ok = false;
    arctic::AdvWriteResult r = advanced_param_write(ap, 1, &bus_ok);
    if (r != arctic::AdvWriteResult::OK || !bus_ok) {
        ESP_LOGE(TAG, "Trigger AP%u failed: %s", (unsigned)ap,
                 arctic::adv_write_result_name(r));
        show_settings_write_error(i18n_get(STR_HP_CANNOT_SAVE));
    } else {
        ESP_LOGI(TAG, "Triggered AP%u (momentary command)", (unsigned)ap);
    }
    if (mbox) lv_msgbox_close(mbox);
}

static void show_ap_trigger_confirm(uint8_t ap) {
    const arctic::AdvancedParam* p = arctic::advanced_param_lookup(ap);
    if (!p || !p->is_trigger || p->reg == arctic::ADV_REG_UNKNOWN) return;
    s_trigger_ap = ap;

    lv_obj_t* mbox = lv_msgbox_create(lv_layer_top());
    char title[80];
    snprintf(title, sizeof(title), "%s (AP%u)", i18n_get_key(p->name_msg_id, p->name), (unsigned)ap);
    lv_msgbox_add_title(mbox, title);
    lv_msgbox_add_text(mbox,
        "Run this momentary command now? It starts immediately and does not "
        "store a value.");
    lv_obj_t* run_btn = lv_msgbox_add_footer_button(mbox, "Run");
    lv_obj_add_event_cb(run_btn, ap_trigger_run_cb, LV_EVENT_CLICKED, mbox);
    lv_msgbox_add_close_button(mbox);  // X acts as Cancel
    lv_obj_center(mbox);
    lv_obj_set_width(mbox, 460);
}

// ============================================================================
// Public Functions
// ============================================================================

void heatpump_control_create_in(lv_obj_t* parent) {
    if (state.shown) {
        return;
    }

    ESP_LOGI(TAG, "Building control tab");
    state.on_close = nullptr;

    // The panel provided by the tab shell is our root; build directly into it.
    state.screen = parent;

    // Scrollable content fills the panel; reserve room for the persistent nav
    // bar (drawn by the tab shell) at the bottom.
    state.scroll_container = lv_obj_create(state.screen);
    lv_obj_set_size(state.scroll_container, LV_PCT(100), LV_PCT(100));
    lv_obj_align(state.scroll_container, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(state.scroll_container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.scroll_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.scroll_container, 15, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(state.scroll_container, NAV_BAR_H + 15, LV_PART_MAIN);
    lv_obj_set_flex_flow(state.scroll_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(state.scroll_container, 10, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(state.scroll_container, LV_SCROLLBAR_MODE_AUTO);
    
    // =========================================================================
    // DISCONNECTED BANNER — a single screen-level notice (first child) shown
    // when the heat pump isn't connected, so we don't repeat the message inside
    // every parameter/setpoint dialog. Toggled by power_update_timer_cb. Hidden
    // by default; visibility is reconciled on show + every 2 s.
    // =========================================================================
    state.disconnected_banner = lv_obj_create(state.scroll_container);
    lv_obj_set_size(state.disconnected_banner, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(state.disconnected_banner, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state.disconnected_banner, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.disconnected_banner, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(state.disconnected_banner, COLOR_ERROR, LV_PART_MAIN);
    lv_obj_set_style_radius(state.disconnected_banner, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.disconnected_banner, 16, LV_PART_MAIN);
    lv_obj_clear_flag(state.disconnected_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(state.disconnected_banner, (void*)"disconnected_banner");
    lv_obj_add_flag(state.disconnected_banner, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* banner_lbl = lv_label_create(state.disconnected_banner);
    char banner_buf[96];
    snprintf(banner_buf, sizeof(banner_buf), LV_SYMBOL_WARNING "  %s", i18n_get(STR_HP_NOT_CONNECTED));
    lv_label_set_text(banner_lbl, banner_buf);
    lv_obj_set_style_text_font(banner_lbl, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(banner_lbl, COLOR_ERROR, LV_PART_MAIN);
    lv_obj_set_width(banner_lbl, LV_PCT(100));
    lv_label_set_long_mode(banner_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(banner_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(banner_lbl);

    // =========================================================================
    // POWER BUTTON (prominent at top of advanced screen)
    // =========================================================================
    state.power_btn = lv_btn_create(state.scroll_container);
    lv_obj_set_size(state.power_btn, LV_PCT(100), 80);
    lv_obj_set_style_bg_color(state.power_btn, COLOR_ERROR, LV_PART_MAIN);
    lv_obj_set_style_radius(state.power_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(state.power_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(state.power_btn, power_btn_event_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(state.power_btn, power_btn_event_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(state.power_btn, power_btn_event_cb, LV_EVENT_PRESS_LOST, nullptr);
    lv_obj_add_event_cb(state.power_btn, power_btn_event_cb, LV_EVENT_CLICKED, nullptr);
    
    // Progress bar (hidden until hold starts)
    state.power_hold_bar = lv_bar_create(state.power_btn);
    lv_obj_set_size(state.power_hold_bar, LV_PCT(92), 8);
    lv_obj_align(state.power_hold_bar, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_bar_set_range(state.power_hold_bar, 0, 100);
    lv_bar_set_value(state.power_hold_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(state.power_hold_bar, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state.power_hold_bar, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_bg_color(state.power_hold_bar, COLOR_TEXT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(state.power_hold_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(state.power_hold_bar, 4, LV_PART_INDICATOR);
    lv_obj_add_flag(state.power_hold_bar, LV_OBJ_FLAG_HIDDEN);
    
    state.power_btn_label = lv_label_create(state.power_btn);
    lv_label_set_text(state.power_btn_label, i18n_get(STR_HP_POWER_OFF));
    lv_obj_set_style_text_font(state.power_btn_label, UI_FONT_HEADER, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.power_btn_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(state.power_btn_label, LV_ALIGN_CENTER, 0, -4);
    
    // Set initial appearance based on current state
    {
        arctic::HeatPumpState hp_state = arctic::getState();
        update_power_btn_appearance(hp_state.connected ? hp_state.unit_on : false);
        if (state.disconnected_banner) {
            bool disconnected = !hp_state.connected && !app_prefs_is_demo_mode();
            if (disconnected) lv_obj_remove_flag(state.disconnected_banner, LV_OBJ_FLAG_HIDDEN);
            else              lv_obj_add_flag(state.disconnected_banner, LV_OBJ_FLAG_HIDDEN);
        }
    }
    
    // Timer to keep power button in sync (2s interval)
    state.power_update_timer = lv_timer_create(power_update_timer_cb, 2000, nullptr);
    
    // =========================================================================
    // MODE SELECTOR (below power button)
    // =========================================================================
    create_section_header(state.scroll_container, i18n_get(STR_HP_MODE));
    
    // Determine current mode index
    {
        arctic::HeatPumpState hp_mode = arctic::getState();
        switch (hp_mode.working_mode) {
            case arctic::WorkingMode::COOLING:          state.active_mode_idx = 0; break;
            case arctic::WorkingMode::FLOOR_HEATING:    state.active_mode_idx = 1; break;
            case arctic::WorkingMode::FAN_COIL_HEATING: state.active_mode_idx = 2; break;
            case arctic::WorkingMode::HOT_WATER:        state.active_mode_idx = 3; break;
            case arctic::WorkingMode::AUTO:             state.active_mode_idx = 4; break;
            default: state.active_mode_idx = 0; break;
        }
    }
    
    // Create a row of mode buttons (2 rows of ~3 for readability)
    lv_obj_t* mode_grid = lv_obj_create(state.scroll_container);
    lv_obj_set_size(mode_grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(mode_grid, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mode_grid, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(mode_grid, COLOR_CARD_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(mode_grid, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(mode_grid, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(mode_grid, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_row(mode_grid, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_column(mode_grid, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(mode_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(mode_grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(mode_grid, LV_OBJ_FLAG_SCROLLABLE);
    
    for (int i = 0; i < 5; i++) {
        lv_obj_t* btn = lv_btn_create(mode_grid);
        // First 3 buttons on row 1 (~215px each), last 2 on row 2 (~330px each)
        int btn_w = (i < 3) ? 210 : 320;
        lv_obj_set_size(btn, btn_w, 60);
        lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
        lv_obj_set_user_data(btn, (void*)(intptr_t)i);
        lv_obj_add_event_cb(btn, mode_btn_event_cb, LV_EVENT_CLICKED, nullptr);
        
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, i18n_get(s_mode_labels[i]));
        lv_obj_set_style_text_font(lbl, UI_FONT_SMALL, LV_PART_MAIN);
        lv_obj_center(lbl);
        
        state.mode_btns[i] = btn;
        state.mode_labels[i] = lbl;
    }
    
    update_mode_btn_styles(state.active_mode_idx);

    // Apply initial enabled/disabled state to the mode buttons based on connection.
    {
        arctic::HeatPumpState hp_conn = arctic::getState();
        bool disconnected = !hp_conn.connected && !app_prefs_is_demo_mode();
        set_mode_controls_enabled(!disconnected);
    }
    create_section_header(state.scroll_container, i18n_get(STR_HP_SETPOINTS));
    
    // Cooling setpoint row
    create_setpoint_row(state.scroll_container, 0, &state.cooling_value_label);
    
    // Heating setpoint row  
    create_setpoint_row(state.scroll_container, 1, &state.heating_value_label);
    
    // Hot water setpoint row
    create_setpoint_row(state.scroll_container, 2, &state.hotwater_value_label);
    
    // =========================================================================
    // ADVANCED PARAMETERS - arctic-macon table, write-verified registers only.
    // (The old "P-parameter" list was an unverified mapping with wrong register
    //  addresses and no sign-decode; it has been retired in favour of this.)
    // =========================================================================
    create_ap_section(state.scroll_container);

    // Create edit dialog (hidden initially)
    create_edit_dialog();
    
    state.shown = true;
    
    // Load setpoint values first (these are quick, just read from cached state)
    arctic::HeatPumpState hp = arctic::getState();
    bool demo_mode = app_prefs_is_demo_mode();
    {
        char buf[32];
        int16_t cooling_sp, heating_sp, hotwater_sp;
        
        if (demo_mode) {
            // Demo mode - simulated values
            cooling_sp = 18;
            heating_sp = 45;
            hotwater_sp = 50;
        } else if (hp.connected) {
            // Real values
            cooling_sp = hp.cooling_setpoint;
            heating_sp = hp.heating_setpoint;
            hotwater_sp = hp.hot_water_setpoint;
        } else {
            // Disconnected - values unknown, will show as 0
            cooling_sp = 0;
            heating_sp = 0;
            hotwater_sp = 0;
        }
        
        if (state.cooling_value_label) {
            if (!demo_mode && !hp.connected) {
                lv_label_set_text(state.cooling_value_label, "--");
            } else {
                snprintf(buf, sizeof(buf), "%d %s", 
                         app_prefs_convert_temp(cooling_sp), app_prefs_temp_unit_str());
                lv_label_set_text(state.cooling_value_label, buf);
            }
        }
        if (state.heating_value_label) {
            if (!demo_mode && !hp.connected) {
                lv_label_set_text(state.heating_value_label, "--");
            } else {
                snprintf(buf, sizeof(buf), "%d %s", 
                         app_prefs_convert_temp(heating_sp), app_prefs_temp_unit_str());
                lv_label_set_text(state.heating_value_label, buf);
            }
        }
        if (state.hotwater_value_label) {
            if (!demo_mode && !hp.connected) {
                lv_label_set_text(state.hotwater_value_label, "--");
            } else {
                snprintf(buf, sizeof(buf), "%d %s", 
                         app_prefs_convert_temp(hotwater_sp), app_prefs_temp_unit_str());
                lv_label_set_text(state.hotwater_value_label, buf);
            }
        }
    }
    
    // Start staggered loading of parameter values
    // Reads one parameter every 100ms to avoid blocking UI
    state.load_index = 0;
    if (demo_mode) {
        // Demo mode: load all simulated values immediately
        ESP_LOGI(TAG, "Using demo values");
        for (int i = 0; i < state.ap_display_count; i++) {
            ap_update_display(i);  // advanced_param_read returns the vendor default in demo
        }
    } else if (!hp.connected) {
        // Disconnected: show placeholder values
        ESP_LOGI(TAG, "Disconnected - showing placeholders");
        for (int i = 0; i < state.ap_display_count; i++) {
            if (state.ap_value_labels[i]) {
                lv_label_set_text(state.ap_value_labels[i], "--");
            }
        }
    } else {
        // Connected: stagger reads to avoid blocking the UI.
        state.load_timer = lv_timer_create(load_timer_cb, 100, nullptr);
    }
    
    // Persistent bottom navigation bar is created by the tab shell, not here.
}

void heatpump_control_set_active(bool active) {
    if (state.power_update_timer) {
        if (active) {
            lv_timer_resume(state.power_update_timer);
        } else {
            lv_timer_pause(state.power_update_timer);
        }
    }
}

void heatpump_control_hide(void) {
    if (!state.shown) {
        return;
    }
    
    ESP_LOGI(TAG, "Hiding heat pump settings screen");
    
    // Stop load timer if running
    if (state.load_timer) {
        lv_timer_del(state.load_timer);
        state.load_timer = nullptr;
    }
    
    // Stop power timers
    if (state.power_hold_timer) {
        lv_timer_del(state.power_hold_timer);
        state.power_hold_timer = nullptr;
    }
    if (state.power_update_timer) {
        lv_timer_del(state.power_update_timer);
        state.power_update_timer = nullptr;
    }
    
    heatpump_control_close_cb_t cb = state.on_close;
    
    // Reset state
    state.shown = false;
    state.on_close = nullptr;
    state.screen = nullptr;
    state.scroll_container = nullptr;
    state.disconnected_banner = nullptr;
    state.power_btn = nullptr;
    state.power_btn_label = nullptr;
    state.power_hold_bar = nullptr;
    state.power_holding = false;
    state.power_hold_completed = false;
    memset(state.mode_btns, 0, sizeof(state.mode_btns));
    memset(state.mode_labels, 0, sizeof(state.mode_labels));
    state.active_mode_idx = 0;
    state.cooling_value_label = nullptr;
    state.heating_value_label = nullptr;
    state.hotwater_value_label = nullptr;
    state.load_index = 0;
    state.edit_dialog = nullptr;
    state.edit_title = nullptr;
    state.edit_detail = nullptr;
    state.edit_description = nullptr;
    state.edit_range_label = nullptr;
    state.edit_value_label = nullptr;
    state.edit_minus_btn = nullptr;
    state.edit_plus_btn = nullptr;
    state.edit_save_btn = nullptr;
    state.current_setpoint_type = -1;
    state.current_ap = -1;
    memset(state.ap_value_labels, 0, sizeof(state.ap_value_labels));
    memset(state.ap_display_nums, 0, sizeof(state.ap_display_nums));
    state.ap_display_count = 0;
    
    // Call callback to load the previous screen (animation will delete this screen)
    if (cb) {
        cb();
    }
}

bool heatpump_control_is_shown(void) {
    return state.shown;
}
