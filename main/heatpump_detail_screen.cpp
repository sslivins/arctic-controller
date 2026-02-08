/*
 * Arctic Heat Pump Controller
 * Heat Pump Detail Screen - Shows all sensor readings
 */

#include "heatpump_detail_screen.h"
#include "modbus/arctic_heatpump.h"
#include "modbus/arctic_registers.h"
#include "ui_common.h"
#include <esp_log.h>
#include <stdio.h>

static const char* TAG = "hp_detail";

// ============================================================================
// Colors (matching main screen)
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

// ============================================================================
// State
// ============================================================================
static struct {
    bool shown = false;
    lv_obj_t* screen = nullptr;
    lv_obj_t* scroll_container = nullptr;
    heatpump_detail_close_cb_t on_close = nullptr;
    lv_timer_t* update_timer = nullptr;
    
    // Temperature labels
    lv_obj_t* tank_temp = nullptr;
    lv_obj_t* outlet_temp = nullptr;
    lv_obj_t* inlet_temp = nullptr;
    lv_obj_t* outdoor_temp = nullptr;
    lv_obj_t* discharge_temp = nullptr;
    lv_obj_t* suction_temp = nullptr;
    lv_obj_t* outdoor_coil_temp = nullptr;
    lv_obj_t* indoor_coil_temp = nullptr;
    lv_obj_t* ipm_temp = nullptr;
    
    // System reading labels
    lv_obj_t* compressor_freq = nullptr;
    lv_obj_t* fan_speed = nullptr;
    lv_obj_t* ac_voltage = nullptr;
    lv_obj_t* ac_current = nullptr;
    lv_obj_t* dc_voltage = nullptr;
    lv_obj_t* dc_current = nullptr;
    lv_obj_t* high_pressure = nullptr;
    lv_obj_t* low_pressure = nullptr;
    lv_obj_t* primary_eev = nullptr;
    lv_obj_t* secondary_eev = nullptr;
    
