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
#include "ui_common.h"
#include <esp_log.h>
#include <stdio.h>

static const char* TAG = "hp_screen";

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
    
    // Error indicator
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
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
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
    // Load saved screen back
    if (state.saved_screen) {
        lv_scr_load(state.saved_screen);
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
    // Load saved screen back
    if (state.saved_screen) {
        lv_scr_load(state.saved_screen);
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
    // Load saved screen back
    if (state.saved_screen) {
        lv_scr_load(state.saved_screen);
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
    lv_obj_set_style_text_font(state.mode_label, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.mode_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(state.mode_label, LV_ALIGN_TOP_MID, 0, 5);
    
    // Tank temperature (large, center)
    state.tank_temp_label = lv_label_create(state.status_card);
    lv_label_set_text(state.tank_temp_label, "-- °C");
    lv_obj_set_style_text_font(state.tank_temp_label, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.tank_temp_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(state.tank_temp_label, LV_ALIGN_CENTER, 0, 0);
    
    // "Tank Temperature" label
    lv_obj_t* tank_desc = lv_label_create(state.status_card);
    lv_label_set_text(tank_desc, "Tank Temperature");
    lv_obj_set_style_text_font(tank_desc, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(tank_desc, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(tank_desc, LV_ALIGN_CENTER, 0, 40);
    
    // Setpoint (bottom right)
    state.setpoint_label = lv_label_create(state.status_card);
    lv_label_set_text(state.setpoint_label, "Set: -- °C");
    lv_obj_set_style_text_font(state.setpoint_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.setpoint_label, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(state.setpoint_label, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    
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
    // BOTTOM: Error/Status Row
    // =========================================================================
    state.error_label = lv_label_create(state.container);
    lv_label_set_text(state.error_label, "");
    lv_obj_set_style_text_font(state.error_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.error_label, COLOR_ERROR, LV_PART_MAIN);
    lv_obj_align(state.error_label, LV_ALIGN_TOP_MID, 0, 320);
    lv_obj_set_width(state.error_label, LV_PCT(100));
    lv_obj_set_style_text_align(state.error_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    
    // =========================================================================
    // BOTTOM: Three button bar - Controls | Temps | System
    // =========================================================================
    
    // Controls button (left)
    state.controls_btn = lv_btn_create(state.container);
    lv_obj_set_size(state.controls_btn, 200, 60);
    lv_obj_align(state.controls_btn, LV_ALIGN_BOTTOM_LEFT, 10, 0);
    lv_obj_set_style_bg_color(state.controls_btn, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(state.controls_btn, lv_color_hex(0x2a3a5e), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(state.controls_btn, COLOR_SUCCESS, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.controls_btn, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(state.controls_btn, 12, LV_PART_MAIN);
    lv_obj_add_event_cb(state.controls_btn, controls_btn_cb, LV_EVENT_CLICKED, nullptr);
    
    lv_obj_t* ctrl_label = lv_label_create(state.controls_btn);
    lv_label_set_text(ctrl_label, LV_SYMBOL_SETTINGS " Controls");
    lv_obj_set_style_text_font(ctrl_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(ctrl_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_center(ctrl_label);
    
    // Temperatures button (center)
    state.temps_btn = lv_btn_create(state.container);
    lv_obj_set_size(state.temps_btn, 200, 60);
    lv_obj_align(state.temps_btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(state.temps_btn, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(state.temps_btn, lv_color_hex(0x2a3a5e), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(state.temps_btn, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.temps_btn, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(state.temps_btn, 12, LV_PART_MAIN);
    lv_obj_add_event_cb(state.temps_btn, temps_btn_cb, LV_EVENT_CLICKED, nullptr);
    
    lv_obj_t* temps_label = lv_label_create(state.temps_btn);
    lv_label_set_text(temps_label, "Temps " LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(temps_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(temps_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_center(temps_label);
    
    // System readings button (right)
    state.system_btn = lv_btn_create(state.container);
    lv_obj_set_size(state.system_btn, 200, 60);
    lv_obj_align(state.system_btn, LV_ALIGN_BOTTOM_RIGHT, -10, 0);
    lv_obj_set_style_bg_color(state.system_btn, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(state.system_btn, lv_color_hex(0x2a3a5e), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(state.system_btn, COLOR_WARNING, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.system_btn, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(state.system_btn, 12, LV_PART_MAIN);
    lv_obj_add_event_cb(state.system_btn, system_btn_cb, LV_EVENT_CLICKED, nullptr);
    
    lv_obj_t* system_label = lv_label_create(state.system_btn);
    lv_label_set_text(system_label, "System " LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(system_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(system_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_center(system_label);

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
    
    if (!hp.connected) {
        // Disconnected state
        lv_label_set_text(state.mode_label, "DISCONNECTED");
        lv_obj_set_style_text_color(state.mode_label, COLOR_TEXT_DIM, LV_PART_MAIN);
        lv_label_set_text(state.tank_temp_label, "-- °C");
        lv_label_set_text(state.setpoint_label, "Set: -- °C");
        
        // All indicators off
        set_indicator_active(state.compressor_indicator, false, COLOR_SUCCESS);
        set_indicator_active(state.fan_indicator, false, COLOR_SUCCESS);
        set_indicator_active(state.pump_indicator, false, COLOR_SUCCESS);
        set_indicator_active(state.heater_indicator, false, COLOR_WARNING);
        
        // Update fan label
        lv_label_set_text(state.fan_label, "Fan");
        
        lv_label_set_text(state.error_label, "Waiting for heat pump connection...");
        lv_obj_set_style_text_color(state.error_label, COLOR_TEXT_DIM, LV_PART_MAIN);
        return;
    }
    
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
    char temp_buf[16];
    snprintf(temp_buf, sizeof(temp_buf), "%d °C", hp.water_tank_temp);
    lv_label_set_text(state.tank_temp_label, temp_buf);
    
    // Setpoint (choose based on mode)
    int16_t setpoint = 0;
    switch (hp.working_mode) {
        case arctic::WorkingMode::COOLING:
            setpoint = hp.cooling_setpoint;
            break;
        case arctic::WorkingMode::FLOOR_HEATING:
        case arctic::WorkingMode::FAN_COIL_HEATING:
            setpoint = hp.heating_setpoint;
            break;
        case arctic::WorkingMode::HOT_WATER:
            setpoint = hp.hot_water_setpoint;
            break;
        default:
            setpoint = hp.heating_setpoint;  // Default to heating
            break;
    }
    snprintf(temp_buf, sizeof(temp_buf), "Set: %d °C", setpoint);
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
    
    // Error display
    if (hp.hasAnyError()) {
        char error_buf[256];
        arctic::getErrorDescriptions(error_buf, sizeof(error_buf));
        lv_label_set_text(state.error_label, error_buf);
        lv_obj_set_style_text_color(state.error_label, COLOR_ERROR, LV_PART_MAIN);
    } else {
        lv_label_set_text(state.error_label, "");
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
