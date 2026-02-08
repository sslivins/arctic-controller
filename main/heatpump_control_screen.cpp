/*
 * Arctic Heat Pump Controller
 * Heat Pump Control Screen - Modify settings
 */

#include "heatpump_control_screen.h"
#include "modbus/arctic_heatpump.h"
#include "modbus/arctic_registers.h"
#include "ui_common.h"
#include <esp_log.h>
#include <stdio.h>

static const char* TAG = "hp_control";

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
#define COLOR_HEATING       lv_color_hex(0xf97316)
#define COLOR_COOLING       lv_color_hex(0x3b82f6)

// ============================================================================
// State
// ============================================================================
static struct {
    bool shown = false;
    lv_obj_t* screen = nullptr;
    lv_obj_t* scroll_container = nullptr;
    heatpump_control_close_cb_t on_close = nullptr;
    lv_timer_t* update_timer = nullptr;
    
    // Basic controls
    lv_obj_t* power_switch = nullptr;
    lv_obj_t* mode_dropdown = nullptr;
    lv_obj_t* cooling_spinbox = nullptr;
    lv_obj_t* heating_spinbox = nullptr;
    lv_obj_t* hotwater_spinbox = nullptr;
    
    // Technician section - P-Parameters
    lv_obj_t* tech_section = nullptr;
    lv_obj_t* p1_eev_initial_spinbox = nullptr;
    lv_obj_t* p5_sterilize_time_spinbox = nullptr;
    lv_obj_t* p13_max_temp_spinbox = nullptr;
    lv_obj_t* p23_cooling_auto_spinbox = nullptr;
    lv_obj_t* p24_heating_auto_spinbox = nullptr;
    lv_obj_t* p28_mode_switch_spinbox = nullptr;
    lv_obj_t* p29_defrost_cycle_spinbox = nullptr;
    lv_obj_t* p30_defrost_enter_spinbox = nullptr;
    lv_obj_t* p31_defrost_extend_spinbox = nullptr;
    lv_obj_t* p32_defrost_diff_spinbox = nullptr;
    lv_obj_t* p33_defrost_extend_time_spinbox = nullptr;
    lv_obj_t* p34_max_defrost_spinbox = nullptr;
    lv_obj_t* p35_defrost_exit_spinbox = nullptr;
    lv_obj_t* p36_water_return_temp_spinbox = nullptr;
    lv_obj_t* p37_water_return_time_spinbox = nullptr;
    lv_obj_t* p38_low_ambient_spinbox = nullptr;
    lv_obj_t* p39_freq_reduction_spinbox = nullptr;
    lv_obj_t* p40_cooling_low_ambient_spinbox = nullptr;
    lv_obj_t* p41_eev_mode_spinbox = nullptr;
    lv_obj_t* p42_eev_superheat_spinbox = nullptr;
    lv_obj_t* p43_3way_valve_spinbox = nullptr;
    lv_obj_t* p44_pump_mode_spinbox = nullptr;
    lv_obj_t* p45_pump_interval_spinbox = nullptr;
    lv_obj_t* p46_pump_low_ambient_spinbox = nullptr;
    lv_obj_t* p47_waterway_cleaning_spinbox = nullptr;
    
    // Status
    lv_obj_t* status_label = nullptr;
} state;

// ============================================================================
// Helper Functions
// ============================================================================

