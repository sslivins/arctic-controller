/*
 * Arctic Heat Pump Controller
 * Heat Pump System Readings Screen Implementation
 * 
 * Full-screen display of system readings (pressures, voltages, currents, etc.)
 */

#include "heatpump_system_screen.h"
#include "modbus/arctic_heatpump.h"
#include "ui_common.h"
#include "fonts/fonts.h"
#include "app_preferences.h"
#include "i18n/i18n.h"
#include <esp_log.h>
#include <stdio.h>

static const char* TAG = "hp_system";

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

// ============================================================================
// State
// ============================================================================
static struct {
    bool shown = false;
    lv_obj_t* screen = nullptr;
    heatpump_system_close_cb_t on_close = nullptr;
    lv_timer_t* update_timer = nullptr;
    
    // Compressor readings
    lv_obj_t* compressor_freq = nullptr;
    lv_obj_t* fan_speed = nullptr;
    
    // Electrical readings
    lv_obj_t* ac_voltage = nullptr;
    lv_obj_t* ac_current = nullptr;
    lv_obj_t* dc_voltage = nullptr;
    lv_obj_t* dc_current = nullptr;
    
    // Pressure readings
    lv_obj_t* high_pressure = nullptr;
    lv_obj_t* low_pressure = nullptr;
    
    // Expansion valve readings
    lv_obj_t* primary_eev = nullptr;
    lv_obj_t* secondary_eev = nullptr;
    
    // Setpoints
    lv_obj_t* cooling_setpoint = nullptr;
    lv_obj_t* heating_setpoint = nullptr;
    lv_obj_t* hotwater_setpoint = nullptr;
} state;

// ============================================================================
// Helper Functions
// ============================================================================

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
    lv_obj_set_style_text_font(lbl, &montserrat_24_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    
    return header;
}