    // Setpoint labels
    lv_obj_t* cooling_setpoint = nullptr;
    lv_obj_t* heating_setpoint = nullptr;
    lv_obj_t* hotwater_setpoint = nullptr;
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

static lv_obj_t* create_reading_row(lv_obj_t* parent, const char* label, lv_obj_t** value_label_out) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 36);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(row, 5, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t* name_lbl = lv_label_create(row);
    lv_label_set_text(name_lbl, label);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(name_lbl, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    
    lv_obj_t* val_lbl = lv_label_create(row);
    lv_label_set_text(val_lbl, "--");
    lv_obj_set_style_text_font(val_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(val_lbl, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(val_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
    
    *value_label_out = val_lbl;
    return row;
}

static void update_readings() {
    if (!state.shown) return;
    
    arctic::HeatPumpState hp = arctic::getState();
    char buf[32];
    
    if (!hp.connected) {
        // Set all to "--" when disconnected
        const char* na = "--";
        lv_label_set_text(state.tank_temp, na);
        lv_label_set_text(state.outlet_temp, na);
        lv_label_set_text(state.inlet_temp, na);
        lv_label_set_text(state.outdoor_temp, na);
        lv_label_set_text(state.discharge_temp, na);
        lv_label_set_text(state.suction_temp, na);
        lv_label_set_text(state.outdoor_coil_temp, na);
        lv_label_set_text(state.indoor_coil_temp, na);
        lv_label_set_text(state.ipm_temp, na);
        lv_label_set_text(state.compressor_freq, na);
        lv_label_set_text(state.fan_speed, na);
        lv_label_set_text(state.ac_voltage, na);
        lv_label_set_text(state.ac_current, na);
        lv_label_set_text(state.dc_voltage, na);
        lv_label_set_text(state.dc_current, na);
        lv_label_set_text(state.high_pressure, na);
        lv_label_set_text(state.low_pressure, na);
        lv_label_set_text(state.primary_eev, na);
        lv_label_set_text(state.secondary_eev, na);
        lv_label_set_text(state.cooling_setpoint, na);
        lv_label_set_text(state.heating_setpoint, na);
        lv_label_set_text(state.hotwater_setpoint, na);
        return;
    }
    
    // Temperatures
    snprintf(buf, sizeof(buf), "%d °C", hp.water_tank_temp);
    lv_label_set_text(state.tank_temp, buf);
    
    snprintf(buf, sizeof(buf), "%d °C", hp.outlet_water_temp);
    lv_label_set_text(state.outlet_temp, buf);
    
    snprintf(buf, sizeof(buf), "%d °C", hp.inlet_water_temp);
    lv_label_set_text(state.inlet_temp, buf);
    
    snprintf(buf, sizeof(buf), "%d °C", hp.outdoor_ambient_temp);
    lv_label_set_text(state.outdoor_temp, buf);
    
    snprintf(buf, sizeof(buf), "%d °C", hp.discharge_temp);
    lv_label_set_text(state.discharge_temp, buf);
    
    snprintf(buf, sizeof(buf), "%d °C", hp.suction_temp);
    lv_label_set_text(state.suction_temp, buf);
    
    snprintf(buf, sizeof(buf), "%d °C", hp.outdoor_coil_temp);
    lv_label_set_text(state.outdoor_coil_temp, buf);
    
    snprintf(buf, sizeof(buf), "%d °C", hp.indoor_coil_temp);
    lv_label_set_text(state.indoor_coil_temp, buf);
    
    snprintf(buf, sizeof(buf), "%d °C", hp.ipm_temp);
    lv_label_set_text(state.ipm_temp, buf);
    
    // System readings
    snprintf(buf, sizeof(buf), "%d Hz", hp.compressor_freq);
    lv_label_set_text(state.compressor_freq, buf);
    
    snprintf(buf, sizeof(buf), "%d RPM", hp.fan_speed);
    lv_label_set_text(state.fan_speed, buf);
    
    snprintf(buf, sizeof(buf), "%d V", hp.ac_voltage);
    lv_label_set_text(state.ac_voltage, buf);
    
    snprintf(buf, sizeof(buf), "%d A", hp.ac_current);
    lv_label_set_text(state.ac_current, buf);
    
    snprintf(buf, sizeof(buf), "%.1f V", hp.getDcVoltageV());
    lv_label_set_text(state.dc_voltage, buf);
    
    snprintf(buf, sizeof(buf), "%d A", hp.dc_current);
    lv_label_set_text(state.dc_current, buf);
    
    snprintf(buf, sizeof(buf), "%.2f MPa", hp.getHighPressureMPa());
    lv_label_set_text(state.high_pressure, buf);
    
    snprintf(buf, sizeof(buf), "%.2f MPa", hp.getLowPressureMPa());
    lv_label_set_text(state.low_pressure, buf);
    
    snprintf(buf, sizeof(buf), "%d steps", hp.primary_eev_opening);
    lv_label_set_text(state.primary_eev, buf);
    
    snprintf(buf, sizeof(buf), "%d steps", hp.secondary_eev_opening);
    lv_label_set_text(state.secondary_eev, buf);
    
    // Setpoints
    snprintf(buf, sizeof(buf), "%d °C", hp.cooling_setpoint);
    lv_label_set_text(state.cooling_setpoint, buf);
    
    snprintf(buf, sizeof(buf), "%d °C", hp.heating_setpoint);
    lv_label_set_text(state.heating_setpoint, buf);
    
    snprintf(buf, sizeof(buf), "%d °C", hp.hot_water_setpoint);
    lv_label_set_text(state.hotwater_setpoint, buf);
}

static void update_timer_cb(lv_timer_t* timer) {
    update_readings();
}

static void close_btn_cb(lv_event_t* e) {
    heatpump_detail_hide();
}

// ============================================================================
// Public Functions
// ============================================================================

void heatpump_detail_show(heatpump_detail_close_cb_t on_close) {
    if (state.shown) {
        return;
    }
    
    ESP_LOGI(TAG, "Showing heat pump detail screen");
    state.on_close = on_close;
    
    // Create full-screen overlay
    state.screen = lv_obj_create(NULL);
    lv_obj_set_size(state.screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(state.screen, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state.screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_flex_flow(state.screen, LV_FLEX_FLOW_COLUMN);  // Vertical flex layout
    lv_obj_set_style_pad_all(state.screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(state.screen, LV_OBJ_FLAG_SCROLLABLE);
    
    // Header with title and close button
    lv_obj_t* header = lv_obj_create(state.screen);
    lv_obj_set_size(header, LV_PCT(100), 70);
    lv_obj_set_style_bg_color(header, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(header, 20, LV_PART_MAIN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "Sensor Readings");
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
    
    // Scrollable content container - use flex grow to fill remaining space
    state.scroll_container = lv_obj_create(state.screen);
    lv_obj_set_width(state.scroll_container, LV_PCT(100));
    lv_obj_set_flex_grow(state.scroll_container, 1);  // Take all remaining vertical space
    lv_obj_set_style_bg_opa(state.scroll_container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.scroll_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.scroll_container, 15, LV_PART_MAIN);
    lv_obj_set_flex_flow(state.scroll_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(state.scroll_container, 5, LV_PART_MAIN);
    
    // =========================================================================
    // TEMPERATURES Section
    // =========================================================================
    create_section_header(state.scroll_container, "Temperatures");
    create_reading_row(state.scroll_container, "Water Tank", &state.tank_temp);
    create_reading_row(state.scroll_container, "Water Outlet", &state.outlet_temp);
    create_reading_row(state.scroll_container, "Water Inlet", &state.inlet_temp);
    create_reading_row(state.scroll_container, "Outdoor Ambient", &state.outdoor_temp);
    create_reading_row(state.scroll_container, "Discharge", &state.discharge_temp);
    create_reading_row(state.scroll_container, "Suction", &state.suction_temp);
    create_reading_row(state.scroll_container, "Outdoor Coil", &state.outdoor_coil_temp);
    create_reading_row(state.scroll_container, "Indoor Coil", &state.indoor_coil_temp);
    create_reading_row(state.scroll_container, "IPM Module", &state.ipm_temp);
    
    // =========================================================================
    // SYSTEM READINGS Section
    // =========================================================================
    create_section_header(state.scroll_container, "System Readings");
    create_reading_row(state.scroll_container, "Compressor Freq", &state.compressor_freq);
    create_reading_row(state.scroll_container, "Fan Speed", &state.fan_speed);
    create_reading_row(state.scroll_container, "AC Voltage", &state.ac_voltage);
    create_reading_row(state.scroll_container, "AC Current", &state.ac_current);
    create_reading_row(state.scroll_container, "DC Voltage", &state.dc_voltage);
    create_reading_row(state.scroll_container, "DC Current", &state.dc_current);
    create_reading_row(state.scroll_container, "High Pressure", &state.high_pressure);
    create_reading_row(state.scroll_container, "Low Pressure", &state.low_pressure);
    create_reading_row(state.scroll_container, "Primary EEV", &state.primary_eev);
    create_reading_row(state.scroll_container, "Secondary EEV", &state.secondary_eev);
    
    // =========================================================================
    // SETPOINTS Section
    // =========================================================================
    create_section_header(state.scroll_container, "Setpoints");
    create_reading_row(state.scroll_container, "Cooling", &state.cooling_setpoint);
    create_reading_row(state.scroll_container, "Heating", &state.heating_setpoint);
    create_reading_row(state.scroll_container, "Hot Water", &state.hotwater_setpoint);
    
    state.shown = true;
    
    // Create update timer
    state.update_timer = lv_timer_create(update_timer_cb, 1000, nullptr);
    
    // Initial update
    update_readings();
    
    // Load the screen
    lv_scr_load(state.screen);
}

void heatpump_detail_hide(void) {
    if (!state.shown) {
        return;
    }
    
    ESP_LOGI(TAG, "Hiding heat pump detail screen");
    
    if (state.update_timer) {
        lv_timer_del(state.update_timer);
        state.update_timer = nullptr;
    }
    
    heatpump_detail_close_cb_t cb = state.on_close;
    lv_obj_t* screen_to_delete = state.screen;
    
    // Mark as not shown and clear state BEFORE callback
    state.shown = false;
    state.on_close = nullptr;
    state.screen = nullptr;
    
    // Reset all label pointers
    state.tank_temp = nullptr;
    state.outlet_temp = nullptr;
    state.inlet_temp = nullptr;
    state.outdoor_temp = nullptr;
    state.discharge_temp = nullptr;
    state.suction_temp = nullptr;
    state.outdoor_coil_temp = nullptr;
    state.indoor_coil_temp = nullptr;
    state.ipm_temp = nullptr;
    state.compressor_freq = nullptr;
    state.fan_speed = nullptr;
    state.ac_voltage = nullptr;
    state.ac_current = nullptr;
    state.dc_voltage = nullptr;
    state.dc_current = nullptr;
    state.high_pressure = nullptr;
    state.low_pressure = nullptr;
    state.primary_eev = nullptr;
    state.secondary_eev = nullptr;
    state.cooling_setpoint = nullptr;
    state.heating_setpoint = nullptr;
    state.hotwater_setpoint = nullptr;
    
    // Call callback first to load the previous screen
    if (cb) {
        cb();
    }
    
    // Now delete the detail screen (no longer active)
    if (screen_to_delete) {
        lv_obj_del(screen_to_delete);
    }
}

bool heatpump_detail_is_shown(void) {
    return state.shown;
}
