/*
 * Arctic Heat Pump Controller
 * Heat Pump Temperatures Screen Implementation
 * 
 * Full-screen display of all temperature sensors with large, readable fonts.
 */

#include "heatpump_temps_screen.h"
#include "nav_bar.h"
#include "modbus/arctic_heatpump.h"
#include "ui_common.h"
#include "fonts/fonts.h"
#include "app_preferences.h"
#include "i18n/i18n.h"
#include <esp_log.h>
#include <stdio.h>

static const char* TAG = "hp_temps";

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
#define COLOR_WARM          lv_color_hex(0xf97316)
#define COLOR_COLD          lv_color_hex(0x3b82f6)

// ============================================================================
// State
// ============================================================================
static struct {
    bool shown = false;
    lv_obj_t* screen = nullptr;
    heatpump_temps_close_cb_t on_close = nullptr;
    lv_timer_t* update_timer = nullptr;
    
    // Temperature value labels (right side of each row)
    lv_obj_t* tank_temp = nullptr;
    lv_obj_t* outlet_temp = nullptr;
    lv_obj_t* inlet_temp = nullptr;
    lv_obj_t* outdoor_temp = nullptr;
    lv_obj_t* discharge_temp = nullptr;
    lv_obj_t* suction_temp = nullptr;
    lv_obj_t* outdoor_coil_temp = nullptr;
    lv_obj_t* indoor_coil_temp = nullptr;
    lv_obj_t* ipm_temp = nullptr;

    // System - compressor readings
    lv_obj_t* compressor_freq = nullptr;
    lv_obj_t* fan_speed = nullptr;
    // System - electrical readings
    lv_obj_t* ac_voltage = nullptr;
    lv_obj_t* ac_current = nullptr;
    lv_obj_t* dc_voltage = nullptr;
    lv_obj_t* dc_current = nullptr;
    // System - expansion valve readings
    lv_obj_t* primary_eev = nullptr;
    lv_obj_t* secondary_eev = nullptr;
    // System - setpoints
    lv_obj_t* cooling_setpoint = nullptr;
    lv_obj_t* heating_setpoint = nullptr;
    lv_obj_t* hotwater_setpoint = nullptr;
} state;

// ============================================================================
// Helper Functions
// ============================================================================