static lv_obj_t* create_reading_row(lv_obj_t* parent, const char* label, lv_obj_t** value_label_out) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 65);
    lv_obj_set_style_bg_color(row, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 25, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    
    // Label on left
    lv_obj_t* name_lbl = lv_label_create(row);
    lv_label_set_text(name_lbl, label);
    lv_obj_set_style_text_font(name_lbl, &montserrat_24_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(name_lbl, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    
    // Value on right
    lv_obj_t* val_lbl = lv_label_create(row);
    lv_label_set_text(val_lbl, "--");
    lv_obj_set_style_text_font(val_lbl, &montserrat_32_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(val_lbl, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(val_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
    
    *value_label_out = val_lbl;
    return row;
}

static void update_readings() {
    if (!state.shown) return;
    
    arctic::HeatPumpState hp = arctic::getState();
    bool demo_mode = app_prefs_is_demo_mode();
    char buf[32];
    
    if (demo_mode) {
        // Demo mode - show simulated values
        lv_label_set_text(state.compressor_freq, "60 Hz");
        lv_obj_set_style_text_color(state.compressor_freq, COLOR_SUCCESS, LV_PART_MAIN);
        
        lv_label_set_text(state.fan_speed, "850 RPM");
        lv_obj_set_style_text_color(state.fan_speed, COLOR_SUCCESS, LV_PART_MAIN);
        
        lv_label_set_text(state.ac_voltage, "230 V");
        lv_label_set_text(state.ac_current, "5 A");
        lv_label_set_text(state.dc_voltage, "380.0 V");
        lv_label_set_text(state.dc_current, "4 A");
        
        lv_label_set_text(state.high_pressure, "2.50 MPa");
        lv_label_set_text(state.low_pressure, "0.85 MPa");
        
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
    
    // Real mode - update from actual values
    snprintf(buf, sizeof(buf), "%d Hz", hp.compressor_freq);
    lv_label_set_text(state.compressor_freq, buf);
    lv_obj_set_style_text_color(state.compressor_freq, 
        hp.compressor_freq > 0 ? COLOR_SUCCESS : COLOR_TEXT_DIM, LV_PART_MAIN);
    
    snprintf(buf, sizeof(buf), "%d RPM", hp.fan_speed);
    lv_label_set_text(state.fan_speed, buf);
    lv_obj_set_style_text_color(state.fan_speed,
        hp.fan_speed > 0 ? COLOR_SUCCESS : COLOR_TEXT_DIM, LV_PART_MAIN);
    
    // Electrical
    snprintf(buf, sizeof(buf), "%d V", hp.ac_voltage);
    lv_label_set_text(state.ac_voltage, buf);
    
    snprintf(buf, sizeof(buf), "%d A", hp.ac_current);
    lv_label_set_text(state.ac_current, buf);
    
    // dc_voltage is in tenths of volts
    snprintf(buf, sizeof(buf), "%d.%d V", hp.dc_voltage / 10, hp.dc_voltage % 10);
    lv_label_set_text(state.dc_voltage, buf);
    
    snprintf(buf, sizeof(buf), "%d A", hp.dc_current);
    lv_label_set_text(state.dc_current, buf);
    
    // Pressures
    // high_pressure is in hundredths of MPa
    snprintf(buf, sizeof(buf), "%d.%02d MPa", hp.high_pressure / 100, hp.high_pressure % 100);
    lv_label_set_text(state.high_pressure, buf);
    
    // low_pressure is in hundredths of MPa
    snprintf(buf, sizeof(buf), "%d.%02d MPa", hp.low_pressure / 100, hp.low_pressure % 100);
    lv_label_set_text(state.low_pressure, buf);
    
    // EEV
    snprintf(buf, sizeof(buf), "%d steps", hp.primary_eev_opening);
    lv_label_set_text(state.primary_eev, buf);
    
    snprintf(buf, sizeof(buf), "%d steps", hp.secondary_eev_opening);
    lv_label_set_text(state.secondary_eev, buf);
    
    // Setpoints with unit conversion
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
    heatpump_system_close_cb_t cb = state.on_close;
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

void heatpump_system_show(heatpump_system_close_cb_t on_close) {
    if (state.shown) {
        return;
    }
    
    ESP_LOGI(TAG, "Showing system readings screen");
    state.on_close = on_close;
    
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
    
    // Close button (X on right) with circular background
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
    lv_obj_set_user_data(back_btn, (void*)"system_close");
    
    lv_obj_t* back_icon = lv_label_create(back_btn);
    lv_label_set_text(back_icon, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(back_icon, &montserrat_32_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(back_icon, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_center(back_icon);
    
    // Title
    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, i18n_get(STR_HP_SYSTEM_READINGS));
    lv_obj_set_style_text_color(title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, UI_FONT_HEADER, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_user_data(title, (void*)"system_title");
    
    // Scrollable content
    lv_obj_t* content = lv_obj_create(state.screen);
    lv_obj_set_size(content, LV_PCT(100), LV_PCT(92));
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(content, 20, LV_PART_MAIN);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 10, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
    
    // Compressor section
    create_section_header(content, i18n_get(STR_HP_COMPRESSOR));
    create_reading_row(content, i18n_get(STR_HP_FREQUENCY), &state.compressor_freq);
    create_reading_row(content, i18n_get(STR_HP_FAN_SPEED), &state.fan_speed);
    
    // Electrical section
    create_section_header(content, i18n_get(STR_HP_ELECTRICAL));
    create_reading_row(content, i18n_get(STR_HP_AC_VOLTAGE), &state.ac_voltage);
    create_reading_row(content, i18n_get(STR_HP_AC_CURRENT), &state.ac_current);
    create_reading_row(content, i18n_get(STR_HP_DC_VOLTAGE), &state.dc_voltage);
    create_reading_row(content, i18n_get(STR_HP_DC_CURRENT), &state.dc_current);
    
    // Pressure section
    create_section_header(content, i18n_get(STR_HP_PRESSURES));
    create_reading_row(content, i18n_get(STR_HP_HIGH_PRESSURE), &state.high_pressure);
    create_reading_row(content, i18n_get(STR_HP_LOW_PRESSURE), &state.low_pressure);
    
    // EEV section
    create_section_header(content, i18n_get(STR_HP_EXPANSION_VALVES));
    create_reading_row(content, i18n_get(STR_HP_PRIMARY_EEV), &state.primary_eev);
    create_reading_row(content, i18n_get(STR_HP_SECONDARY_EEV), &state.secondary_eev);
    
    // Setpoints section
    create_section_header(content, i18n_get(STR_HP_SETPOINTS));
    create_reading_row(content, i18n_get(STR_HP_COOLING), &state.cooling_setpoint);
    create_reading_row(content, i18n_get(STR_HP_HEATING), &state.heating_setpoint);
    create_reading_row(content, i18n_get(STR_HP_HOT_WATER), &state.hotwater_setpoint);
    
    state.shown = true;
    
    // Update timer
    state.update_timer = lv_timer_create(update_timer_cb, 1000, nullptr);
    
    // Initial update
    update_readings();
    
    // Load with slide animation (main screen moves up)
    lv_screen_load_anim(state.screen, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, false);
}

void heatpump_system_hide(void) {
    if (!state.shown) {
        return;
    }
    
    ESP_LOGI(TAG, "Hiding system readings screen");
    
    if (state.update_timer) {
        lv_timer_del(state.update_timer);
        state.update_timer = nullptr;
    }
    
    heatpump_system_close_cb_t cb = state.on_close;
    
    state.shown = false;
    state.on_close = nullptr;
    state.screen = nullptr;
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
    
    // Call callback to restore previous screen (animation will delete this screen)
    if (cb) {
        cb();
    }
}

bool heatpump_system_is_shown(void) {
    return state.shown;
}