static lv_obj_t* create_section_header(lv_obj_t* parent, const char* title) {
    lv_obj_t* header = lv_obj_create(parent);
    lv_obj_set_size(header, LV_PCT(100), 40);
    lv_obj_set_style_bg_color(header, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(header, 10, LV_PART_MAIN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t* lbl = lv_label_create(header);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    
    return header;
}

static lv_obj_t* create_control_row(lv_obj_t* parent, const char* label) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 50);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(row, 5, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t* name_lbl = lv_label_create(row);
    lv_label_set_text(name_lbl, label);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(name_lbl, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    
    return row;
}

static void show_status(const char* msg, bool is_error = false) {
    if (state.status_label) {
        lv_label_set_text(state.status_label, msg);
        lv_obj_set_style_text_color(state.status_label, 
            is_error ? COLOR_ERROR : COLOR_SUCCESS, LV_PART_MAIN);
    }
}

// ============================================================================
// Event Handlers
// ============================================================================

static void power_switch_cb(lv_event_t* e) {
    lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    
    if (arctic::setUnitPower(on)) {
        show_status(on ? "Unit powered ON" : "Unit powered OFF");
    } else {
        show_status("Failed to set power", true);
        // Revert switch state
        if (on) {
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
    }
}

static void mode_dropdown_cb(lv_event_t* e) {
    lv_obj_t* dd = (lv_obj_t*)lv_event_get_target(e);
    uint16_t sel = lv_dropdown_get_selected(dd);
    
    arctic::WorkingMode mode;
    switch (sel) {
        case 0: mode = arctic::WorkingMode::COOLING; break;
        case 1: mode = arctic::WorkingMode::FLOOR_HEATING; break;
        case 2: mode = arctic::WorkingMode::FAN_COIL_HEATING; break;
        case 3: mode = arctic::WorkingMode::HOT_WATER; break;
        case 4: mode = arctic::WorkingMode::AUTO; break;
        default: return;
    }
    
    if (arctic::setWorkingMode(mode)) {
        show_status("Mode changed");
    } else {
        show_status("Failed to change mode", true);
    }
}

static void cooling_spinbox_cb(lv_event_t* e) {
    lv_obj_t* spinbox = (lv_obj_t*)lv_event_get_target(e);
    int32_t val = lv_spinbox_get_value(spinbox);
    
    if (arctic::setCoolingSetpoint((int16_t)val)) {
        show_status("Cooling setpoint updated");
    } else {
        show_status("Failed to update", true);
    }
}

static void heating_spinbox_cb(lv_event_t* e) {
    lv_obj_t* spinbox = (lv_obj_t*)lv_event_get_target(e);
    int32_t val = lv_spinbox_get_value(spinbox);
    
    if (arctic::setHeatingSetpoint((int16_t)val)) {
        show_status("Heating setpoint updated");
    } else {
        show_status("Failed to update", true);
    }
}

static void hotwater_spinbox_cb(lv_event_t* e) {
    lv_obj_t* spinbox = (lv_obj_t*)lv_event_get_target(e);
    int32_t val = lv_spinbox_get_value(spinbox);
    
    if (arctic::setHotWaterSetpoint((int16_t)val)) {
        show_status("Hot water setpoint updated");
    } else {
        show_status("Failed to update", true);
    }
}

// P-parameter spinbox callbacks - using a macro to reduce repetition
#define DEFINE_P_SPINBOX_CB(name, reg_addr, msg) \
static void name##_spinbox_cb(lv_event_t* e) { \
    lv_obj_t* spinbox = (lv_obj_t*)lv_event_get_target(e); \
    int32_t val = lv_spinbox_get_value(spinbox); \
    if (arctic::writeRegister(reg_addr, (uint16_t)val)) { \
        show_status(msg " updated"); \
    } else { \
        show_status("Failed to update", true); \
    } \
}

DEFINE_P_SPINBOX_CB(p1_eev_initial, arctic::reg::P1_EEV_INITIAL_OPENING, "P1 EEV Opening")
DEFINE_P_SPINBOX_CB(p5_sterilize, arctic::reg::P5_STERILIZING_TIME, "P5 Sterilize Time")
DEFINE_P_SPINBOX_CB(p13_max_temp, arctic::reg::P13_MAX_TEMP_SETTING, "P13 Max Temp")
DEFINE_P_SPINBOX_CB(p23_cooling_auto, arctic::reg::P23_COOLING_AUTO_TEMP, "P23 Cooling Auto")
DEFINE_P_SPINBOX_CB(p24_heating_auto, arctic::reg::P24_HEATING_AUTO_TEMP, "P24 Heating Auto")
DEFINE_P_SPINBOX_CB(p28_mode_switch, arctic::reg::P28_MODE_SWITCH_DELAY, "P28 Mode Delay")
DEFINE_P_SPINBOX_CB(p29_defrost_cycle, arctic::reg::P29_DEFROST_CYCLE, "P29 Defrost Cycle")
DEFINE_P_SPINBOX_CB(p30_defrost_enter, arctic::reg::P30_DEFROST_ENTER_TEMP, "P30 Defrost Enter")
DEFINE_P_SPINBOX_CB(p31_defrost_extend, arctic::reg::P31_DEFROST_EXTEND_TEMP, "P31 Defrost Extend")
DEFINE_P_SPINBOX_CB(p32_defrost_diff, arctic::reg::P32_DEFROST_TEMP_DIFF, "P32 Defrost Diff")
DEFINE_P_SPINBOX_CB(p33_defrost_extend_time, arctic::reg::P33_DEFROST_EXTEND_TIME, "P33 Extend Time")
DEFINE_P_SPINBOX_CB(p34_max_defrost, arctic::reg::P34_MAX_DEFROST_TIME, "P34 Max Defrost")
DEFINE_P_SPINBOX_CB(p35_defrost_exit, arctic::reg::P35_DEFROST_EXIT_TEMP, "P35 Defrost Exit")
DEFINE_P_SPINBOX_CB(p36_water_return_temp, arctic::reg::P36_WATER_RETURN_TEMP, "P36 Water Return Temp")
DEFINE_P_SPINBOX_CB(p37_water_return_time, arctic::reg::P37_WATER_RETURN_TIME, "P37 Water Return Time")
DEFINE_P_SPINBOX_CB(p38_low_ambient, arctic::reg::P38_LOW_AMBIENT_PROTECT, "P38 Low Ambient")
DEFINE_P_SPINBOX_CB(p39_freq_reduction, arctic::reg::P39_FREQ_REDUCTION, "P39 Freq Reduction")
DEFINE_P_SPINBOX_CB(p40_cooling_low_ambient, arctic::reg::P40_COOLING_LOW_AMBIENT, "P40 Cooling Low Ambient")
DEFINE_P_SPINBOX_CB(p41_eev_mode, arctic::reg::P41_EEV_SUPERHEAT_MODE, "P41 EEV Mode")
DEFINE_P_SPINBOX_CB(p42_eev_superheat, arctic::reg::P42_EEV_TARGET_SUPERHEAT, "P42 EEV Superheat")
DEFINE_P_SPINBOX_CB(p43_3way_valve, arctic::reg::P43_3WAY_VALVE_TIME, "P43 3-Way Valve")
DEFINE_P_SPINBOX_CB(p44_pump_mode, arctic::reg::P44_PUMP_TARGET_MODE, "P44 Pump Mode")
DEFINE_P_SPINBOX_CB(p45_pump_interval, arctic::reg::P45_PUMP_INTERVAL, "P45 Pump Interval")
DEFINE_P_SPINBOX_CB(p46_pump_low_ambient, arctic::reg::P46_PUMP_LOW_AMBIENT, "P46 Pump Low Ambient")
DEFINE_P_SPINBOX_CB(p47_waterway_cleaning, arctic::reg::P47_WATERWAY_CLEANING, "P47 Waterway Clean")

static void close_btn_cb(lv_event_t* e) {
    heatpump_control_hide();
}

static void update_timer_cb(lv_timer_t* timer) {
    // Refresh current values from heat pump state
    if (!state.shown) return;
    
    arctic::HeatPumpState hp = arctic::getState();
    
    if (!hp.connected) {
        show_status("Heat pump disconnected", true);
        return;
    }
    
    // Update power switch (without triggering callback)
    lv_obj_remove_event_cb(state.power_switch, power_switch_cb);
    if (hp.unit_on) {
        lv_obj_add_state(state.power_switch, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(state.power_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(state.power_switch, power_switch_cb, LV_EVENT_VALUE_CHANGED, nullptr);
}

static lv_obj_t* create_spinbox_with_buttons(lv_obj_t* parent, int32_t min_val, int32_t max_val, 
                                              int32_t initial, lv_event_cb_t value_cb) {
    // Container for spinbox + buttons
    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_set_size(cont, 160, 40);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cont, 0, LV_PART_MAIN);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cont, 5, LV_PART_MAIN);
    
    // Minus button
    lv_obj_t* btn_dec = lv_btn_create(cont);
    lv_obj_set_size(btn_dec, 35, 35);
    lv_obj_set_style_bg_color(btn_dec, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_t* lbl_dec = lv_label_create(btn_dec);
    lv_label_set_text(lbl_dec, "-");
    lv_obj_set_style_text_font(lbl_dec, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_center(lbl_dec);
    
    // Spinbox
    lv_obj_t* spinbox = lv_spinbox_create(cont);
    lv_spinbox_set_range(spinbox, min_val, max_val);
    lv_spinbox_set_digit_format(spinbox, 3, 0);  // 3 digits, 0 after decimal
    lv_spinbox_set_value(spinbox, initial);
    lv_obj_set_size(spinbox, 70, 35);
    lv_obj_set_style_bg_color(spinbox, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_text_color(spinbox, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(spinbox, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_align(spinbox, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    
    // Plus button
    lv_obj_t* btn_inc = lv_btn_create(cont);
    lv_obj_set_size(btn_inc, 35, 35);
    lv_obj_set_style_bg_color(btn_inc, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_t* lbl_inc = lv_label_create(btn_inc);
    lv_label_set_text(lbl_inc, "+");
    lv_obj_set_style_text_font(lbl_inc, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_center(lbl_inc);
    
    // Button callbacks
    lv_obj_add_event_cb(btn_dec, [](lv_event_t* e) {
        lv_obj_t* spinbox = (lv_obj_t*)lv_event_get_user_data(e);
        lv_spinbox_decrement(spinbox);
        lv_obj_send_event(spinbox, LV_EVENT_VALUE_CHANGED, nullptr);
    }, LV_EVENT_CLICKED, spinbox);
    
    lv_obj_add_event_cb(btn_inc, [](lv_event_t* e) {
        lv_obj_t* spinbox = (lv_obj_t*)lv_event_get_user_data(e);
        lv_spinbox_increment(spinbox);
        lv_obj_send_event(spinbox, LV_EVENT_VALUE_CHANGED, nullptr);
    }, LV_EVENT_CLICKED, spinbox);
    
    // Value change callback
    if (value_cb) {
        lv_obj_add_event_cb(spinbox, value_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    }
    
    return spinbox;
}

// ============================================================================
// Public Functions
// ============================================================================

void heatpump_control_show(heatpump_control_close_cb_t on_close) {
    if (state.shown) {
        return;
    }
    
    ESP_LOGI(TAG, "Showing heat pump control screen");
    state.on_close = on_close;
    
    arctic::HeatPumpState hp = arctic::getState();
    
    // Create full-screen overlay
    state.screen = lv_obj_create(NULL);
    lv_obj_set_size(state.screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(state.screen, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state.screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(state.screen, LV_OBJ_FLAG_SCROLLABLE);
    
    // Header with title and close button
    lv_obj_t* header = lv_obj_create(state.screen);
    lv_obj_set_size(header, LV_PCT(100), 70);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(header, 20, LV_PART_MAIN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "Heat Pump Controls");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);
    
    lv_obj_t* close_btn = lv_btn_create(header);
    lv_obj_set_size(close_btn, 50, 50);
    lv_obj_align(close_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(close_btn, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x2a3a5e), LV_STATE_PRESSED);
    lv_obj_set_style_radius(close_btn, 25, LV_PART_MAIN);
    lv_obj_add_event_cb(close_btn, close_btn_cb, LV_EVENT_CLICKED, nullptr);
    
    lv_obj_t* close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(close_lbl, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(close_lbl, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_center(close_lbl);
    
    // Scrollable content container
    state.scroll_container = lv_obj_create(state.screen);
    lv_obj_set_size(state.scroll_container, LV_PCT(100), 650);  // Screen 720 - header 70 = 650
    lv_obj_align(state.scroll_container, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(state.scroll_container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.scroll_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.scroll_container, 15, LV_PART_MAIN);
    lv_obj_set_flex_flow(state.scroll_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(state.scroll_container, 5, LV_PART_MAIN);
    
    // Status label
    state.status_label = lv_label_create(state.scroll_container);
    lv_label_set_text(state.status_label, hp.connected ? "Connected" : "Disconnected");
    lv_obj_set_style_text_font(state.status_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.status_label, 
        hp.connected ? COLOR_SUCCESS : COLOR_ERROR, LV_PART_MAIN);
    
    // =========================================================================
    // BASIC CONTROLS Section
    // =========================================================================
    create_section_header(state.scroll_container, "Basic Controls");
    
    // Power ON/OFF
    lv_obj_t* power_row = create_control_row(state.scroll_container, "Power");
    state.power_switch = lv_switch_create(power_row);
    lv_obj_align(state.power_switch, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(state.power_switch, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(state.power_switch, COLOR_SUCCESS, 
        static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | static_cast<lv_style_selector_t>(LV_STATE_CHECKED));
    if (hp.unit_on) {
        lv_obj_add_state(state.power_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(state.power_switch, power_switch_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    
    // Mode selector
    lv_obj_t* mode_row = create_control_row(state.scroll_container, "Mode");
    state.mode_dropdown = lv_dropdown_create(mode_row);
    lv_dropdown_set_options(state.mode_dropdown, 
        "Cooling\n"
        "Floor Heating\n"
        "Fan Coil Heating\n"
        "Hot Water\n"
        "Auto");
    lv_obj_set_size(state.mode_dropdown, 180, 40);
    lv_obj_align(state.mode_dropdown, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(state.mode_dropdown, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.mode_dropdown, COLOR_TEXT, LV_PART_MAIN);
    
    // Set current mode
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
    
    // Cooling setpoint
    lv_obj_t* cooling_row = create_control_row(state.scroll_container, "Cooling Setpoint");
    state.cooling_spinbox = create_spinbox_with_buttons(cooling_row, 5, 30, 
        hp.cooling_setpoint, cooling_spinbox_cb);
    lv_obj_align(lv_obj_get_parent(state.cooling_spinbox), LV_ALIGN_RIGHT_MID, 0, 0);
    
    // Heating setpoint
    lv_obj_t* heating_row = create_control_row(state.scroll_container, "Heating Setpoint");
    state.heating_spinbox = create_spinbox_with_buttons(heating_row, 20, 60, 
        hp.heating_setpoint, heating_spinbox_cb);
    lv_obj_align(lv_obj_get_parent(state.heating_spinbox), LV_ALIGN_RIGHT_MID, 0, 0);
    
    // Hot water setpoint
    lv_obj_t* hotwater_row = create_control_row(state.scroll_container, "Hot Water Setpoint");
    state.hotwater_spinbox = create_spinbox_with_buttons(hotwater_row, 30, 60, 
        hp.hot_water_setpoint, hotwater_spinbox_cb);
    lv_obj_align(lv_obj_get_parent(state.hotwater_spinbox), LV_ALIGN_RIGHT_MID, 0, 0);
    
    // =========================================================================
    // TECHNICIAN Section - P-Parameters
    // =========================================================================
    create_section_header(state.scroll_container, "Technician Settings - EEV");
    
    // P1 - EEV Initial Opening (0-500 steps)
    lv_obj_t* p1_row = create_control_row(state.scroll_container, "EEV Initial Opening (P1)");
    state.p1_eev_initial_spinbox = create_spinbox_with_buttons(p1_row, 0, 500, 
        250, p1_eev_initial_spinbox_cb);
    lv_obj_align(lv_obj_get_parent(state.p1_eev_initial_spinbox), LV_ALIGN_RIGHT_MID, 0, 0);
    
    // P41 - EEV Superheat Mode (0=Superheat, 1=Fixed)
    lv_obj_t* p41_row = create_control_row(state.scroll_container, "EEV Mode (P41) 0=SH 1=Fix");
    state.p41_eev_mode_spinbox = create_spinbox_with_buttons(p41_row, 0, 1, 
        0, p41_eev_mode_spinbox_cb);
    lv_obj_align(lv_obj_get_parent(state.p41_eev_mode_spinbox), LV_ALIGN_RIGHT_MID, 0, 0);
    
    // P42 - EEV Target Superheat
    lv_obj_t* p42_row = create_control_row(state.scroll_container, "EEV Target Superheat (P42)");
    state.p42_eev_superheat_spinbox = create_spinbox_with_buttons(p42_row, 0, 20, 
        5, p42_eev_superheat_spinbox_cb);
    lv_obj_align(lv_obj_get_parent(state.p42_eev_superheat_spinbox), LV_ALIGN_RIGHT_MID, 0, 0);
    
    create_section_header(state.scroll_container, "Technician Settings - Defrost");
    
    // P29 - Defrost Cycle (minutes)
    lv_obj_t* p29_row = create_control_row(state.scroll_container, "Defrost Cycle min (P29)");
    state.p29_defrost_cycle_spinbox = create_spinbox_with_buttons(p29_row, 30, 120, 
        60, p29_defrost_cycle_spinbox_cb);
    lv_obj_align(lv_obj_get_parent(state.p29_defrost_cycle_spinbox), LV_ALIGN_RIGHT_MID, 0, 0);
    
    // P30 - Defrost Enter Temp
    lv_obj_t* p30_row = create_control_row(state.scroll_container, "Defrost Enter °C (P30)");
    state.p30_defrost_enter_spinbox = create_spinbox_with_buttons(p30_row, -20, 5, 
        -5, p30_defrost_enter_spinbox_cb);
    lv_obj_align(lv_obj_get_parent(state.p30_defrost_enter_spinbox), LV_ALIGN_RIGHT_MID, 0, 0);
    
    // P31 - Ambient temp to extend defrost time
    lv_obj_t* p31_row = create_control_row(state.scroll_container, "Defrost Extend Temp (P31)");
    state.p31_defrost_extend_spinbox = create_spinbox_with_buttons(p31_row, -20, 10, 
        0, p31_defrost_extend_spinbox_cb);
    lv_obj_align(lv_obj_get_parent(state.p31_defrost_extend_spinbox), LV_ALIGN_RIGHT_MID, 0, 0);
    
    // P32 - Ambient-coil temp diff to enter defrost
    lv_obj_t* p32_row = create_control_row(state.scroll_container, "Defrost Temp Diff (P32)");
    state.p32_defrost_diff_spinbox = create_spinbox_with_buttons(p32_row, 0, 30, 
        10, p32_defrost_diff_spinbox_cb);
    lv_obj_align(lv_obj_get_parent(state.p32_defrost_diff_spinbox), LV_ALIGN_RIGHT_MID, 0, 0);
    
    // P33 - Extend defrost cycle time
    lv_obj_t* p33_row = create_control_row(state.scroll_container, "Defrost Extend min (P33)");
    state.p33_defrost_extend_time_spinbox = create_spinbox_with_buttons(p33_row, 0, 60, 
        15, p33_defrost_extend_time_spinbox_cb);
    lv_obj_align(lv_obj_get_parent(state.p33_defrost_extend_time_spinbox), LV_ALIGN_RIGHT_MID, 0, 0);
    
    // P34 - Maximum defrost time
    lv_obj_t* p34_row = create_control_row(state.scroll_container, "Max Defrost min (P34)");
    state.p34_max_defrost_spinbox = create_spinbox_with_buttons(p34_row, 1, 30, 
        10, p34_max_defrost_spinbox_cb);
    lv_obj_align(lv_obj_get_parent(state.p34_max_defrost_spinbox), LV_ALIGN_RIGHT_MID, 0, 0);
    
    // P35 - Defrost Exit Temp
    lv_obj_t* p35_row = create_control_row(state.scroll_container, "Defrost Exit °C (P35)");
    state.p35_defrost_exit_spinbox = create_spinbox_with_buttons(p35_row, 5, 30, 
        15, p35_defrost_exit_spinbox_cb);
    lv_obj_align(lv_obj_get_parent(state.p35_defrost_exit_spinbox), LV_ALIGN_RIGHT_MID, 0, 0);
    
    create_section_header(state.scroll_container, "Technician Settings - Protection");
    
    // P38 - Low ambient protection
    lv_obj_t* p38_row = create_control_row(state.scroll_container, "Low Ambient °C (P38)");
    state.p38_low_ambient_spinbox = create_spinbox_with_buttons(p38_row, -30, 10, 
        -20, p38_low_ambient_spinbox_cb);
    lv_obj_align(lv_obj_get_parent(state.p38_low_ambient_spinbox), LV_ALIGN_RIGHT_MID, 0, 0);
    
    // P39 - Freq reduction near target
    lv_obj_t* p39_row = create_control_row(state.scroll_container, "Freq Reduction °C (P39)");
    state.p39_freq_reduction_spinbox = create_spinbox_with_buttons(p39_row, 0, 10, 
        3, p39_freq_reduction_spinbox_cb);
    lv_obj_align(lv_obj_get_parent(state.p39_freq_reduction_spinbox), LV_ALIGN_RIGHT_MID, 0, 0);
    
    // P40 - Cooling low ambient protection
    lv_obj_t* p40_row = create_control_row(state.scroll_container, "Cool Low Ambient °C (P40)");
    state.p40_cooling_low_ambient_spinbox = create_spinbox_with_buttons(p40_row, -10, 20, 
        5, p40_cooling_low_ambient_spinbox_cb);
    lv_obj_align(lv_obj_get_parent(state.p40_cooling_low_ambient_spinbox), LV_ALIGN_RIGHT_MID, 0, 0);
    
    create_section_header(state.scroll_container, "Technician Settings - Auto Mode");
    
    // P13 - Maximum setting temperature
    lv_obj_t* p13_row = create_control_row(state.scroll_container, "Max Setting Temp (P13)");
    state.p13_max_temp_spinbox = create_spinbox_with_buttons(p13_row, 40, 65, 
        55, p13_max_temp_spinbox_cb);
    lv_obj_align(lv_obj_get_parent(state.p13_max_temp_spinbox), LV_ALIGN_RIGHT_MID, 0, 0);
    
    // P23 - Cooling ambient for auto mode
    lv_obj_t* p23_row = create_control_row(state.scroll_container, "Cooling Auto °C (P23)");
    state.p23_cooling_auto_spinbox = create_spinbox_with_buttons(p23_row, 15, 35, 
        25, p23_cooling_auto_spinbox_cb);
    lv_obj_align(lv_obj_get_parent(state.p23_cooling_auto_spinbox), LV_ALIGN_RIGHT_MID, 0, 0);
    
    // P24 - Heating ambient for auto mode
    lv_obj_t* p24_row = create_control_row(state.scroll_container, "Heating Auto °C (P24)");
    state.p24_heating_auto_spinbox = create_spinbox_with_buttons(p24_row, 5, 25, 
        15, p24_heating_auto_spinbox_cb);
    lv_obj_align(lv_obj_get_parent(state.p24_heating_auto_spinbox), LV_ALIGN_RIGHT_MID, 0, 0);
    
    // P28 - Mode switch delay under auto mode
    lv_obj_t* p28_row = create_control_row(state.scroll_container, "Mode Switch Delay (P28)");
    state.p28_mode_switch_spinbox = create_spinbox_with_buttons(p28_row, 0, 60, 
        10, p28_mode_switch_spinbox_cb);
    lv_obj_align(lv_obj_get_parent(state.p28_mode_switch_spinbox), LV_ALIGN_RIGHT_MID, 0, 0);
    
    create_section_header(state.scroll_container, "Technician Settings - Pump & Valve");
    
    // P5 - Sterilizing time
    lv_obj_t* p5_row = create_control_row(state.scroll_container, "Sterilize Time min (P5)");
    state.p5_sterilize_time_spinbox = create_spinbox_with_buttons(p5_row, 0, 120, 
        30, p5_sterilize_spinbox_cb);
    lv_obj_align(lv_obj_get_parent(state.p5_sterilize_time_spinbox), LV_ALIGN_RIGHT_MID, 0, 0);
    
    // P36 - Water return cycle temp
    lv_obj_t* p36_row = create_control_row(state.scroll_container, "Water Return °C (P36)");
    state.p36_water_return_temp_spinbox = create_spinbox_with_buttons(p36_row, 20, 50, 
        35, p36_water_return_temp_spinbox_cb);
    lv_obj_align(lv_obj_get_parent(state.p36_water_return_temp_spinbox), LV_ALIGN_RIGHT_MID, 0, 0);
    
    // P37 - Water return cycle time
    lv_obj_t* p37_row = create_control_row(state.scroll_container, "Water Return min (P37)");
    state.p37_water_return_time_spinbox = create_spinbox_with_buttons(p37_row, 0, 60, 
        10, p37_water_return_time_spinbox_cb);
    lv_obj_align(lv_obj_get_parent(state.p37_water_return_time_spinbox), LV_ALIGN_RIGHT_MID, 0, 0);
    
    // P43 - 3-way valve switching time
    lv_obj_t* p43_row = create_control_row(state.scroll_container, "3-Way Valve sec (P43)");
    state.p43_3way_valve_spinbox = create_spinbox_with_buttons(p43_row, 0, 300, 
        60, p43_3way_valve_spinbox_cb);
    lv_obj_align(lv_obj_get_parent(state.p43_3way_valve_spinbox), LV_ALIGN_RIGHT_MID, 0, 0);
    
    // P44 - Pump mode at target (0=per P45, 1=OFF, 2=ON)
    lv_obj_t* p44_row = create_control_row(state.scroll_container, "Pump Mode (P44) 0/1/2");
    state.p44_pump_mode_spinbox = create_spinbox_with_buttons(p44_row, 0, 2, 
        0, p44_pump_mode_spinbox_cb);
    lv_obj_align(lv_obj_get_parent(state.p44_pump_mode_spinbox), LV_ALIGN_RIGHT_MID, 0, 0);
    
    // P45 - Pump running interval
    lv_obj_t* p45_row = create_control_row(state.scroll_container, "Pump Interval min (P45)");
    state.p45_pump_interval_spinbox = create_spinbox_with_buttons(p45_row, 0, 60, 
        10, p45_pump_interval_spinbox_cb);
    lv_obj_align(lv_obj_get_parent(state.p45_pump_interval_spinbox), LV_ALIGN_RIGHT_MID, 0, 0);
    
    // P46 - Low ambient to turn on pump in standby
    lv_obj_t* p46_row = create_control_row(state.scroll_container, "Pump Low Ambient (P46)");
    state.p46_pump_low_ambient_spinbox = create_spinbox_with_buttons(p46_row, -20, 10, 
        0, p46_pump_low_ambient_spinbox_cb);
    lv_obj_align(lv_obj_get_parent(state.p46_pump_low_ambient_spinbox), LV_ALIGN_RIGHT_MID, 0, 0);
    
    // P47 - Waterway cleaning (0=OFF, 1=Pump, 2=+3WV1, 3=+3WV1+3WV2)
    lv_obj_t* p47_row = create_control_row(state.scroll_container, "Waterway Clean (P47) 0-3");
    state.p47_waterway_cleaning_spinbox = create_spinbox_with_buttons(p47_row, 0, 3, 
        0, p47_waterway_cleaning_spinbox_cb);
    lv_obj_align(lv_obj_get_parent(state.p47_waterway_cleaning_spinbox), LV_ALIGN_RIGHT_MID, 0, 0);

    state.shown = true;
    
    // Create update timer
    state.update_timer = lv_timer_create(update_timer_cb, 2000, nullptr);
    
    // Load the screen
    lv_scr_load(state.screen);
}

void heatpump_control_hide(void) {
    if (!state.shown) {
        return;
    }
    
    ESP_LOGI(TAG, "Hiding heat pump control screen");
    
    if (state.update_timer) {
        lv_timer_del(state.update_timer);
        state.update_timer = nullptr;
    }
    
    heatpump_control_close_cb_t cb = state.on_close;
    lv_obj_t* screen_to_delete = state.screen;
    
    // Mark as not shown and clear state BEFORE callback
    state.shown = false;
    state.on_close = nullptr;
    state.screen = nullptr;
    state.power_switch = nullptr;
    state.mode_dropdown = nullptr;
    state.cooling_spinbox = nullptr;
    state.heating_spinbox = nullptr;
    state.hotwater_spinbox = nullptr;
    // Clear all P-parameter spinboxes
    state.p1_eev_initial_spinbox = nullptr;
    state.p5_sterilize_time_spinbox = nullptr;
    state.p13_max_temp_spinbox = nullptr;
    state.p23_cooling_auto_spinbox = nullptr;
    state.p24_heating_auto_spinbox = nullptr;
    state.p28_mode_switch_spinbox = nullptr;
    state.p29_defrost_cycle_spinbox = nullptr;
    state.p30_defrost_enter_spinbox = nullptr;
    state.p31_defrost_extend_spinbox = nullptr;
    state.p32_defrost_diff_spinbox = nullptr;
    state.p33_defrost_extend_time_spinbox = nullptr;
    state.p34_max_defrost_spinbox = nullptr;
    state.p35_defrost_exit_spinbox = nullptr;
    state.p36_water_return_temp_spinbox = nullptr;
    state.p37_water_return_time_spinbox = nullptr;
    state.p38_low_ambient_spinbox = nullptr;
    state.p39_freq_reduction_spinbox = nullptr;
    state.p40_cooling_low_ambient_spinbox = nullptr;
    state.p41_eev_mode_spinbox = nullptr;
    state.p42_eev_superheat_spinbox = nullptr;
    state.p43_3way_valve_spinbox = nullptr;
    state.p44_pump_mode_spinbox = nullptr;
    state.p45_pump_interval_spinbox = nullptr;
    state.p46_pump_low_ambient_spinbox = nullptr;
    state.p47_waterway_cleaning_spinbox = nullptr;
    state.status_label = nullptr;
    
    // Call callback first to load the previous screen
    if (cb) {
        cb();
    }
    
    // Now delete the control screen (no longer active)
    if (screen_to_delete) {
        lv_obj_del(screen_to_delete);
    }
}

bool heatpump_control_is_shown(void) {
    return state.shown;
}
