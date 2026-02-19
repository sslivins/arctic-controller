/*
 * Arctic Heat Pump Controller
 * Heat Pump Temperatures Screen Implementation
 * 
 * Full-screen display of all temperature sensors with large, readable fonts.
 */

#include "heatpump_temps_screen.h"
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

// Helper to color-code temperature values
static lv_color_t get_temp_color(int16_t temp) {
    if (temp >= 50) return COLOR_WARM;
    if (temp <= 5) return COLOR_COLD;
    return COLOR_TEXT;
}

static void update_readings() {
    if (!state.shown) return;
    
    arctic::HeatPumpState hp = arctic::getState();
    bool demo_mode = app_prefs_is_demo_mode();
    char buf[16];
    
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

void heatpump_temps_show(heatpump_temps_close_cb_t on_close) {
    if (state.shown) {
        return;
    }
    
    ESP_LOGI(TAG, "Showing temperatures screen");
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
    lv_obj_set_user_data(back_btn, (void*)"temps_close");
    
    lv_obj_t* back_icon = lv_label_create(back_btn);
    lv_label_set_text(back_icon, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(back_icon, &montserrat_32_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(back_icon, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_center(back_icon);
    
    // Title
    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, i18n_get(STR_HP_TEMPERATURES));
    lv_obj_set_style_text_color(title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, UI_FONT_HEADER, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_user_data(title, (void*)"temps_title");
    
    // Scrollable content (remaining 92%)
    lv_obj_t* content = lv_obj_create(state.screen);
    lv_obj_set_size(content, LV_PCT(100), LV_PCT(92));
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(content, 20, LV_PART_MAIN);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 12, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
    
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
    
    state.shown = true;
    
    // Update timer
    state.update_timer = lv_timer_create(update_timer_cb, 1000, nullptr);
    
    // Initial update
    update_readings();
    
    // Load with slide animation (main screen moves up)
    lv_screen_load_anim(state.screen, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, false);
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
    
    // Call callback to restore previous screen (animation will delete this screen)
    if (cb) {
        cb();
    }
}

bool heatpump_temps_is_shown(void) {
    return state.shown;
}