static lv_obj_t* create_temp_row(lv_obj_t* parent, const char* label, lv_obj_t** value_label_out) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 75);
    lv_obj_set_style_bg_color(row, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 25, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    
    // Label on left - larger font
    lv_obj_t* name_lbl = lv_label_create(row);
    lv_label_set_text(name_lbl, label);
    lv_obj_set_style_text_font(name_lbl, &montserrat_24_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(name_lbl, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    
    // Value on right - large and bold
    lv_obj_t* val_lbl = lv_label_create(row);
    lv_label_set_text(val_lbl, "-- °C");
    lv_obj_set_style_text_font(val_lbl, &montserrat_32_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(val_lbl, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(val_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
    
    *value_label_out = val_lbl;
    return row;
}

static lv_obj_t* create_section_header(lv_obj_t* parent, const char* title) {
    lv_obj_t* header = lv_obj_create(parent);
    lv_obj_set_size(header, LV_PCT(100), 50);
    lv_obj_set_style_bg_color(header, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(header, 20, LV_PART_MAIN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl = lv_label_create(header);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, UI_FONT_SECTION, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    return header;
}

static lv_obj_t* create_reading_row(lv_obj_t* parent, const char* label, lv_obj_t** value_label_out) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 75);
    lv_obj_set_style_bg_color(row, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 25, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* name_lbl = lv_label_create(row);
    lv_label_set_text(name_lbl, label);
    lv_obj_set_style_text_font(name_lbl, &montserrat_24_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(name_lbl, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t* val_lbl = lv_label_create(row);
    lv_label_set_text(val_lbl, "--");
    lv_obj_set_style_text_font(val_lbl, &montserrat_32_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(val_lbl, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(val_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    *value_label_out = val_lbl;
    return row;
}
static lv_color_t get_temp_color(int16_t temp) {
    if (temp >= 50) return COLOR_WARM;
    if (temp <= 5) return COLOR_COLD;
    return COLOR_TEXT;
}

static void update_readings() {
    if (!state.shown) return;
    
    arctic::HeatPumpState hp = arctic::getState();
    bool demo_mode = app_prefs_is_demo_mode();
    char buf[32];
    
    // Helper to format temperature with unit conversion
    auto format_temp = [&buf](int16_t temp_c) {
        snprintf(buf, sizeof(buf), "%d %s", 
                 app_prefs_convert_temp(temp_c), app_prefs_temp_unit_str());
    };
    
    if (demo_mode) {
        // Demo mode - show simulated temperatures
        format_temp(42);  // Tank
        lv_label_set_text(state.tank_temp, buf);
        lv_obj_set_style_text_color(state.tank_temp, get_temp_color(42), LV_PART_MAIN);
        
        format_temp(45);  // Outlet
        lv_label_set_text(state.outlet_temp, buf);
        lv_obj_set_style_text_color(state.outlet_temp, get_temp_color(45), LV_PART_MAIN);
        
        format_temp(38);  // Inlet
        lv_label_set_text(state.inlet_temp, buf);
        lv_obj_set_style_text_color(state.inlet_temp, get_temp_color(38), LV_PART_MAIN);
        
        format_temp(22);  // Outdoor ambient
        lv_label_set_text(state.outdoor_temp, buf);
        lv_obj_set_style_text_color(state.outdoor_temp, get_temp_color(22), LV_PART_MAIN);
        
        format_temp(85);  // Discharge
        lv_label_set_text(state.discharge_temp, buf);
        lv_obj_set_style_text_color(state.discharge_temp, get_temp_color(85), LV_PART_MAIN);
        
        format_temp(12);  // Suction
        lv_label_set_text(state.suction_temp, buf);
        lv_obj_set_style_text_color(state.suction_temp, get_temp_color(12), LV_PART_MAIN);
        
        format_temp(35);  // Outdoor coil
        lv_label_set_text(state.outdoor_coil_temp, buf);
        lv_obj_set_style_text_color(state.outdoor_coil_temp, get_temp_color(35), LV_PART_MAIN);
        
        format_temp(40);  // Indoor coil
        lv_label_set_text(state.indoor_coil_temp, buf);
        lv_obj_set_style_text_color(state.indoor_coil_temp, get_temp_color(40), LV_PART_MAIN);
        
        format_temp(55);  // IPM
        lv_label_set_text(state.ipm_temp, buf);
        lv_obj_set_style_text_color(state.ipm_temp, get_temp_color(55), LV_PART_MAIN);

        // System - demo values
        lv_label_set_text(state.compressor_freq, "60 Hz");
        lv_obj_set_style_text_color(state.compressor_freq, COLOR_SUCCESS, LV_PART_MAIN);
        lv_label_set_text(state.fan_speed, "850 RPM");
        lv_obj_set_style_text_color(state.fan_speed, COLOR_SUCCESS, LV_PART_MAIN);
        lv_label_set_text(state.ac_voltage, "230 V");
        lv_label_set_text(state.ac_current, "5 A");
        lv_label_set_text(state.dc_voltage, "380.0 V");
        lv_label_set_text(state.dc_current, "4 A");
        lv_label_set_text(state.primary_eev, "350 steps");
        lv_label_set_text(state.secondary_eev, "200 steps");
        snprintf(buf, sizeof(buf), "%d %s", app_prefs_convert_temp(20), app_prefs_temp_unit_str());
        lv_label_set_text(state.cooling_setpoint, buf);
        snprintf(buf, sizeof(buf), "%d %s", app_prefs_convert_temp(45), app_prefs_temp_unit_str());
        lv_label_set_text(state.heating_setpoint, buf);
        snprintf(buf, sizeof(buf), "%d %s", app_prefs_convert_temp(50), app_prefs_temp_unit_str());
        lv_label_set_text(state.hotwater_setpoint, buf);
        return;
    }
    
    if (!hp.connected) {
        // Disconnected - show placeholders
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
        // System
        lv_label_set_text(state.compressor_freq, na);
        lv_label_set_text(state.fan_speed, na);
        lv_label_set_text(state.ac_voltage, na);
        lv_label_set_text(state.ac_current, na);
        lv_label_set_text(state.dc_voltage, na);
        lv_label_set_text(state.dc_current, na);
        lv_label_set_text(state.primary_eev, na);
        lv_label_set_text(state.secondary_eev, na);
        lv_label_set_text(state.cooling_setpoint, na);
        lv_label_set_text(state.heating_setpoint, na);
        lv_label_set_text(state.hotwater_setpoint, na);
        return;
    }
    
    // Real mode - update all temperatures with color coding
    format_temp(hp.water_tank_temp);
    lv_label_set_text(state.tank_temp, buf);
    lv_obj_set_style_text_color(state.tank_temp, get_temp_color(hp.water_tank_temp), LV_PART_MAIN);
    
    format_temp(hp.outlet_water_temp);
    lv_label_set_text(state.outlet_temp, buf);
    lv_obj_set_style_text_color(state.outlet_temp, get_temp_color(hp.outlet_water_temp), LV_PART_MAIN);
    
    format_temp(hp.inlet_water_temp);
    lv_label_set_text(state.inlet_temp, buf);
    lv_obj_set_style_text_color(state.inlet_temp, get_temp_color(hp.inlet_water_temp), LV_PART_MAIN);
    
    format_temp(hp.outdoor_ambient_temp);
    lv_label_set_text(state.outdoor_temp, buf);
    lv_obj_set_style_text_color(state.outdoor_temp, get_temp_color(hp.outdoor_ambient_temp), LV_PART_MAIN);
    
    format_temp(hp.discharge_temp);
    lv_label_set_text(state.discharge_temp, buf);
    lv_obj_set_style_text_color(state.discharge_temp, get_temp_color(hp.discharge_temp), LV_PART_MAIN);
    
    format_temp(hp.suction_temp);
    lv_label_set_text(state.suction_temp, buf);
    lv_obj_set_style_text_color(state.suction_temp, get_temp_color(hp.suction_temp), LV_PART_MAIN);
    
    format_temp(hp.outdoor_coil_temp);
    lv_label_set_text(state.outdoor_coil_temp, buf);
    lv_obj_set_style_text_color(state.outdoor_coil_temp, get_temp_color(hp.outdoor_coil_temp), LV_PART_MAIN);
    
    format_temp(hp.indoor_coil_temp);
    lv_label_set_text(state.indoor_coil_temp, buf);
    lv_obj_set_style_text_color(state.indoor_coil_temp, get_temp_color(hp.indoor_coil_temp), LV_PART_MAIN);
    
    format_temp(hp.ipm_temp);
    lv_label_set_text(state.ipm_temp, buf);
    lv_obj_set_style_text_color(state.ipm_temp, get_temp_color(hp.ipm_temp), LV_PART_MAIN);

    // System - real values
    snprintf(buf, sizeof(buf), "%d Hz", hp.compressor_freq);
    lv_label_set_text(state.compressor_freq, buf);
    lv_obj_set_style_text_color(state.compressor_freq,
        hp.compressor_freq > 0 ? COLOR_SUCCESS : COLOR_TEXT_DIM, LV_PART_MAIN);

    snprintf(buf, sizeof(buf), "%d RPM", hp.fan_speed);
    lv_label_set_text(state.fan_speed, buf);
    lv_obj_set_style_text_color(state.fan_speed,
        hp.fan_speed > 0 ? COLOR_SUCCESS : COLOR_TEXT_DIM, LV_PART_MAIN);

    snprintf(buf, sizeof(buf), "%d V", hp.ac_voltage);
    lv_label_set_text(state.ac_voltage, buf);

    snprintf(buf, sizeof(buf), "%d A", hp.ac_current);
    lv_label_set_text(state.ac_current, buf);

    // dc_voltage is in volts (conversion owned by the macon library)
    snprintf(buf, sizeof(buf), "%.0f V", hp.getDcVoltageV());
    lv_label_set_text(state.dc_voltage, buf);

    snprintf(buf, sizeof(buf), "%d A", hp.dc_current);
    lv_label_set_text(state.dc_current, buf);

    snprintf(buf, sizeof(buf), "%d steps", hp.primary_eev_opening);
    lv_label_set_text(state.primary_eev, buf);

    snprintf(buf, sizeof(buf), "%d steps", hp.secondary_eev_opening);
    lv_label_set_text(state.secondary_eev, buf);

    snprintf(buf, sizeof(buf), "%d %s", app_prefs_convert_temp(hp.cooling_setpoint), app_prefs_temp_unit_str());
    lv_label_set_text(state.cooling_setpoint, buf);

    snprintf(buf, sizeof(buf), "%d %s", app_prefs_convert_temp(hp.heating_setpoint), app_prefs_temp_unit_str());
    lv_label_set_text(state.heating_setpoint, buf);

    snprintf(buf, sizeof(buf), "%d %s", app_prefs_convert_temp(hp.hot_water_setpoint), app_prefs_temp_unit_str());
    lv_label_set_text(state.hotwater_setpoint, buf);
}

static void update_timer_cb(lv_timer_t* timer) {
    update_readings();
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
    heatpump_temps_close_cb_t cb = state.on_close;
    state.on_close = nullptr;
    state.shown = false;
    state.screen = nullptr;
    
    // Call callback - it will load the previous screen with auto_del=true
    if (cb) {
        cb();
    }
}

// ============================================================================
// Public Functions
// ============================================================================

void heatpump_temps_create_in(lv_obj_t* parent) {
    if (state.shown) {
        return;
    }

    ESP_LOGI(TAG, "Building temperatures tab");
    state.on_close = nullptr;

    // The panel provided by the tab shell is our root; build directly into it.
    state.screen = parent;

    // Scrollable content fills the panel; reserve room for the persistent nav
    // bar (drawn by the tab shell) at the bottom.
    lv_obj_t* content = lv_obj_create(state.screen);
    lv_obj_set_size(content, LV_PCT(100), LV_PCT(100));
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(content, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(content, NAV_BAR_H + 20, LV_PART_MAIN);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 12, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
    
    // ===== Temperatures section =====
    create_section_header(content, i18n_get(STR_HP_TEMPERATURES));

    // Temperature rows - Water temperatures first
    create_temp_row(content, i18n_get(STR_HP_WATER_TANK), &state.tank_temp);
    create_temp_row(content, i18n_get(STR_HP_WATER_OUTLET), &state.outlet_temp);
    create_temp_row(content, i18n_get(STR_HP_WATER_INLET), &state.inlet_temp);
    
    // Ambient and refrigerant temps
    create_temp_row(content, i18n_get(STR_HP_OUTDOOR_AMBIENT), &state.outdoor_temp);
    create_temp_row(content, i18n_get(STR_HP_DISCHARGE), &state.discharge_temp);
    create_temp_row(content, i18n_get(STR_HP_SUCTION), &state.suction_temp);
    
    // Coil temps
    create_temp_row(content, i18n_get(STR_HP_OUTDOOR_COIL), &state.outdoor_coil_temp);
    create_temp_row(content, i18n_get(STR_HP_INDOOR_COIL), &state.indoor_coil_temp);
    
    // Module temp
    create_temp_row(content, i18n_get(STR_HP_IPM_MODULE), &state.ipm_temp);

    // ===== System section =====
    create_section_header(content, i18n_get(STR_HP_SYSTEM_SECTION));
    create_reading_row(content, i18n_get(STR_HP_FREQUENCY), &state.compressor_freq);
    create_reading_row(content, i18n_get(STR_HP_FAN_SPEED), &state.fan_speed);
    create_reading_row(content, i18n_get(STR_HP_AC_VOLTAGE), &state.ac_voltage);
    create_reading_row(content, i18n_get(STR_HP_AC_CURRENT), &state.ac_current);
    create_reading_row(content, i18n_get(STR_HP_DC_VOLTAGE), &state.dc_voltage);
    create_reading_row(content, i18n_get(STR_HP_DC_CURRENT), &state.dc_current);
    create_reading_row(content, i18n_get(STR_HP_PRIMARY_EEV), &state.primary_eev);
    create_reading_row(content, i18n_get(STR_HP_SECONDARY_EEV), &state.secondary_eev);
    create_reading_row(content, i18n_get(STR_HP_COOLING), &state.cooling_setpoint);
    create_reading_row(content, i18n_get(STR_HP_HEATING), &state.heating_setpoint);
    create_reading_row(content, i18n_get(STR_HP_HOT_WATER), &state.hotwater_setpoint);
    
    state.shown = true;
    
    // Update timer
    state.update_timer = lv_timer_create(update_timer_cb, 1000, nullptr);
    
    // Initial update
    update_readings();
}

void heatpump_temps_set_active(bool active) {
    if (state.update_timer) {
        if (active) {
            lv_timer_resume(state.update_timer);
        } else {
            lv_timer_pause(state.update_timer);
        }
    }
    if (active) {
        update_readings();
    }
}

void heatpump_temps_hide(void) {
    if (!state.shown) {
        return;
    }
    
    ESP_LOGI(TAG, "Hiding temperatures screen");
    
    if (state.update_timer) {
        lv_timer_del(state.update_timer);
        state.update_timer = nullptr;
    }
    
    heatpump_temps_close_cb_t cb = state.on_close;
    
    state.shown = false;
    state.on_close = nullptr;
    state.screen = nullptr;
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
    state.primary_eev = nullptr;
    state.secondary_eev = nullptr;
    state.cooling_setpoint = nullptr;
    state.heating_setpoint = nullptr;
    state.hotwater_setpoint = nullptr;
    
    // Call callback to restore previous screen (animation will delete this screen)
    if (cb) {
        cb();
    }
}

bool heatpump_temps_is_shown(void) {
    return state.shown;
}
