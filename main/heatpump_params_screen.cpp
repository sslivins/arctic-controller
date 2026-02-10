/*
 * Arctic Heat Pump Controller
 * Heat Pump Parameters Screen - Technician P-Parameters
 * 
 * Display-only rows with click-to-edit dialogs.
 * Portrait mode: 720x1280
 */

#include "heatpump_control_screen.h"
#include "modbus/arctic_heatpump.h"
#include "modbus/arctic_registers.h"
#include "heatpump_params.h"
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

// ============================================================================
// Helper to get unit string for display (uses shared ParamUnit enum)
// ============================================================================
static const char* get_param_unit_str(ParamUnit unit_type) {
    switch (unit_type) {
        case ParamUnit::STEPS:         return "steps";
        case ParamUnit::MINUTES:       return "min";
        case ParamUnit::SECONDS:       return "sec";
        case ParamUnit::TEMP_ABSOLUTE: return app_prefs_temp_unit_str();
        case ParamUnit::TEMP_OFFSET:   return app_prefs_temp_diff_unit_str();
        default:                       return "";
    }
}

// Helper to format display name with P-code (e.g., "EEV Opening (P1)")
static void format_param_display_name(char* buf, size_t buf_size, const HeatPumpParam* param) {
    snprintf(buf, buf_size, "%s (%s)", i18n_get(param->name_id), param->p_code);
}

// Map category string from heatpump_params.h to i18n string ID
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
    heatpump_control_close_cb_t on_close = nullptr;
    
    // Setpoint rows (Basic Settings section)
    lv_obj_t* cooling_value_label = nullptr;
    lv_obj_t* heating_value_label = nullptr;
    lv_obj_t* hotwater_value_label = nullptr;
    
    // Array of value labels for each parameter row
    lv_obj_t* value_labels[32] = {};
    
    // Staggered loading
    lv_timer_t* load_timer = nullptr;
    int load_index = 0;
    
    // Edit dialog
    lv_obj_t* edit_dialog = nullptr;
    lv_obj_t* edit_title = nullptr;
    lv_obj_t* edit_description = nullptr;
    lv_obj_t* edit_range_label = nullptr;
    lv_obj_t* edit_value_label = nullptr;
    lv_obj_t* edit_minus_btn = nullptr;
    lv_obj_t* edit_plus_btn = nullptr;
    int current_param_idx = -1;
    float edit_value = 0;       // Float for F mode precision
    int16_t edit_value_celsius = 0;  // Original value in Celsius (for non-temp params too)
    
    // Setpoint editing (uses same dialog, different handling)
    // -1 = editing P-parameter, 0/1/2 = editing cooling/heating/hotwater
    int current_setpoint_type = -1;
} state;

// Setpoint definitions are initialized lazily to pick up i18n language
struct SetpointDef {
    string_id_t name_id;
    const char* description;  // Technical - not translated
    int16_t min_val;    // In Celsius
    int16_t max_val;    // In Celsius
};

static const SetpointDef s_setpoints[] = {
    {STR_HP_COOLING_SETPOINT, "Target water temperature for cooling mode.", 5, 30},
    {STR_HP_HEATING_SETPOINT, "Target water temperature for floor/fan heating mode.", 20, 60},
    {STR_HP_HOT_WATER_SETPOINT, "Target temperature for hot water tank.", 30, 60},
};

// ============================================================================
// Forward Declarations
// ============================================================================
static void close_btn_cb(lv_event_t* e);
static void param_row_cb(lv_event_t* e);
static void edit_cancel_cb(lv_event_t* e);
static void edit_save_cb(lv_event_t* e);
static void edit_minus_cb(lv_event_t* e);
static void edit_plus_cb(lv_event_t* e);
static void show_edit_dialog(int param_idx);
static void hide_edit_dialog(void);
static void update_edit_value_display(void);
static void load_timer_cb(lv_timer_t* timer);
static void setpoint_row_cb(lv_event_t* e);
static void show_setpoint_edit(int setpoint_type);  // 0=cooling, 1=heating, 2=hotwater

// ============================================================================
// Demo Setpoints (screen-local; P-parameters use shared heatpump_params.cpp)
// ============================================================================
static int16_t s_demo_setpoints[3] = {-1, -1, -1};  // Demo setpoints: cooling, heating, hot water (-1 = not set)
static bool s_demo_setpoints_initialized = false;

