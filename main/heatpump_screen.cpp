/*
 * Arctic Heat Pump Controller
 * Heat Pump Status Display Screen
 */

#include "heatpump_screen.h"
#include "heatpump_temps_screen.h"
#include "heatpump_system_screen.h"
#include "heatpump_control_screen.h"
#include "modbus/arctic_heatpump.h"
#include "modbus/arctic_registers.h"
#include "app_preferences.h"
#include "ui_common.h"
#include <esp_log.h>
#include <stdio.h>

static const char* TAG = "hp_screen";

// Helper: show error popup for write failures when disconnected
static void show_write_error_popup(const char* message) {
    lv_obj_t* msgbox = lv_msgbox_create(lv_layer_top());
    lv_msgbox_add_title(msgbox, "Communication Error");
    lv_msgbox_add_text(msgbox, message);
    lv_msgbox_add_close_button(msgbox);
    lv_obj_center(msgbox);
    lv_obj_set_width(msgbox, 400);
}

// ============================================================================
// UI Elements
// ============================================================================
static struct {
    bool created = false;
    lv_obj_t* container = nullptr;
    
    // Main status card
    lv_obj_t* status_card = nullptr;
    lv_obj_t* connection_indicator = nullptr;
    lv_obj_t* mode_label = nullptr;
    lv_obj_t* tank_temp_label = nullptr;
    lv_obj_t* setpoint_label = nullptr;
    
    // Component status indicators
    lv_obj_t* compressor_indicator = nullptr;
    lv_obj_t* compressor_label = nullptr;
    lv_obj_t* fan_indicator = nullptr;
    lv_obj_t* fan_label = nullptr;
    lv_obj_t* pump_indicator = nullptr;
    lv_obj_t* pump_label = nullptr;
    lv_obj_t* heater_indicator = nullptr;
    lv_obj_t* heater_label = nullptr;
    
    // Control elements
    lv_obj_t* power_btn = nullptr;         // Large power button
    lv_obj_t* power_btn_label = nullptr;   // Power button text
    lv_obj_t* mode_dropdown = nullptr;
    
    // Single active setpoint (changes based on mode)
    lv_obj_t* active_setpoint_name = nullptr;   // "Cooling" / "Heating" / "Hot Water"
    lv_obj_t* active_setpoint_label = nullptr;
    lv_obj_t* active_setpoint_minus = nullptr;
    lv_obj_t* active_setpoint_plus = nullptr;
    
    // Demo mode state (used when heat pump disconnected or demo enabled)
    bool demo_power = true;
    int demo_mode = 1;  // 0=Cool, 1=Floor Heat, 2=Fan Heat, 3=Hot Water, 4=Auto
    int16_t demo_cooling = 18;
    int16_t demo_heating = 45;
    int16_t demo_hotwater = 50;
    
    // Power meter display
    lv_obj_t* power_meter_label = nullptr;
    
    // Error card (prominent)
    lv_obj_t* error_card = nullptr;
    lv_obj_t* error_label = nullptr;
    
    // Bottom button bar
    lv_obj_t* temps_btn = nullptr;
    lv_obj_t* system_btn = nullptr;
    lv_obj_t* controls_btn = nullptr;
    
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
        return "DEFROST";
    }
    switch (mode) {
        case arctic::WorkingMode::COOLING:           return "COOLING";
        case arctic::WorkingMode::FLOOR_HEATING:     return "FLOOR HEAT";
        case arctic::WorkingMode::FAN_COIL_HEATING:  return "FAN HEAT";
        case arctic::WorkingMode::HOT_WATER:         return "HOT WATER";
        case arctic::WorkingMode::AUTO:              return "AUTO";
        default:                                      return "UNKNOWN";
    }
}

static lv_color_t getModeColor(arctic::WorkingMode mode, bool defrosting) {
    if (defrosting) {
        return COLOR_DEFROST;
    }
    switch (mode) {
        case arctic::WorkingMode::COOLING:
            return COLOR_COOLING;
        case arctic::WorkingMode::FLOOR_HEATING:
        case arctic::WorkingMode::FAN_COIL_HEATING:
            return COLOR_HEATING;
        case arctic::WorkingMode::HOT_WATER:
            return COLOR_HOT_WATER;
        default:
            return COLOR_ACCENT;
    }
}

static lv_obj_t* create_status_indicator(lv_obj_t* parent, const char* label_text, lv_obj_t** indicator_out, lv_obj_t** label_out) {
    // Container for indicator + label
    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_set_size(cont, LV_SIZE_CONTENT, 50);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cont, 0, LV_PART_MAIN);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    // Circle indicator (12px diameter)
    lv_obj_t* indicator = lv_obj_create(cont);
    lv_obj_set_size(indicator, 16, 16);
    lv_obj_set_style_radius(indicator, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(indicator, COLOR_INACTIVE, LV_PART_MAIN);
    lv_obj_set_style_border_width(indicator, 0, LV_PART_MAIN);
    lv_obj_clear_flag(indicator, LV_OBJ_FLAG_SCROLLABLE);
    
    // Label below
    lv_obj_t* lbl = lv_label_create(cont);
    lv_label_set_text(lbl, label_text);
    lv_obj_set_style_text_font(lbl, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, COLOR_TEXT_DIM, LV_PART_MAIN);
    
    *indicator_out = indicator;
    *label_out = lbl;
    
    return cont;
}