static void init_demo_setpoints(void) {
    if (s_demo_setpoints_initialized) return;
    
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
// Helper Functions
// ============================================================================

// Wrapper for UI that shows error dialog on write failure
static bool write_param_value_with_ui(int param_idx, int16_t value) {
    if (!heatpump_param_write_by_index(param_idx, value)) {
        show_settings_write_error(i18n_get(STR_HP_CANNOT_SAVE));
        return false;
    }
    return true;
}

static void update_param_display(int param_idx, int16_t value_celsius) {
    if (param_idx < 0 || param_idx >= NUM_HEATPUMP_PARAMS || param_idx >= 32) return;
    if (!state.value_labels[param_idx]) return;
    
    const HeatPumpParam& param = HEATPUMP_PARAMS[param_idx];
    const char* unit_str = get_param_unit_str(param.unit_type);
    char buf[32];
    
    // Convert temperature values based on unit type
    int display_value;
    if (param.unit_type == ParamUnit::TEMP_ABSOLUTE) {
        display_value = app_prefs_convert_temp(value_celsius);
    } else if (param.unit_type == ParamUnit::TEMP_OFFSET) {
        display_value = app_prefs_convert_temp_diff(value_celsius);
    } else {
        display_value = value_celsius;  // Non-temp values, no conversion
    }
    
    if (unit_str[0]) {
        snprintf(buf, sizeof(buf), "%d %s", display_value, unit_str);
    } else {
        snprintf(buf, sizeof(buf), "%d", display_value);
    }
    lv_label_set_text(state.value_labels[param_idx], buf);
}

static void load_timer_cb(lv_timer_t* timer) {
    (void)timer;
    
    if (!state.shown || state.load_index >= NUM_HEATPUMP_PARAMS) {
        // Done loading - delete timer
        if (state.load_timer) {
            lv_timer_del(state.load_timer);
            state.load_timer = nullptr;
        }
        return;
    }
    
    // Don't load if edit dialog is open
    if (state.current_param_idx >= 0) return;
    
    // Read and display one parameter
    int16_t val = heatpump_param_read_by_index(state.load_index);
    update_param_display(state.load_index, val);
    
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
    lv_obj_set_style_text_font(lbl, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    
    return header;
}

static lv_obj_t* create_param_row(lv_obj_t* parent, int param_idx) {
    const HeatPumpParam& param = HEATPUMP_PARAMS[param_idx];
    
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
    
    // Store param index in user data
    lv_obj_set_user_data(row, (void*)(intptr_t)param_idx);
    lv_obj_add_event_cb(row, param_row_cb, LV_EVENT_CLICKED, nullptr);
    
    // Parameter name with P-code (left)
    char display_name[64];
    format_param_display_name(display_name, sizeof(display_name), &param);
    lv_obj_t* name_lbl = lv_label_create(row);
    lv_label_set_text(name_lbl, display_name);
    lv_obj_set_style_text_font(name_lbl, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(name_lbl, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_width(name_lbl, 400);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
    
    // Value (right)
    lv_obj_t* val_lbl = lv_label_create(row);
    lv_label_set_text(val_lbl, "---");
    lv_obj_set_style_text_font(val_lbl, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(val_lbl, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(val_lbl, LV_ALIGN_RIGHT_MID, -30, 0);
    
    // Arrow indicator
    lv_obj_t* arrow = lv_label_create(row);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(arrow, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(arrow, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, 0, 0);
    
    // Store value label reference
    if (param_idx < 32) {
        state.value_labels[param_idx] = val_lbl;
    }
    
    return row;
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
    lv_obj_set_style_text_font(name_lbl, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(name_lbl, row_color, LV_PART_MAIN);
    lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_width(name_lbl, 400);
    
    // Value (right)
    lv_obj_t* val_lbl = lv_label_create(row);
    lv_label_set_text(val_lbl, "---");
    lv_obj_set_style_text_font(val_lbl, UI_FONT_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(val_lbl, row_color, LV_PART_MAIN);
    lv_obj_align(val_lbl, LV_ALIGN_RIGHT_MID, -30, 0);
    
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
    
    // Header bar with X and checkmark
    lv_obj_t* header = lv_obj_create(state.edit_dialog);
    lv_obj_set_size(header, LV_PCT(100), 100);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(header, 20, LV_PART_MAIN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    
    // Cancel button (X) with circular background
    lv_obj_t* cancel_btn = lv_btn_create(header);
    lv_obj_set_size(cancel_btn, 60, 60);
    lv_obj_align(cancel_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x3d4f6f), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(cancel_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(cancel_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(cancel_btn, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(cancel_btn, COLOR_ERROR, LV_PART_MAIN);
    lv_obj_set_style_border_opa(cancel_btn, LV_OPA_50, LV_PART_MAIN);
    lv_obj_add_event_cb(cancel_btn, edit_cancel_cb, LV_EVENT_CLICKED, nullptr);
    
    lv_obj_t* cancel_icon = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_icon, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(cancel_icon, UI_FONT_ICON, LV_PART_MAIN);
    lv_obj_set_style_text_color(cancel_icon, COLOR_ERROR, LV_PART_MAIN);
    lv_obj_center(cancel_icon);
    
    // Title in center
    state.edit_title = lv_label_create(header);
    lv_label_set_text(state.edit_title, i18n_get(STR_HP_EDIT_PARAMETER));
    lv_obj_set_style_text_font(state.edit_title, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.edit_title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(state.edit_title, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_width(state.edit_title, 500);
    lv_obj_set_style_text_align(state.edit_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(state.edit_title, LV_LABEL_LONG_DOT);
    
    // Save button (checkmark) with circular background
    lv_obj_t* save_btn = lv_btn_create(header);
    lv_obj_set_size(save_btn, 60, 60);
    lv_obj_align(save_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(0x3d4f6f), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(save_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(save_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(save_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(save_btn, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(save_btn, COLOR_SUCCESS, LV_PART_MAIN);
    lv_obj_set_style_border_opa(save_btn, LV_OPA_50, LV_PART_MAIN);
    lv_obj_add_event_cb(save_btn, edit_save_cb, LV_EVENT_CLICKED, nullptr);
    
    lv_obj_t* save_icon = lv_label_create(save_btn);
    lv_label_set_text(save_icon, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(save_icon, UI_FONT_ICON, LV_PART_MAIN);
    lv_obj_set_style_text_color(save_icon, COLOR_SUCCESS, LV_PART_MAIN);
    lv_obj_center(save_icon);
    
    // Content area
    lv_obj_t* content = lv_obj_create(state.edit_dialog);
    lv_obj_set_size(content, LV_PCT(90), 500);
    lv_obj_align(content, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(content, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(content, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(content, 30, LV_PART_MAIN);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    
    // Description text
    state.edit_description = lv_label_create(content);
    lv_label_set_text(state.edit_description, "Parameter description...");
    lv_obj_set_style_text_font(state.edit_description, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.edit_description, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_width(state.edit_description, LV_PCT(100));
    lv_label_set_long_mode(state.edit_description, LV_LABEL_LONG_WRAP);
    lv_obj_align(state.edit_description, LV_ALIGN_TOP_MID, 0, 0);
    
    // Range label
    state.edit_range_label = lv_label_create(content);
    lv_label_set_text(state.edit_range_label, "Range: 0 - 100");
    lv_obj_set_style_text_font(state.edit_range_label, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.edit_range_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(state.edit_range_label, LV_ALIGN_TOP_MID, 0, 120);
    
    // Value display with +/- buttons
    lv_obj_t* value_row = lv_obj_create(content);
    lv_obj_set_size(value_row, 400, 120);
    lv_obj_align(value_row, LV_ALIGN_CENTER, 0, 50);
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
}

// ============================================================================
// Event Handlers
// ============================================================================

static void close_btn_cb(lv_event_t* e) {
    (void)e;
    ESP_LOGI(TAG, "Close button clicked");
    
    // Stop load timer first to prevent use-after-free
    if (state.load_timer) {
        lv_timer_del(state.load_timer);
        state.load_timer = nullptr;
    }
    
    // Save and clear callback
    heatpump_control_close_cb_t cb = state.on_close;
    state.on_close = nullptr;
    state.shown = false;
    state.screen = nullptr;
    
    // Call callback - it will load the previous screen with auto_del=true
    if (cb) {
        cb();
    }
}

static void param_row_cb(lv_event_t* e) {
    lv_obj_t* row = (lv_obj_t*)lv_event_get_target(e);
    int param_idx = (int)(intptr_t)lv_obj_get_user_data(row);
    show_edit_dialog(param_idx);
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
    
    if (state.current_param_idx >= 0 && state.current_param_idx < NUM_HEATPUMP_PARAMS) {
        const HeatPumpParam& param = HEATPUMP_PARAMS[state.current_param_idx];
        
        // Convert back to Celsius for temp parameters
        int16_t save_val;
        if (param.unit_type == ParamUnit::TEMP_ABSOLUTE) {
            save_val = app_prefs_temp_to_celsius_from_f(state.edit_value);
        } else if (param.unit_type == ParamUnit::TEMP_OFFSET) {
            save_val = app_prefs_temp_diff_to_celsius_from_f(state.edit_value);
        } else {
            save_val = (int16_t)roundf(state.edit_value);
        }
        
        if (write_param_value_with_ui(state.current_param_idx, save_val)) {
            ESP_LOGI(TAG, "Saved %s = %d (from %.0f display units)", 
                     param.name, save_val, state.edit_value);
            
            // Update the displayed value in the list
            if (state.current_param_idx < 32 && state.value_labels[state.current_param_idx]) {
                char buf[32];
                int display_val = (int)roundf(state.edit_value);
                const char* unit_str = get_param_unit_str(param.unit_type);
                if (unit_str[0]) {
                    snprintf(buf, sizeof(buf), "%d %s", display_val, unit_str);
                } else {
                    snprintf(buf, sizeof(buf), "%d", display_val);
                }
                lv_label_set_text(state.value_labels[state.current_param_idx], buf);
            }
        } else {
            ESP_LOGE(TAG, "Failed to save %s", param.name);
        }
    }
    
    hide_edit_dialog();
}

static void edit_minus_cb(lv_event_t* e) {
    (void)e;
    
    float min_val = 0;
    
    if (state.current_setpoint_type >= 0 && state.current_setpoint_type <= 2) {
        // Setpoints are always TEMP_ABSOLUTE - convert limit to display units
        min_val = app_prefs_convert_temp_f(s_setpoints[state.current_setpoint_type].min_val);
    } else if (state.current_param_idx >= 0 && state.current_param_idx < NUM_HEATPUMP_PARAMS) {
        const HeatPumpParam& param = HEATPUMP_PARAMS[state.current_param_idx];
        if (param.unit_type == ParamUnit::TEMP_ABSOLUTE) {
            min_val = app_prefs_convert_temp_f(param.min_val);
        } else if (param.unit_type == ParamUnit::TEMP_OFFSET) {
            min_val = app_prefs_convert_temp_diff_f(param.min_val);
        } else {
            min_val = (float)param.min_val;
        }
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
    
    float max_val = 0;
    
    if (state.current_setpoint_type >= 0 && state.current_setpoint_type <= 2) {
        // Setpoints are always TEMP_ABSOLUTE - convert limit to display units
        max_val = app_prefs_convert_temp_f(s_setpoints[state.current_setpoint_type].max_val);
    } else if (state.current_param_idx >= 0 && state.current_param_idx < NUM_HEATPUMP_PARAMS) {
        const HeatPumpParam& param = HEATPUMP_PARAMS[state.current_param_idx];
        if (param.unit_type == ParamUnit::TEMP_ABSOLUTE) {
            max_val = app_prefs_convert_temp_f(param.max_val);
        } else if (param.unit_type == ParamUnit::TEMP_OFFSET) {
            max_val = app_prefs_convert_temp_diff_f(param.max_val);
        } else {
            max_val = (float)param.max_val;
        }
    } else {
        return;
    }
    
    if (state.edit_value < max_val) {
        state.edit_value += 1.0f;
        update_edit_value_display();
    }
}

static void show_edit_dialog(int param_idx) {
    if (param_idx < 0 || param_idx >= NUM_HEATPUMP_PARAMS) return;
    
    state.current_param_idx = param_idx;
    state.current_setpoint_type = -1;  // Not editing a setpoint
    const HeatPumpParam& param = HEATPUMP_PARAMS[param_idx];
    
    // Read current value in Celsius
    state.edit_value_celsius = heatpump_param_read_by_index(param_idx);
    
    // Convert to display units (float for F mode precision)
    if (param.unit_type == ParamUnit::TEMP_ABSOLUTE) {
        state.edit_value = app_prefs_convert_temp_f(state.edit_value_celsius);
    } else if (param.unit_type == ParamUnit::TEMP_OFFSET) {
        state.edit_value = app_prefs_convert_temp_diff_f(state.edit_value_celsius);
    } else {
        state.edit_value = (float)state.edit_value_celsius;
    }
    
    // Update dialog content - format name with P-code
    char title_buf[64];
    format_param_display_name(title_buf, sizeof(title_buf), &param);
    lv_label_set_text(state.edit_title, title_buf);
    lv_label_set_text(state.edit_description, param.description);
    
    // Range label - convert limits for temp params
    char range_buf[64];
    const char* unit_str = get_param_unit_str(param.unit_type);
    int display_min, display_max;
    
    if (param.unit_type == ParamUnit::TEMP_ABSOLUTE) {
        display_min = app_prefs_convert_temp(param.min_val);
        display_max = app_prefs_convert_temp(param.max_val);
    } else if (param.unit_type == ParamUnit::TEMP_OFFSET) {
        display_min = app_prefs_convert_temp_diff(param.min_val);
        display_max = app_prefs_convert_temp_diff(param.max_val);
    } else {
        display_min = param.min_val;
        display_max = param.max_val;
    }
    
    if (unit_str[0]) {
        snprintf(range_buf, sizeof(range_buf), "%s %d - %d %s", 
                 i18n_get(STR_HP_RANGE_FMT), display_min, display_max, unit_str);
    } else {
        snprintf(range_buf, sizeof(range_buf), "%s %d - %d", 
                 i18n_get(STR_HP_RANGE_FMT), display_min, display_max);
    }
    lv_label_set_text(state.edit_range_label, range_buf);
    
    update_edit_value_display();
    
    // Show dialog with animation
    lv_obj_remove_flag(state.edit_dialog, LV_OBJ_FLAG_HIDDEN);
}

static void show_setpoint_edit(int setpoint_type) {
    if (setpoint_type < 0 || setpoint_type > 2) return;
    
    state.current_param_idx = -1;  // Not editing a P-parameter
    state.current_setpoint_type = setpoint_type;
    const SetpointDef& sp = s_setpoints[setpoint_type];
    
    // Read current value in Celsius
    int16_t celsius_val;
    arctic::HeatPumpState hp = arctic::getState();
    if (app_prefs_is_demo_mode()) {
        // Demo mode - use in-memory value
        celsius_val = get_demo_setpoint(setpoint_type);
    } else if (!hp.connected) {
        // Disconnected - use midpoint as placeholder
        celsius_val = (sp.min_val + sp.max_val) / 2;
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
    lv_label_set_text(state.edit_description, sp.description);
    
    // Range label - convert limits to display units
    char range_buf[64];
    int display_min = app_prefs_convert_temp(sp.min_val);
    int display_max = app_prefs_convert_temp(sp.max_val);
    snprintf(range_buf, sizeof(range_buf), "%s %d - %d %s", 
             i18n_get(STR_HP_RANGE_FMT), display_min, display_max, app_prefs_temp_unit_str());
    lv_label_set_text(state.edit_range_label, range_buf);
    
    update_edit_value_display();
    
    // Show dialog
    lv_obj_remove_flag(state.edit_dialog, LV_OBJ_FLAG_HIDDEN);
}

static void hide_edit_dialog(void) {
    lv_obj_add_flag(state.edit_dialog, LV_OBJ_FLAG_HIDDEN);
    state.current_param_idx = -1;
    state.current_setpoint_type = -1;
}

static void update_edit_value_display(void) {
    const char* unit = "";
    int display_val = (int)roundf(state.edit_value);  // Round float to int for display
    
    if (state.current_setpoint_type >= 0 && state.current_setpoint_type <= 2) {
        // Setpoints are always TEMP_ABSOLUTE
        unit = app_prefs_temp_unit_str();
    } else if (state.current_param_idx >= 0 && state.current_param_idx < NUM_HEATPUMP_PARAMS) {
        unit = get_param_unit_str(HEATPUMP_PARAMS[state.current_param_idx].unit_type);
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
// Public Functions
// ============================================================================

void heatpump_control_show(heatpump_control_close_cb_t on_close) {
    if (state.shown) {
        return;
    }
    
    ESP_LOGI(TAG, "Showing heat pump settings screen");
    state.on_close = on_close;
    
    // Create full-screen
    state.screen = lv_obj_create(NULL);
    lv_obj_set_size(state.screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(state.screen, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state.screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(state.screen, LV_OBJ_FLAG_SCROLLABLE);
    
    // Header with title and close button
    lv_obj_t* header = lv_obj_create(state.screen);
    lv_obj_set_size(header, LV_PCT(100), 100);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(header, 20, LV_PART_MAIN);
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
    lv_obj_add_event_cb(back_btn, close_btn_cb, LV_EVENT_CLICKED, nullptr);
    
    lv_obj_t* back_icon = lv_label_create(back_btn);
    lv_label_set_text(back_icon, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(back_icon, UI_FONT_ICON, LV_PART_MAIN);
    lv_obj_set_style_text_color(back_icon, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_center(back_icon);
    
    // Title
    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, i18n_get(STR_HP_ADVANCED));
    lv_obj_set_style_text_color(title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, UI_FONT_HEADER, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
    
    // Scrollable content - 1280 - 100 header = 1180
    state.scroll_container = lv_obj_create(state.screen);
    lv_obj_set_size(state.scroll_container, LV_PCT(100), 1180);
    lv_obj_align(state.scroll_container, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_set_style_bg_opa(state.scroll_container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.scroll_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.scroll_container, 15, LV_PART_MAIN);
    lv_obj_set_flex_flow(state.scroll_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(state.scroll_container, 10, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(state.scroll_container, LV_SCROLLBAR_MODE_AUTO);
    
    // =========================================================================
    // BASIC SETTINGS SECTION - Setpoints
    // =========================================================================
    create_section_header(state.scroll_container, i18n_get(STR_HP_SETPOINTS));
    
    // Cooling setpoint row
    create_setpoint_row(state.scroll_container, 0, &state.cooling_value_label);
    
    // Heating setpoint row  
    create_setpoint_row(state.scroll_container, 1, &state.heating_value_label);
    
    // Hot water setpoint row
    create_setpoint_row(state.scroll_container, 2, &state.hotwater_value_label);
    
    // =========================================================================
    // TECHNICIAN PARAMETERS - P-parameters grouped by category
    // =========================================================================
    
    // Create parameter rows grouped by category
    const char* current_category = nullptr;
    for (int i = 0; i < NUM_HEATPUMP_PARAMS; i++) {
        // Add section header if category changed
        if (current_category == nullptr || strcmp(current_category, HEATPUMP_PARAMS[i].category) != 0) {
            current_category = HEATPUMP_PARAMS[i].category;
            create_section_header(state.scroll_container, get_category_i18n(current_category));
        }
        
        create_param_row(state.scroll_container, i);
    }
    
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
        for (int i = 0; i < NUM_HEATPUMP_PARAMS; i++) {
            int16_t val = heatpump_param_read_by_index(i);
            update_param_display(i, val);
        }
    } else if (!hp.connected) {
        // Disconnected: show placeholder values
        ESP_LOGI(TAG, "Disconnected - showing placeholders");
        for (int i = 0; i < NUM_HEATPUMP_PARAMS; i++) {
            if (state.value_labels[i]) {
                lv_label_set_text(state.value_labels[i], "--");
            }
        }
    } else {
        // Connected: stagger reads to avoid blocking
        state.load_timer = lv_timer_create(load_timer_cb, 100, nullptr);
    }
    
    // Load the screen with slide animation (main screen moves up)
    lv_scr_load_anim(state.screen, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, false);
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
    
    heatpump_control_close_cb_t cb = state.on_close;
    
    // Reset state
    state.shown = false;
    state.on_close = nullptr;
    state.screen = nullptr;
    state.scroll_container = nullptr;
    state.cooling_value_label = nullptr;
    state.heating_value_label = nullptr;
    state.hotwater_value_label = nullptr;
    state.load_index = 0;
    state.edit_dialog = nullptr;
    state.edit_title = nullptr;
    state.edit_description = nullptr;
    state.edit_range_label = nullptr;
    state.edit_value_label = nullptr;
    state.edit_minus_btn = nullptr;
    state.edit_plus_btn = nullptr;
    state.current_param_idx = -1;
    state.current_setpoint_type = -1;
    memset(state.value_labels, 0, sizeof(state.value_labels));
    
    // Call callback to load the previous screen (animation will delete this screen)
    if (cb) {
        cb();
    }
}

bool heatpump_control_is_shown(void) {
    return state.shown;
}