static void set_indicator_active(lv_obj_t* indicator, bool active, lv_color_t active_color) {
    if (indicator) {
        lv_obj_set_style_bg_color(indicator, active ? active_color : COLOR_INACTIVE, LV_PART_MAIN);
    }
}

// ============================================================================
// Detail Screen Callbacks
// ============================================================================

static void on_temps_close(void) {
    // Load saved screen back with slide-down animation
    // auto_del=true - LVGL will delete the sub-screen after animation
    if (state.saved_screen) {
        lv_screen_load_anim(state.saved_screen, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 400, 0, true);
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
    // Load saved screen back with slide-down animation
    // auto_del=true - LVGL will delete the sub-screen after animation
    if (state.saved_screen) {
        lv_screen_load_anim(state.saved_screen, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 400, 0, true);
        state.saved_screen = nullptr;
    }
}

static void system_btn_cb(lv_event_t* e) {
    // Save current screen
    state.saved_screen = lv_scr_act();
    
    // Show system readings screen
    heatpump_system_show(on_system_close);
}

static void on_control_close(void) {
    // Load saved screen back with slide-down animation
    // auto_del=true - LVGL will delete the sub-screen after animation
    if (state.saved_screen) {
        lv_screen_load_anim(state.saved_screen, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 400, 0, true);
        state.saved_screen = nullptr;
    }
}

static void power_btn_cb(lv_event_t* e) {
    arctic::HeatPumpState hp = arctic::getState();
    bool demo_mode = app_prefs_is_demo_mode();
    
    if (demo_mode) {
        // Demo mode - just update local state
        state.demo_power = !state.demo_power;
        ESP_LOGI(TAG, "[DEMO] Power %s", state.demo_power ? "ON" : "OFF");
        heatpump_screen_update();
        return;
    }
    
    if (!hp.connected) {
        // Disconnected - show error popup
        show_write_error_popup("Cannot control power: Heat pump not connected");
        return;
    }
    
    arctic::setUnitPower(!hp.unit_on);
}

static void mode_dropdown_cb(lv_event_t* e) {
    lv_obj_t* dd = (lv_obj_t*)lv_event_get_target(e);
    uint16_t sel = lv_dropdown_get_selected(dd);
    bool demo_mode = app_prefs_is_demo_mode();
    
    if (demo_mode) {
        // Demo mode - just update local state
        state.demo_mode = sel;
        const char* mode_names[] = {"Cooling", "Floor Heat", "Fan Heat", "Hot Water", "Auto"};
        ESP_LOGI(TAG, "[DEMO] Mode: %s", mode_names[sel]);
        return;
    }
    
    arctic::HeatPumpState hp = arctic::getState();
    if (!hp.connected) {
        // Disconnected - show error popup
        show_write_error_popup("Cannot change mode: Heat pump not connected");
        return;
    }
    
    arctic::WorkingMode mode;
    switch (sel) {
        case 0: mode = arctic::WorkingMode::COOLING; break;
        case 1: mode = arctic::WorkingMode::FLOOR_HEATING; break;
        case 2: mode = arctic::WorkingMode::FAN_COIL_HEATING; break;
        case 3: mode = arctic::WorkingMode::HOT_WATER; break;
        case 4: mode = arctic::WorkingMode::AUTO; break;
        default: return;
    }
    arctic::setWorkingMode(mode);
}

// Get the active mode for setpoint adjustment
static int get_active_mode() {
    bool demo_mode = app_prefs_is_demo_mode();
    
    if (!demo_mode) {
        arctic::HeatPumpState hp = arctic::getState();
        if (hp.connected) {
            switch (hp.working_mode) {
                case arctic::WorkingMode::COOLING: return 0;
                case arctic::WorkingMode::FLOOR_HEATING:
                case arctic::WorkingMode::FAN_COIL_HEATING: return 1;
                case arctic::WorkingMode::HOT_WATER: return 3;
                case arctic::WorkingMode::AUTO: return 1;  // Default to heating in auto
                default: return 1;
            }
        }
        return 1;  // Default when disconnected
    }
    return state.demo_mode;
}

static void setpoint_minus_cb(lv_event_t* e) {
    (void)e;
    bool demo_mode = app_prefs_is_demo_mode();
    int mode = get_active_mode();
    
    if (demo_mode) {
        // Demo mode
        if (mode == 0) { if (state.demo_cooling > 5) state.demo_cooling--; }
        else if (mode == 3) { if (state.demo_hotwater > 30) state.demo_hotwater--; }
        else { if (state.demo_heating > 20) state.demo_heating--; }
        return;
    }
    
    arctic::HeatPumpState hp = arctic::getState();
    if (!hp.connected) {
        show_write_error_popup("Cannot adjust setpoint: Heat pump not connected");
        return;
    }
    
    // Real mode
    if (mode == 0) {
        int16_t val = hp.cooling_setpoint - 1;
        if (val >= 5) arctic::setCoolingSetpoint(val);
    } else if (mode == 3) {
        int16_t val = hp.hot_water_setpoint - 1;
        if (val >= 30) arctic::setHotWaterSetpoint(val);
    } else {
        int16_t val = hp.heating_setpoint - 1;
        if (val >= 20) arctic::setHeatingSetpoint(val);
    }
}

static void setpoint_plus_cb(lv_event_t* e) {
    (void)e;
    bool demo_mode = app_prefs_is_demo_mode();
    int mode = get_active_mode();
    
    if (demo_mode) {
        // Demo mode
        if (mode == 0) { if (state.demo_cooling < 30) state.demo_cooling++; }
        else if (mode == 3) { if (state.demo_hotwater < 60) state.demo_hotwater++; }
        else { if (state.demo_heating < 60) state.demo_heating++; }
        return;
    }
    
    arctic::HeatPumpState hp = arctic::getState();
    if (!hp.connected) {
        show_write_error_popup("Cannot adjust setpoint: Heat pump not connected");
        return;
    }
    
    // Real mode
    if (mode == 0) {
        int16_t val = hp.cooling_setpoint + 1;
        if (val <= 30) arctic::setCoolingSetpoint(val);
    } else if (mode == 3) {
        int16_t val = hp.hot_water_setpoint + 1;
        if (val <= 60) arctic::setHotWaterSetpoint(val);
    } else {
        int16_t val = hp.heating_setpoint + 1;
        if (val <= 60) arctic::setHeatingSetpoint(val);
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
    
    ESP_LOGI(TAG, "Creating heat pump status display");
    
    // Main container - fills from y_offset to near bottom of screen
    // Screen is 1280px tall, leave 40px for footer
    state.container = lv_obj_create(parent);
    lv_obj_set_size(state.container, 700, 1280 - y_offset - 40);
    lv_obj_align(state.container, LV_ALIGN_TOP_MID, 0, y_offset);
    lv_obj_set_style_bg_opa(state.container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.container, 0, LV_PART_MAIN);
    lv_obj_clear_flag(state.container, LV_OBJ_FLAG_SCROLLABLE);
    
    // =========================================================================
    // TOP: Main Status Card
    // =========================================================================
    state.status_card = lv_obj_create(state.container);
    lv_obj_set_size(state.status_card, LV_PCT(100), 200);
    lv_obj_align(state.status_card, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(state.status_card, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_border_color(state.status_card, COLOR_CARD_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.status_card, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(state.status_card, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.status_card, 20, LV_PART_MAIN);
    lv_obj_clear_flag(state.status_card, LV_OBJ_FLAG_SCROLLABLE);
    
    // Connection status indicator (top left corner)
    state.connection_indicator = lv_obj_create(state.status_card);
    lv_obj_set_size(state.connection_indicator, 12, 12);
    lv_obj_set_style_radius(state.connection_indicator, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(state.connection_indicator, COLOR_INACTIVE, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.connection_indicator, 0, LV_PART_MAIN);
    lv_obj_align(state.connection_indicator, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_clear_flag(state.connection_indicator, LV_OBJ_FLAG_SCROLLABLE);
    
    // Mode label (large, centered at top)
    state.mode_label = lv_label_create(state.status_card);
    lv_label_set_text(state.mode_label, "DISCONNECTED");
    lv_obj_set_style_text_font(state.mode_label, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.mode_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(state.mode_label, LV_ALIGN_TOP_MID, 0, 5);
    
    // Tank temperature (large, center)
    state.tank_temp_label = lv_label_create(state.status_card);
    lv_label_set_text(state.tank_temp_label, "-- °");
    lv_obj_set_style_text_font(state.tank_temp_label, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.tank_temp_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(state.tank_temp_label, LV_ALIGN_CENTER, 0, 0);
    
    // "Tank Temperature" label
    lv_obj_t* tank_desc = lv_label_create(state.status_card);
    lv_label_set_text(tank_desc, "Tank Temperature");
    lv_obj_set_style_text_font(tank_desc, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(tank_desc, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(tank_desc, LV_ALIGN_CENTER, 0, 40);
    
    // Setpoint (bottom right)
    state.setpoint_label = lv_label_create(state.status_card);
    lv_label_set_text(state.setpoint_label, "Set: -- °");
    lv_obj_set_style_text_font(state.setpoint_label, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.setpoint_label, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(state.setpoint_label, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    
    // Power meter (bottom left) - shows AC voltage × current = watts
    state.power_meter_label = lv_label_create(state.status_card);
    lv_label_set_text(state.power_meter_label, "-- W");
    lv_obj_set_style_text_font(state.power_meter_label, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.power_meter_label, COLOR_WARNING, LV_PART_MAIN);
    lv_obj_align(state.power_meter_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    
    // =========================================================================
    // MIDDLE: Component Status Row
    // =========================================================================
    lv_obj_t* status_row = lv_obj_create(state.container);
    lv_obj_set_size(status_row, LV_PCT(100), 90);
    lv_obj_align(status_row, LV_ALIGN_TOP_MID, 0, 215);
    lv_obj_set_style_bg_color(status_row, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_border_color(status_row, COLOR_CARD_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(status_row, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(status_row, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(status_row, 15, LV_PART_MAIN);
    lv_obj_clear_flag(status_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    // Compressor indicator
    create_status_indicator(status_row, "Compressor", &state.compressor_indicator, &state.compressor_label);
    
    // Fan indicator
    create_status_indicator(status_row, "Fan", &state.fan_indicator, &state.fan_label);
    
    // Water pump indicator
    create_status_indicator(status_row, "Pump", &state.pump_indicator, &state.pump_label);
    
    // Backup heater indicator
    create_status_indicator(status_row, "Aux Heat", &state.heater_indicator, &state.heater_label);
    
    // =========================================================================
    // ERROR CARD: Prominent error/status display
    // =========================================================================
    state.error_card = lv_obj_create(state.container);
    lv_obj_set_size(state.error_card, LV_PCT(100), 60);
    lv_obj_align(state.error_card, LV_ALIGN_TOP_MID, 0, 320);
    lv_obj_set_style_bg_color(state.error_card, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_border_color(state.error_card, COLOR_WARNING, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.error_card, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(state.error_card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.error_card, 10, LV_PART_MAIN);
    lv_obj_clear_flag(state.error_card, LV_OBJ_FLAG_SCROLLABLE);
    
    state.error_label = lv_label_create(state.error_card);
    lv_label_set_text(state.error_label, "");
    lv_obj_set_style_text_font(state.error_label, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.error_label, COLOR_WARNING, LV_PART_MAIN);
    lv_obj_set_width(state.error_label, LV_PCT(100));
    lv_obj_set_style_text_align(state.error_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(state.error_label);
    
    // Hide error card initially
    lv_obj_add_flag(state.error_card, LV_OBJ_FLAG_HIDDEN);
    
    // =========================================================================
    // CONTROLS CARD: Power, Mode, Single Setpoint
    // =========================================================================
    lv_obj_t* controls_card = lv_obj_create(state.container);
    lv_obj_set_size(controls_card, LV_PCT(100), 330);
    lv_obj_align(controls_card, LV_ALIGN_TOP_MID, 0, 395);
    lv_obj_set_style_bg_color(controls_card, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_border_color(controls_card, COLOR_CARD_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(controls_card, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(controls_card, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(controls_card, 20, LV_PART_MAIN);
    lv_obj_clear_flag(controls_card, LV_OBJ_FLAG_SCROLLABLE);
    
    // --- Large Power Button (full width, prominent) ---
    state.power_btn = lv_btn_create(controls_card);
    lv_obj_set_size(state.power_btn, LV_PCT(100), 80);
    lv_obj_align(state.power_btn, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(state.power_btn, COLOR_ERROR, LV_PART_MAIN);  // Red when off
    lv_obj_set_style_bg_color(state.power_btn, lv_color_hex(0x8b0000), LV_STATE_PRESSED);
    lv_obj_set_style_radius(state.power_btn, 12, LV_PART_MAIN);
    lv_obj_add_event_cb(state.power_btn, power_btn_cb, LV_EVENT_CLICKED, nullptr);
    
    state.power_btn_label = lv_label_create(state.power_btn);
    lv_label_set_text(state.power_btn_label, LV_SYMBOL_POWER " POWER OFF");
    lv_obj_set_style_text_font(state.power_btn_label, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.power_btn_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_center(state.power_btn_label);
    
    // --- Mode Row ---
    lv_obj_t* mode_row = lv_obj_create(controls_card);
    lv_obj_set_size(mode_row, LV_PCT(100), 60);
    lv_obj_align(mode_row, LV_ALIGN_TOP_MID, 0, 95);
    lv_obj_set_style_bg_opa(mode_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(mode_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(mode_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(mode_row, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t* mode_lbl = lv_label_create(mode_row);
    lv_label_set_text(mode_lbl, "Mode");
    lv_obj_set_style_text_font(mode_lbl, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(mode_lbl, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(mode_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    
    state.mode_dropdown = lv_dropdown_create(mode_row);
    lv_dropdown_set_options(state.mode_dropdown, 
        "Cooling\n"
        "Floor Heating\n"
        "Fan Coil Heating\n"
        "Hot Water\n"
        "Auto");
    lv_obj_set_size(state.mode_dropdown, 280, 50);
    lv_obj_align(state.mode_dropdown, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(state.mode_dropdown, lv_color_hex(0x1a2a4e), LV_PART_MAIN);
    lv_obj_set_style_text_color(state.mode_dropdown, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(state.mode_dropdown, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_border_color(state.mode_dropdown, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.mode_dropdown, 1, LV_PART_MAIN);
    lv_obj_add_event_cb(state.mode_dropdown, mode_dropdown_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    
    // --- Active Setpoint Row (single row that changes label based on mode) ---
    lv_obj_t* setpoint_row = lv_obj_create(controls_card);
    lv_obj_set_size(setpoint_row, LV_PCT(100), 80);
    lv_obj_align(setpoint_row, LV_ALIGN_TOP_MID, 0, 170);
    lv_obj_set_style_bg_opa(setpoint_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(setpoint_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(setpoint_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(setpoint_row, LV_OBJ_FLAG_SCROLLABLE);
    
    // Setpoint name label (Cooling/Heating/Hot Water)
    state.active_setpoint_name = lv_label_create(setpoint_row);
    lv_label_set_text(state.active_setpoint_name, "Setpoint");
    lv_obj_set_style_text_font(state.active_setpoint_name, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.active_setpoint_name, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(state.active_setpoint_name, LV_ALIGN_LEFT_MID, 0, 0);
    
    // Minus button (larger)
    state.active_setpoint_minus = lv_btn_create(setpoint_row);
    lv_obj_set_size(state.active_setpoint_minus, 70, 60);
    lv_obj_align(state.active_setpoint_minus, LV_ALIGN_RIGHT_MID, -200, 0);
    lv_obj_set_style_bg_color(state.active_setpoint_minus, lv_color_hex(0x1a2a4e), LV_PART_MAIN);
    lv_obj_set_style_bg_color(state.active_setpoint_minus, lv_color_hex(0x2a3a5e), LV_STATE_PRESSED);
    lv_obj_set_style_radius(state.active_setpoint_minus, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(state.active_setpoint_minus, setpoint_minus_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* minus_lbl = lv_label_create(state.active_setpoint_minus);
    lv_label_set_text(minus_lbl, "-");
    lv_obj_set_style_text_font(minus_lbl, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_center(minus_lbl);
    
    // Value label (larger)
    state.active_setpoint_label = lv_label_create(setpoint_row);
    lv_label_set_text(state.active_setpoint_label, "-- °");
    lv_obj_set_style_text_font(state.active_setpoint_label, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.active_setpoint_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(state.active_setpoint_label, LV_ALIGN_RIGHT_MID, -90, 0);
    
    // Plus button (larger)
    state.active_setpoint_plus = lv_btn_create(setpoint_row);
    lv_obj_set_size(state.active_setpoint_plus, 70, 60);
    lv_obj_align(state.active_setpoint_plus, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(state.active_setpoint_plus, lv_color_hex(0x1a2a4e), LV_PART_MAIN);
    lv_obj_set_style_bg_color(state.active_setpoint_plus, lv_color_hex(0x2a3a5e), LV_STATE_PRESSED);
    lv_obj_set_style_radius(state.active_setpoint_plus, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(state.active_setpoint_plus, setpoint_plus_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* plus_lbl = lv_label_create(state.active_setpoint_plus);
    lv_label_set_text(plus_lbl, "+");
    lv_obj_set_style_text_font(plus_lbl, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_center(plus_lbl);
    
    // =========================================================================
    // BOTTOM: Three button bar - Temps | System | Advanced
    // =========================================================================
    
    // Temperatures button (left)
    state.temps_btn = lv_btn_create(state.container);
    lv_obj_set_size(state.temps_btn, 200, 60);
    lv_obj_align(state.temps_btn, LV_ALIGN_BOTTOM_LEFT, 10, 0);
    lv_obj_set_style_bg_color(state.temps_btn, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(state.temps_btn, lv_color_hex(0x2a3a5e), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(state.temps_btn, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.temps_btn, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(state.temps_btn, 12, LV_PART_MAIN);
    lv_obj_add_event_cb(state.temps_btn, temps_btn_cb, LV_EVENT_CLICKED, nullptr);
    
    lv_obj_t* temps_label = lv_label_create(state.temps_btn);
    lv_label_set_text(temps_label, LV_SYMBOL_MINUS " Temps");
    lv_obj_set_style_text_font(temps_label, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(temps_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_center(temps_label);
    
    // System readings button (center)
    state.system_btn = lv_btn_create(state.container);
    lv_obj_set_size(state.system_btn, 200, 60);
    lv_obj_align(state.system_btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(state.system_btn, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(state.system_btn, lv_color_hex(0x2a3a5e), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(state.system_btn, COLOR_WARNING, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.system_btn, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(state.system_btn, 12, LV_PART_MAIN);
    lv_obj_add_event_cb(state.system_btn, system_btn_cb, LV_EVENT_CLICKED, nullptr);
    
    lv_obj_t* system_label = lv_label_create(state.system_btn);
    lv_label_set_text(system_label, LV_SYMBOL_LIST " System");
    lv_obj_set_style_text_font(system_label, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(system_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_center(system_label);
    
    // Advanced button (right) - heat pump P-parameters
    state.controls_btn = lv_btn_create(state.container);
    lv_obj_set_size(state.controls_btn, 200, 60);
    lv_obj_align(state.controls_btn, LV_ALIGN_BOTTOM_RIGHT, -10, 0);
    lv_obj_set_style_bg_color(state.controls_btn, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(state.controls_btn, lv_color_hex(0x2a3a5e), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(state.controls_btn, COLOR_SUCCESS, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.controls_btn, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(state.controls_btn, 12, LV_PART_MAIN);
    lv_obj_add_event_cb(state.controls_btn, controls_btn_cb, LV_EVENT_CLICKED, nullptr);
    
    lv_obj_t* ctrl_label = lv_label_create(state.controls_btn);
    lv_label_set_text(ctrl_label, LV_SYMBOL_SETTINGS " Advanced");
    lv_obj_set_style_text_font(ctrl_label, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(ctrl_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_center(ctrl_label);

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
    
    // Connection indicator
    set_indicator_active(state.connection_indicator, hp.connected, COLOR_SUCCESS);
    
    // Helper: Get setpoint name, value, and color for active mode
    auto get_active_setpoint_info = [&](bool connected, int mode, int16_t* value, const char** name, lv_color_t* color) {
        if (!connected) {
            // Demo mode
            if (mode == 0) {
                *value = state.demo_cooling;
                *name = "Cooling";
                *color = COLOR_COOLING;
            } else if (mode == 3) {
                *value = state.demo_hotwater;
                *name = "Hot Water";
                *color = COLOR_ERROR;
            } else {
                *value = state.demo_heating;
                *name = "Heating";
                *color = COLOR_HEATING;
            }
        } else {
            // Real mode
            switch (hp.working_mode) {
                case arctic::WorkingMode::COOLING:
                    *value = hp.cooling_setpoint;
                    *name = "Cooling";
                    *color = COLOR_COOLING;
                    break;
                case arctic::WorkingMode::FLOOR_HEATING:
                case arctic::WorkingMode::FAN_COIL_HEATING:
                    *value = hp.heating_setpoint;
                    *name = "Heating";
                    *color = COLOR_HEATING;
                    break;
                case arctic::WorkingMode::HOT_WATER:
                    *value = hp.hot_water_setpoint;
                    *name = "Hot Water";
                    *color = COLOR_ERROR;
                    break;
                case arctic::WorkingMode::AUTO:
                default:
                    *value = hp.heating_setpoint;
                    *name = "Heating";
                    *color = COLOR_HEATING;
                    break;
            }
        }
    };
    
    // Helper: Update power button appearance
    auto update_power_btn = [](bool power_on) {
        if (!state.power_btn || !state.power_btn_label) return;
        
        if (power_on) {
            lv_obj_set_style_bg_color(state.power_btn, COLOR_SUCCESS, LV_PART_MAIN);
            lv_obj_set_style_bg_color(state.power_btn, lv_color_hex(0x2d8b3d), LV_STATE_PRESSED);
            lv_label_set_text(state.power_btn_label, LV_SYMBOL_POWER " POWER ON");
            // Dark text on green for better contrast
            lv_obj_set_style_text_color(state.power_btn_label, lv_color_hex(0x0a2010), LV_PART_MAIN);
        } else {
            lv_obj_set_style_bg_color(state.power_btn, COLOR_ERROR, LV_PART_MAIN);
            lv_obj_set_style_bg_color(state.power_btn, lv_color_hex(0x8b0000), LV_STATE_PRESSED);
            lv_label_set_text(state.power_btn_label, LV_SYMBOL_POWER " POWER OFF");
            // White text on red
            lv_obj_set_style_text_color(state.power_btn_label, COLOR_TEXT, LV_PART_MAIN);
        }
    };
    
    // Get demo mode and connection state
    bool demo_mode_enabled = app_prefs_is_demo_mode();
    bool connected = hp.connected;
    
    if (demo_mode_enabled) {
        // =====================================================================
        // DEMO MODE - show simulated values and allow interaction
        // =====================================================================
        const char* demo_mode_names[] = {"COOLING", "FLOOR HEAT", "FAN HEAT", "HOT WATER", "AUTO"};
        lv_color_t demo_mode_colors[] = {COLOR_COOLING, COLOR_HEATING, COLOR_HEATING, COLOR_ERROR, COLOR_SUCCESS};
        
        if (!state.demo_power) {
            lv_label_set_text(state.mode_label, "DEMO - STANDBY");
            lv_obj_set_style_text_color(state.mode_label, COLOR_TEXT_DIM, LV_PART_MAIN);
        } else {
            char mode_buf[32];
            snprintf(mode_buf, sizeof(mode_buf), "DEMO - %s", demo_mode_names[state.demo_mode]);
            lv_label_set_text(state.mode_label, mode_buf);
            lv_obj_set_style_text_color(state.mode_label, demo_mode_colors[state.demo_mode], LV_PART_MAIN);
        }
        
        // Demo tank temperature
        char demo_tank_buf[16];
        snprintf(demo_tank_buf, sizeof(demo_tank_buf), "%d %s", 
                 app_prefs_convert_temp(42), app_prefs_temp_unit_str());
        lv_label_set_text(state.tank_temp_label, demo_tank_buf);
        
        // Demo setpoint in status card
        int16_t sp_val;
        const char* sp_name;
        lv_color_t sp_color;
        get_active_setpoint_info(false, state.demo_mode, &sp_val, &sp_name, &sp_color);
        
        char sp_buf[24];
        snprintf(sp_buf, sizeof(sp_buf), "Set: %d %s", 
                 app_prefs_convert_temp(sp_val), app_prefs_temp_unit_str());
        lv_label_set_text(state.setpoint_label, sp_buf);
        
        // Demo component indicators (simulate some activity)
        set_indicator_active(state.compressor_indicator, state.demo_power, COLOR_SUCCESS);
        set_indicator_active(state.fan_indicator, state.demo_power, COLOR_SUCCESS);
        set_indicator_active(state.pump_indicator, state.demo_power, COLOR_ACCENT);
        set_indicator_active(state.heater_indicator, false, COLOR_WARNING);
        
        lv_label_set_text(state.fan_label, state.demo_power ? "Fan Med" : "Fan");
        
        // Demo power meter (simulated 1.2 kW when running)
        if (state.power_meter_label) {
            lv_label_set_text(state.power_meter_label, state.demo_power ? "1200 W" : "0 W");
        }
        
        // Show demo mode message in info card
        lv_label_set_text(state.error_label, "Demo Mode Enabled");
        lv_obj_set_style_text_color(state.error_label, COLOR_WARNING, LV_PART_MAIN);
        lv_obj_set_style_border_color(state.error_card, COLOR_WARNING, LV_PART_MAIN);
        lv_obj_remove_flag(state.error_card, LV_OBJ_FLAG_HIDDEN);
        
        // Update power button
        update_power_btn(state.demo_power);
        
        // Update mode dropdown
        if (state.mode_dropdown) {
            lv_obj_remove_event_cb(state.mode_dropdown, mode_dropdown_cb);
            lv_dropdown_set_selected(state.mode_dropdown, state.demo_mode);
            lv_obj_add_event_cb(state.mode_dropdown, mode_dropdown_cb, LV_EVENT_VALUE_CHANGED, nullptr);
        }
        
        // Update active setpoint row
        if (state.active_setpoint_name) {
            lv_label_set_text(state.active_setpoint_name, sp_name);
            lv_obj_set_style_text_color(state.active_setpoint_name, sp_color, LV_PART_MAIN);
        }
        if (state.active_setpoint_label) {
            snprintf(sp_buf, sizeof(sp_buf), "%d %s", 
                     app_prefs_convert_temp(sp_val), app_prefs_temp_unit_str());
            lv_label_set_text(state.active_setpoint_label, sp_buf);
        }
        
        return;
    }
    
    if (!connected) {
        // =====================================================================
        // DISCONNECTED MODE - show placeholders, controls will show error popups
        // =====================================================================
        lv_label_set_text(state.mode_label, "DISCONNECTED");
        lv_obj_set_style_text_color(state.mode_label, COLOR_ERROR, LV_PART_MAIN);
        
        // Placeholder values
        lv_label_set_text(state.tank_temp_label, "--");
        lv_label_set_text(state.setpoint_label, "Set: --");
        
        // All indicators off
        set_indicator_active(state.compressor_indicator, false, COLOR_SUCCESS);
        set_indicator_active(state.fan_indicator, false, COLOR_SUCCESS);
        set_indicator_active(state.pump_indicator, false, COLOR_ACCENT);
        set_indicator_active(state.heater_indicator, false, COLOR_WARNING);
        lv_label_set_text(state.fan_label, "Fan");
        
        // Power meter placeholder
        if (state.power_meter_label) {
            lv_label_set_text(state.power_meter_label, "-- W");
        }
        
        // Show disconnected warning in error card
        lv_label_set_text(state.error_label, "Heat pump not connected");
        lv_obj_set_style_text_color(state.error_label, COLOR_ERROR, LV_PART_MAIN);
        lv_obj_set_style_border_color(state.error_card, COLOR_ERROR, LV_PART_MAIN);
        lv_obj_remove_flag(state.error_card, LV_OBJ_FLAG_HIDDEN);
        
        // Power button shows off state
        update_power_btn(false);
        
        // Mode dropdown - reset to first option
        if (state.mode_dropdown) {
            lv_obj_remove_event_cb(state.mode_dropdown, mode_dropdown_cb);
            lv_dropdown_set_selected(state.mode_dropdown, 0);
            lv_obj_add_event_cb(state.mode_dropdown, mode_dropdown_cb, LV_EVENT_VALUE_CHANGED, nullptr);
        }
        
        // Active setpoint row - placeholders
        if (state.active_setpoint_name) {
            lv_label_set_text(state.active_setpoint_name, "Setpoint");
            lv_obj_set_style_text_color(state.active_setpoint_name, COLOR_TEXT_DIM, LV_PART_MAIN);
        }
        if (state.active_setpoint_label) {
            lv_label_set_text(state.active_setpoint_label, "--");
        }
        
        return;
    }
    
    // =========================================================================
    // REAL MODE - connected to heat pump
    // =========================================================================
    
    // Mode
    bool defrosting = hp.isDefrosting();
    const char* mode_text = getModeText(hp.working_mode, defrosting);
    lv_color_t mode_color = getModeColor(hp.working_mode, defrosting);
    
    if (!hp.unit_on) {
        lv_label_set_text(state.mode_label, "STANDBY");
        lv_obj_set_style_text_color(state.mode_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    } else {
        lv_label_set_text(state.mode_label, mode_text);
        lv_obj_set_style_text_color(state.mode_label, mode_color, LV_PART_MAIN);
    }
    
    // Tank temperature
    char temp_buf[24];
    snprintf(temp_buf, sizeof(temp_buf), "%d %s", 
             app_prefs_convert_temp(hp.water_tank_temp), app_prefs_temp_unit_str());
    lv_label_set_text(state.tank_temp_label, temp_buf);
    
    // Setpoint in status card (based on mode)
    int16_t sp_val;
    const char* sp_name;
    lv_color_t sp_color;
    get_active_setpoint_info(true, 0, &sp_val, &sp_name, &sp_color);
    
    snprintf(temp_buf, sizeof(temp_buf), "Set: %d %s", 
             app_prefs_convert_temp(sp_val), app_prefs_temp_unit_str());
    lv_label_set_text(state.setpoint_label, temp_buf);
    
    // Component indicators
    set_indicator_active(state.compressor_indicator, hp.isCompressorRunning(), COLOR_SUCCESS);
    set_indicator_active(state.fan_indicator, hp.isFanRunning(), COLOR_SUCCESS);
    set_indicator_active(state.pump_indicator, hp.isWaterPumpRunning(), COLOR_ACCENT);
    set_indicator_active(state.heater_indicator, hp.isBackupHeaterOn(), COLOR_WARNING);
    
    // Update fan label with speed
    int fan_speed = hp.getFanSpeedLevel();
    if (fan_speed > 0) {
        const char* speed_names[] = {"", "Low", "Med", "High"};
        char fan_buf[16];
        snprintf(fan_buf, sizeof(fan_buf), "Fan %s", speed_names[fan_speed]);
        lv_label_set_text(state.fan_label, fan_buf);
    } else {
        lv_label_set_text(state.fan_label, "Fan");
    }
    
    // Power meter: AC voltage × AC current
    if (state.power_meter_label) {
        // ac_current is in tenths of amps, ac_voltage is in volts
        uint32_t power_watts = (hp.ac_voltage * hp.ac_current) / 10;
        char power_buf[32];
        if (power_watts >= 1000) {
            snprintf(power_buf, sizeof(power_buf), "%.1f kW", power_watts / 1000.0f);
        } else {
            snprintf(power_buf, sizeof(power_buf), "%lu W", (unsigned long)power_watts);
        }
        lv_label_set_text(state.power_meter_label, power_buf);
    }
    
    // Error display
    if (hp.hasAnyError()) {
        char error_buf[256];
        arctic::getErrorDescriptions(error_buf, sizeof(error_buf));
        lv_label_set_text(state.error_label, error_buf);
        lv_obj_set_style_text_color(state.error_label, COLOR_ERROR, LV_PART_MAIN);
        lv_obj_set_style_border_color(state.error_card, COLOR_ERROR, LV_PART_MAIN);
        lv_obj_remove_flag(state.error_card, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(state.error_card, LV_OBJ_FLAG_HIDDEN);
    }
    
    // Update power button
    update_power_btn(hp.unit_on);
    
    // Mode dropdown - update without triggering callback
    if (state.mode_dropdown) {
        lv_obj_remove_event_cb(state.mode_dropdown, mode_dropdown_cb);
        uint16_t mode_idx = 0;
        switch (hp.working_mode) {
            case arctic::WorkingMode::COOLING: mode_idx = 0; break;
            case arctic::WorkingMode::FLOOR_HEATING: mode_idx = 1; break;
            case arctic::WorkingMode::FAN_COIL_HEATING: mode_idx = 2; break;
            case arctic::WorkingMode::HOT_WATER: mode_idx = 3; break;
            case arctic::WorkingMode::AUTO: mode_idx = 4; break;
        }
        lv_dropdown_set_selected(state.mode_dropdown, mode_idx);
        lv_obj_add_event_cb(state.mode_dropdown, mode_dropdown_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    }
    
    // Update active setpoint row
    if (state.active_setpoint_name) {
        lv_label_set_text(state.active_setpoint_name, sp_name);
        lv_obj_set_style_text_color(state.active_setpoint_name, sp_color, LV_PART_MAIN);
    }
    if (state.active_setpoint_label) {
        char sp_buf[24];
        snprintf(sp_buf, sizeof(sp_buf), "%d %s", 
                 app_prefs_convert_temp(sp_val), app_prefs_temp_unit_str());
        lv_label_set_text(state.active_setpoint_label, sp_buf);
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
