/*
 * Arctic Heat Pump Controller
 * Heat Pump Status Screen Implementation
 *
 * Every reading, grouped like the web Status page: a summary card (state,
 * freshness, uptime, component chips, defrost panel), an optional fault
 * banner, then Water loop / Performance / Refrigerant circuit / Outdoor /
 * Electrical sections. Each row leads with the OEM display code as a pill
 * (from the arctic-macon field catalog); derived readings keep an empty
 * pill-width gap so the names line up.
 *
 * Rows are table-driven and share a handful of static styles so the ~25 rows
 * cost little LVGL heap. Widgets carry stable tags ("st_<key>",
 * "st_<key>_sub", "st_<key>_code") for the device tests.
 */

#include "heatpump_temps_screen.h"
#include "nav_bar.h"
#include "heatpump_controller.h"
#include "heatpump_errors.h"
#include "ui_common.h"
#include "fonts/fonts.h"
#include "app_preferences.h"
#include "time_manager.h"
#include "i18n/i18n.h"
#include "macon_fields.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char* TAG = "hp_status";

// ============================================================================
// Colors and thresholds
// ============================================================================
#define COLOR_ROW_BG        lv_color_hex(0x1e2a4a)
#define COLOR_TEXT          lv_color_hex(0xeaeaea)
#define COLOR_NAME          lv_color_hex(0xb8b8c4)
#define COLOR_TEXT_DIM      lv_color_hex(0x888888)
#define COLOR_UNIT          lv_color_hex(0xaaaaaa)
#define COLOR_MUTED_VALUE   lv_color_hex(0x666666)
#define COLOR_ACCENT        lv_color_hex(0x00d4ff)
#define COLOR_PILL_BORDER   lv_color_hex(0x3a4a72)
#define COLOR_PILL_TEXT     lv_color_hex(0x9fb0d6)
#define COLOR_WARN          lv_color_hex(0xff9800)
#define COLOR_ALERT         lv_color_hex(0xf44336)
#define COLOR_OK            lv_color_hex(0x4caf50)
#define COLOR_CHIP_BORDER   lv_color_hex(0x2e3a5c)
#define COLOR_CHIP_ON       lv_color_hex(0x2f6b3a)
#define COLOR_CHIP_DOT_OFF  lv_color_hex(0x555555)
#define COLOR_DEFROST       lv_color_hex(0x3b82f6)
#define COLOR_DEFROST_EDGE  lv_color_hex(0x2f5fa8)

static constexpr int ROW_H = 58;
static constexpr int PILL_W = 66;
static constexpr int PILL_H = 34;
static constexpr uint32_t STALE_S = 30;          // no read for this long: stale
static constexpr uint32_t DT_SETTLE_S = 120;     // pump run before dT is judged
static constexpr uint32_t MIN_POWER_W = 100;     // below this the COP is noise
static constexpr int16_t DISCHARGE_WARN_C = 100;
static constexpr int16_t IPM_WARN_C = 80;

// ============================================================================
// Row table
// ============================================================================
enum class Group : uint8_t { WATER, PERF, REFRIG, OUTDOOR, ELEC, COUNT };

enum RowId : uint8_t {
    R_OUTLET, R_INLET, R_SUPPLY, R_RETURN, R_WATER_DT, R_FLOW, R_TANK,
    R_HEAT_OUT, R_COP, R_POWER, R_ENERGY,
    R_COMP, R_DISCHARGE, R_SUCTION, R_SUPERHEAT, R_WATER_HX, R_HX_APPROACH, R_MAIN_EEV, R_AUX_EEV,
    R_OUTDOOR_AIR, R_OUTDOOR_COIL, R_COIL_APPROACH, R_FAN,
    R_AC_V, R_AC_A, R_DC_V, R_IPM,
    R_COUNT
};

struct RowDef {
    Group group;
    const char* field;      // macon field supplying the OEM code; nullptr = derived
    string_id_t name;
    const char* tag_val;
    const char* tag_sub;
    const char* tag_code;
};

#define ROW(g, field, name, key) { Group::g, field, name, "st_" key, "st_" key "_sub", "st_" key "_code" }
static const RowDef ROWS[R_COUNT] = {
    ROW(WATER,   "outlet_water_temp",    STR_HISTORY_OUTLET,       "outlet"),
    ROW(WATER,   "inlet_water_temp",     STR_HISTORY_INLET,        "inlet"),
    ROW(WATER,   nullptr,                STR_ST_SUPPLY,            "supply"),
    ROW(WATER,   nullptr,                STR_ST_RETURN,            "return"),
    ROW(WATER,   nullptr,                STR_ST_WATER_DT,          "water_dt"),
    ROW(WATER,   nullptr,                STR_ST_FLOW,              "flow"),
    ROW(WATER,   "water_tank_temp",      STR_ST_TANK,              "tank"),
    ROW(PERF,    nullptr,                STR_ST_HEAT_OUTPUT,       "heat_out"),
    ROW(PERF,    nullptr,                STR_HP_LABEL_COP,         "cop"),
    ROW(PERF,    "realtime_power",       STR_ST_INPUT_POWER,       "power"),
    ROW(PERF,    nullptr,                STR_ST_ENERGY_TODAY,      "energy_today"),
    ROW(REFRIG,  "compressor_freq",      STR_HP_COMPRESSOR,        "compressor_freq"),
    ROW(REFRIG,  "discharge_temp",       STR_ST_DISCHARGE,         "discharge"),
    ROW(REFRIG,  "suction_temp",         STR_ST_SUCTION,           "suction"),
    ROW(REFRIG,  nullptr,                STR_ST_SUPERHEAT,         "superheat"),
    ROW(REFRIG,  "indoor_coil_temp",     STR_ST_WATER_HX,          "water_hx"),
    ROW(REFRIG,  nullptr,                STR_ST_WATER_HX_APPROACH, "water_hx_approach"),
    ROW(REFRIG,  "primary_eev",          STR_ST_MAIN_EEV,          "primary_eev"),
    ROW(REFRIG,  "aux_eev",              STR_HP_AUX_EEV,           "aux_eev"),
    ROW(OUTDOOR, "outdoor_ambient_temp", STR_ST_OUTDOOR_AIR,       "outdoor"),
    ROW(OUTDOOR, "outdoor_coil_temp",    STR_ST_OUTDOOR_COIL,      "outdoor_coil"),
    ROW(OUTDOOR, nullptr,                STR_ST_COIL_APPROACH,     "coil_approach"),
    ROW(OUTDOOR, "fan_speed",            STR_HP_FAN,               "fan_rpm"),
    ROW(ELEC,    "ac_voltage",           STR_ST_AC_VOLTAGE,        "ac_voltage"),
    ROW(ELEC,    "ac_current",           STR_ST_AC_CURRENT,        "ac_current"),
    ROW(ELEC,    "dc_voltage",           STR_ST_DC_BUS,            "dc_voltage"),
    ROW(ELEC,    "ipm_temp",             STR_ST_IPM_MODULE,        "ipm"),
};
#undef ROW

static const struct { string_id_t title; const char* tag; } GROUPS[(int)Group::COUNT] = {
    {STR_ST_WATER_LOOP,  "st_hdr_water"},
    {STR_ST_PERFORMANCE, "st_hdr_perf"},
    {STR_ST_REFRIGERANT, "st_hdr_refrig"},
    {STR_ST_OUTDOOR,     "st_hdr_outdoor"},
    {STR_ST_ELECTRICAL,  "st_hdr_elec"},
};

enum ChipId : uint8_t { C_COMP, C_FAN, C_PUMP, C_AUX, C_DEFROST, C_COUNT };
static const struct { string_id_t name; const char* tag; } CHIPS[C_COUNT] = {
    {STR_HP_COMPRESSOR,  "st_chip_compressor"},
    {STR_HP_FAN,         "st_chip_fan"},
    {STR_HP_PUMP,        "st_chip_pump"},
    {STR_ST_AUX_HEATER,  "st_chip_aux_heater"},
    {STR_ST_DEFROST,     "st_chip_defrost"},
};

enum DefCell : uint8_t { D_COIL, D_AIR, D_WATER, D_START, D_COUNT };

// ============================================================================
// State
// ============================================================================
struct RowWidgets {
    lv_obj_t* row;
    lv_obj_t* name;
    lv_obj_t* sub;
    lv_obj_t* trend;   // tank only
    lv_obj_t* value;
    lv_obj_t* unit;
};

static struct {
    bool shown = false;
    bool active = false;
    lv_obj_t* screen = nullptr;
    lv_obj_t* content = nullptr;
    heatpump_temps_close_cb_t on_close = nullptr;
    lv_timer_t* update_timer = nullptr;

    lv_obj_t* fault = nullptr;
    lv_obj_t* fault_code = nullptr;
    lv_obj_t* fault_title = nullptr;
    lv_obj_t* fault_sub = nullptr;

    lv_obj_t* meta_dot = nullptr;
    lv_obj_t* meta = nullptr;
    lv_obj_t* uptime = nullptr;
    lv_obj_t* now = nullptr;
    lv_obj_t* now_detail = nullptr;

    lv_obj_t* defrost = nullptr;
    lv_obj_t* defrost_elapsed = nullptr;
    lv_obj_t* defrost_cell[D_COUNT] = {};
    lv_obj_t* defrost_val[D_COUNT] = {};

    lv_obj_t* chip[C_COUNT] = {};
    lv_obj_t* chip_dot[C_COUNT] = {};
    lv_obj_t* chip_label[C_COUNT] = {};

    RowWidgets rows[R_COUNT] = {};
} state;

static bool s_styles_ready = false;
static lv_style_t s_row, s_pill, s_name, s_sub, s_value, s_unit, s_trend, s_flat, s_hdr;

static void init_styles() {
    if (s_styles_ready) return;
    s_styles_ready = true;

    lv_style_init(&s_flat);
    lv_style_set_bg_opa(&s_flat, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_flat, 0);
    lv_style_set_pad_all(&s_flat, 0);

    lv_style_init(&s_row);
    lv_style_set_bg_color(&s_row, COLOR_ROW_BG);
    lv_style_set_bg_opa(&s_row, LV_OPA_COVER);
    lv_style_set_radius(&s_row, 12);
    lv_style_set_border_width(&s_row, 0);
    lv_style_set_pad_left(&s_row, 14);
    lv_style_set_pad_right(&s_row, 20);
    lv_style_set_pad_ver(&s_row, 6);
    lv_style_set_pad_column(&s_row, 14);
    lv_style_set_min_height(&s_row, ROW_H);

    lv_style_init(&s_pill);
    lv_style_set_width(&s_pill, PILL_W);
    lv_style_set_height(&s_pill, PILL_H);
    lv_style_set_radius(&s_pill, 8);
    lv_style_set_border_width(&s_pill, 2);
    lv_style_set_border_color(&s_pill, COLOR_PILL_BORDER);
    lv_style_set_text_color(&s_pill, COLOR_PILL_TEXT);
    lv_style_set_text_font(&s_pill, &montserrat_20_latin);
    lv_style_set_text_align(&s_pill, LV_TEXT_ALIGN_CENTER);
    lv_style_set_pad_top(&s_pill, 4);

    lv_style_init(&s_name);
    lv_style_set_text_font(&s_name, &montserrat_24_latin);
    lv_style_set_text_color(&s_name, COLOR_NAME);

    lv_style_init(&s_sub);
    lv_style_set_text_font(&s_sub, &montserrat_16_latin);
    lv_style_set_text_color(&s_sub, COLOR_TEXT_DIM);

    lv_style_init(&s_trend);
    lv_style_set_text_font(&s_trend, &montserrat_16_latin);
    lv_style_set_text_color(&s_trend, COLOR_TEXT_DIM);

    lv_style_init(&s_value);
    lv_style_set_text_font(&s_value, &montserrat_32_latin);
    lv_style_set_text_color(&s_value, COLOR_TEXT);

    lv_style_init(&s_unit);
    lv_style_set_text_font(&s_unit, &montserrat_20_latin);
    lv_style_set_text_color(&s_unit, COLOR_UNIT);
    lv_style_set_pad_bottom(&s_unit, 4);
    lv_style_set_pad_left(&s_unit, 4);

    lv_style_init(&s_hdr);
    lv_style_set_bg_color(&s_hdr, COLOR_ACCENT);
    lv_style_set_bg_opa(&s_hdr, LV_OPA_20);
    lv_style_set_radius(&s_hdr, 12);
    lv_style_set_border_width(&s_hdr, 0);
    lv_style_set_pad_hor(&s_hdr, 20);
    lv_style_set_height(&s_hdr, 46);
    lv_style_set_text_font(&s_hdr, &montserrat_24_latin);
    lv_style_set_text_color(&s_hdr, COLOR_ACCENT);
}

// ============================================================================
// Builders
// ============================================================================

static lv_obj_t* flat_box(lv_obj_t* parent, lv_flex_flow_t flow) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_add_style(o, &s_flat, LV_PART_MAIN);
    lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, flow);
    lv_obj_clear_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    return o;
}

static lv_obj_t* styled_label(lv_obj_t* parent, lv_style_t* style, const char* text) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_add_style(l, style, LV_PART_MAIN);
    lv_label_set_text(l, text);
    return l;
}

static void set_tag(lv_obj_t* o, const char* tag) {
    lv_obj_set_user_data(o, (void*)tag);
}

static void set_visible(lv_obj_t* o, bool visible) {
    if (visible) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// Set label text only when it changed, so the 1 s refresh doesn't redraw
// every label on screen.
static void set_text(lv_obj_t* l, const char* text) {
    const char* cur = lv_label_get_text(l);
    if (cur == nullptr || strcmp(cur, text) != 0) lv_label_set_text(l, text);
}

// OEM code pill; nullptr code keeps the pill's width but draws nothing.
static lv_obj_t* create_pill(lv_obj_t* parent, const char* code, const char* tag) {
    lv_obj_t* p = styled_label(parent, &s_pill, code ? code : "");
    if (code == nullptr) {
        lv_obj_set_style_border_width(p, 0, LV_PART_MAIN);
    } else if (tag) {
        set_tag(p, tag);
    }
    return p;
}

static void create_header(lv_obj_t* parent, Group g) {
    lv_obj_t* h = lv_obj_create(parent);
    lv_obj_remove_style_all(h);
    lv_obj_add_style(h, &s_hdr, LV_PART_MAIN);
    lv_obj_set_width(h, LV_PCT(100));
    lv_obj_clear_flag(h, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_style_margin_top(h, 6, LV_PART_MAIN);
    lv_obj_t* l = lv_label_create(h);
    lv_label_set_text(l, i18n_get(GROUPS[(int)g].title));
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    set_tag(l, GROUPS[(int)g].tag);
}

static void create_row(lv_obj_t* parent, RowId id) {
    const RowDef& def = ROWS[id];
    RowWidgets& w = state.rows[id];

    lv_obj_t* row = lv_obj_create(parent);
    w.row = row;
    lv_obj_remove_style_all(row);
    lv_obj_add_style(row, &s_row, LV_PART_MAIN);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    const char* code = nullptr;
    if (def.field) {
        const arctic::MaconFieldDesc* d = arctic::macon_field_find(def.field);
        if (d != nullptr) code = d->oem_code;
    }
    create_pill(row, code, def.tag_code);

    lv_obj_t* names = flat_box(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(names, 1);
    w.name = styled_label(names, &s_name, i18n_get(def.name));
    lv_obj_set_width(w.name, LV_PCT(100));
    w.sub = styled_label(names, &s_sub, "");
    lv_obj_set_width(w.sub, LV_PCT(100));
    set_tag(w.sub, def.tag_sub);
    lv_obj_add_flag(w.sub, LV_OBJ_FLAG_HIDDEN);

    if (id == R_TANK) {
        w.trend = styled_label(row, &s_trend, "");
        lv_obj_set_style_pad_right(w.trend, 10, LV_PART_MAIN);
        set_tag(w.trend, "st_tank_trend");
    }

    lv_obj_t* vbox = flat_box(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vbox, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    w.value = styled_label(vbox, &s_value, "--");
    set_tag(w.value, def.tag_val);
    w.unit = styled_label(vbox, &s_unit, "");
}

static lv_obj_t* create_card(lv_obj_t* parent, lv_color_t bg, lv_opa_t opa, lv_color_t border, int border_w) {
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(c, opa, LV_PART_MAIN);
    lv_obj_set_style_radius(c, 14, LV_PART_MAIN);
    lv_obj_set_style_border_width(c, border_w, LV_PART_MAIN);
    lv_obj_set_style_border_color(c, border, LV_PART_MAIN);
    lv_obj_clear_flag(c, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    return c;
}

static void create_fault_banner(lv_obj_t* parent) {
    lv_obj_t* f = create_card(parent, COLOR_ALERT, LV_OPA_20, COLOR_ALERT, 2);
    lv_obj_set_style_pad_hor(f, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(f, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_column(f, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(f, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(f, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    set_tag(f, "st_fault");
    state.fault = f;

    state.fault_code = create_pill(f, "", "st_fault_code");
    lv_obj_set_style_border_color(state.fault_code, COLOR_ALERT, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.fault_code, COLOR_ALERT, LV_PART_MAIN);

    lv_obj_t* col = flat_box(f, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(col, 1);
    state.fault_title = styled_label(col, &s_name, "");
    lv_obj_set_style_text_color(state.fault_title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_width(state.fault_title, LV_PCT(100));
    set_tag(state.fault_title, "st_fault_title");
    state.fault_sub = styled_label(col, &s_sub, "");
    lv_obj_set_style_text_color(state.fault_sub, lv_color_hex(0xbbbbbb), LV_PART_MAIN);
    lv_obj_set_width(state.fault_sub, LV_PCT(100));
    lv_obj_add_flag(f, LV_OBJ_FLAG_HIDDEN);
}

static void create_summary(lv_obj_t* parent) {
    lv_obj_t* s = create_card(parent, COLOR_ROW_BG, LV_OPA_COVER, COLOR_ROW_BG, 0);
    lv_obj_set_style_pad_hor(s, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);
    set_tag(s, "st_summary");

    // "● Connected · updated 3 s ago"                         "Up 8h 51m"
    lv_obj_t* meta_row = flat_box(s, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(meta_row, LV_PCT(100));
    lv_obj_set_flex_align(meta_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(meta_row, 8, LV_PART_MAIN);
    state.meta_dot = lv_obj_create(meta_row);
    lv_obj_remove_style_all(state.meta_dot);
    lv_obj_set_size(state.meta_dot, 12, 12);
    lv_obj_set_style_radius(state.meta_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state.meta_dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(state.meta_dot, COLOR_OK, LV_PART_MAIN);
    state.meta = styled_label(meta_row, &s_sub, "");
    lv_obj_set_style_text_font(state.meta, &montserrat_20_latin, LV_PART_MAIN);
    lv_obj_set_flex_grow(state.meta, 1);
    set_tag(state.meta, "st_meta");
    state.uptime = styled_label(meta_row, &s_sub, "");
    lv_obj_set_style_text_font(state.uptime, &montserrat_20_latin, LV_PART_MAIN);
    set_tag(state.uptime, "st_uptime");

    // "Heating · Auto mode · target 45 °C"
    lv_obj_t* op = flat_box(s, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_width(op, LV_PCT(100));
    lv_obj_set_flex_align(op, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(op, 10, LV_PART_MAIN);
    state.now = styled_label(op, &s_value, "--");
    lv_obj_set_style_text_font(state.now, &montserrat_40_latin, LV_PART_MAIN);
    set_tag(state.now, "st_now");
    state.now_detail = styled_label(op, &s_name, "");
    lv_obj_set_style_text_color(state.now_detail, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(state.now_detail, 3, LV_PART_MAIN);
    set_tag(state.now_detail, "st_now_detail");

    // Defrost panel
    lv_obj_t* d = create_card(s, COLOR_DEFROST, LV_OPA_20, COLOR_DEFROST_EDGE, 2);
    lv_obj_set_style_radius(d, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(d, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(d, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(d, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(d, LV_FLEX_FLOW_ROW_WRAP);
    set_tag(d, "st_defrost");
    state.defrost = d;
    lv_obj_t* title = styled_label(d, &s_name, i18n_get(STR_HOME_DEFROSTING));
    lv_obj_set_style_text_color(title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_flex_grow(title, 1);
    state.defrost_elapsed = styled_label(d, &s_name, "");
    lv_obj_set_style_text_color(state.defrost_elapsed, COLOR_TEXT, LV_PART_MAIN);
    set_tag(state.defrost_elapsed, "st_defrost_elapsed");
    static const string_id_t cell_keys[D_COUNT] = {
        STR_ST_OUTDOOR_COIL, STR_ST_OUTDOOR_AIR, STR_ST_OUTLET_INLET, STR_ST_COIL_AT_START};
    static const char* const cell_tags[D_COUNT] = {
        "st_defrost_coil", "st_defrost_air", "st_defrost_water", "st_defrost_start_coil"};
    for (int i = 0; i < D_COUNT; i++) {
        lv_obj_t* cell = flat_box(d, LV_FLEX_FLOW_ROW);
        lv_obj_set_width(cell, LV_PCT(50));
        lv_obj_set_style_pad_column(cell, 6, LV_PART_MAIN);
        if (i == 0) lv_obj_add_flag(cell, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
        lv_obj_t* k = styled_label(cell, &s_sub, i18n_get(cell_keys[i]));
        lv_obj_set_style_text_font(k, &montserrat_20_latin, LV_PART_MAIN);
        lv_obj_set_style_text_color(k, lv_color_hex(0x999999), LV_PART_MAIN);
        state.defrost_val[i] = styled_label(cell, &s_sub, "");
        lv_obj_set_style_text_font(state.defrost_val[i], &montserrat_20_latin, LV_PART_MAIN);
        lv_obj_set_style_text_color(state.defrost_val[i], COLOR_TEXT, LV_PART_MAIN);
        set_tag(state.defrost_val[i], cell_tags[i]);
        state.defrost_cell[i] = cell;
    }
    lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);

    // Component chips
    lv_obj_t* chips = flat_box(s, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_width(chips, LV_PCT(100));
    lv_obj_set_style_pad_column(chips, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(chips, 10, LV_PART_MAIN);
    for (int i = 0; i < C_COUNT; i++) {
        lv_obj_t* chip = flat_box(chips, LV_FLEX_FLOW_ROW);
        lv_obj_set_height(chip, 44);
        lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_radius(chip, 22, LV_PART_MAIN);
        lv_obj_set_style_border_width(chip, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(chip, COLOR_CHIP_BORDER, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(chip, 16, LV_PART_MAIN);
        lv_obj_set_style_pad_column(chip, 10, LV_PART_MAIN);
        lv_obj_t* dot = lv_obj_create(chip);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 12, 12);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, COLOR_CHIP_DOT_OFF, LV_PART_MAIN);
        set_tag(dot, CHIPS[i].tag);
        lv_obj_t* l = styled_label(chip, &s_sub, i18n_get(CHIPS[i].name));
        lv_obj_set_style_text_font(l, &montserrat_20_latin, LV_PART_MAIN);
        state.chip[i] = chip;
        state.chip_dot[i] = dot;
        state.chip_label[i] = l;
    }
}

// ============================================================================
// Formatting
// ============================================================================

static void fmt_int(char* buf, size_t len, int v) { snprintf(buf, len, "%d", v); }

// One decimal from a x1000 integer (W -> kW, Wh -> kWh).
static void fmt_milli_1(char* buf, size_t len, int32_t milli) {
    const int32_t tenths = (milli >= 0 ? milli + 50 : milli - 50) / 100;
    snprintf(buf, len, "%s%ld.%ld", tenths < 0 ? "-" : "", (long)(labs(tenths) / 10), (long)(labs(tenths) % 10));
}

static void fmt_milli_2(char* buf, size_t len, uint32_t milli) {
    const uint32_t hundredths = (milli + 5) / 10;
    snprintf(buf, len, "%lu.%02lu", (unsigned long)(hundredths / 100), (unsigned long)(hundredths % 100));
}

static void fmt_span(char* buf, size_t len, uint32_t s) {
    const uint32_t m = s / 60, h = m / 60, d = h / 24;
    if (d > 0) snprintf(buf, len, "%lud %luh", (unsigned long)d, (unsigned long)(h % 24));
    else if (h > 0) snprintf(buf, len, "%luh %lum", (unsigned long)h, (unsigned long)(m % 60));
    else if (m > 0) snprintf(buf, len, "%lum %02lus", (unsigned long)m, (unsigned long)(s % 60));
    else snprintf(buf, len, "%lus", (unsigned long)s);
}

static void fmt_clock(char* buf, size_t len, time_t t) {
    struct tm local = {};
    localtime_r(&t, &local);
    strftime(buf, len, time_mgr_get_24h_format() ? "%H:%M" : "%I:%M %p", &local);
}

struct Cell {
    char value[24] = "--";
    const char* unit = "";
    const char* sub = "";
    char sub_buf[48] = "";
    lv_color_t color = COLOR_TEXT;
    bool warn = false;
};

static void cell_temp(Cell& c, int16_t celsius) {
    fmt_int(c.value, sizeof(c.value), app_prefs_convert_temp(celsius));
    c.unit = app_prefs_temp_unit_str();
}

// A temperature difference: x1.8 in Fahrenheit, never an offset.
static void cell_delta(Cell& c, int16_t celsius_diff, bool signed_) {
    const int v = app_prefs_convert_temp_diff(celsius_diff);
    snprintf(c.value, sizeof(c.value), signed_ && v > 0 ? "+%d" : "%d", v);
    c.unit = app_prefs_temp_unit_str();
}

// One decimal from a 0.1 °C value; diff: a difference (no Fahrenheit offset).
static void cell_tenths(Cell& c, int16_t x10, bool diff, bool signed_) {
    int32_t v = x10;
    if (app_prefs_get_temp_unit() == TEMP_UNIT_FAHRENHEIT) {
        v = (v * 9 + (v >= 0 ? 2 : -2)) / 5 + (diff ? 0 : 320);
    }
    const int32_t a = v < 0 ? -v : v;
    snprintf(c.value, sizeof(c.value), "%s%ld.%ld", v < 0 ? "-" : (signed_ && v > 0 ? "+" : ""),
             (long)(a / 10), (long)(a % 10));
    c.unit = app_prefs_temp_unit_str();
}

static void cell_hidden(Cell& c, const char* why) {
    strcpy(c.value, "--");
    c.unit = "";
    c.sub = why;
    c.color = COLOR_MUTED_VALUE;
}

static void apply_cell(RowId id, const Cell& c) {
    RowWidgets& w = state.rows[id];
    set_text(w.value, c.value);
    set_text(w.unit, c.unit);
    lv_obj_set_style_text_color(w.value, c.color, LV_PART_MAIN);
    const char* sub = c.sub_buf[0] ? c.sub_buf : c.sub;
    set_text(w.sub, sub);
    lv_obj_set_style_text_color(w.sub, c.warn ? COLOR_WARN : COLOR_TEXT_DIM, LV_PART_MAIN);
    set_visible(w.sub, sub[0] != '\0');
}

static const char* mode_name(arctic::WorkingMode m) {
    switch (m) {
        case arctic::WorkingMode::COOLING:           return i18n_get(STR_HP_OPT_COOLING);
        case arctic::WorkingMode::HEATING:           return i18n_get(STR_HP_OPT_HEATING);
        case arctic::WorkingMode::HOT_WATER:         return i18n_get(STR_HP_OPT_HOT_WATER);
        case arctic::WorkingMode::HOT_WATER_COOLING: return i18n_get(STR_HP_OPT_HOT_WATER_COOLING);
        case arctic::WorkingMode::FREEZE_PROTECTION: return i18n_get(STR_HOME_FREEZE_PROTECTION);
        case arctic::WorkingMode::MODE_3:            return i18n_get(STR_HP_MODE_MODE_3);
        case arctic::WorkingMode::MODE_4:            return i18n_get(STR_HP_MODE_MODE_4);
        default:                                     return nullptr;
    }
}

// Same states as the home screen hero.
static const char* now_text(const arctic::HeatPumpState& hp) {
    if (!hp.connected) return i18n_get(STR_HOME_DISCONNECTED);
    // The fault banner already covers faults; show what the unit is doing.
    if (hp.isDefrosting()) return i18n_get(STR_HOME_DEFROSTING);
    switch (hp.operation) {
        case arctic::HeatPumpOperation::FAULT:   return i18n_get(STR_HOME_FAULT);
        case arctic::HeatPumpOperation::DEFROST: return i18n_get(STR_HOME_DEFROSTING);
        case arctic::HeatPumpOperation::HEATING: return i18n_get(STR_HOME_HEATING);
        case arctic::HeatPumpOperation::COOLING: return i18n_get(STR_HOME_COOLING);
        default: break;
    }
    if (hp.hasAnyError()) return i18n_get(STR_HOME_FAULT);
    const bool on = hp.operation == arctic::HeatPumpOperation::IDLE ||
                    (hp.operation == arctic::HeatPumpOperation::UNKNOWN && hp.unit_on);
    if (hp.working_mode == arctic::WorkingMode::FREEZE_PROTECTION) return i18n_get(STR_HOME_FREEZE_PROTECTION);
    return i18n_get(on ? STR_HOME_IDLE : STR_HOME_STANDBY);
}

// ============================================================================
// Update
// ============================================================================

static void update_fault(const arctic::HeatPumpState& hp) {
    static arctic::ActiveError errors[8];
    const int count = hp.connected && hp.hasAnyError() ? arctic::getActiveErrors(errors, 8) : 0;
    set_visible(state.fault, count > 0);
    if (count <= 0) return;
    const arctic::ActiveError& e = errors[0];
    set_text(state.fault_code, e.code ? e.code : "");
    set_text(state.fault_title, i18n_get_key(e.name_msg_id, e.name ? e.name : ""));
    char sub[96] = "";
    size_t n = 0;
    if (e.first_seen > 0 && time_mgr_is_synced()) {
        char clock[16];
        fmt_clock(clock, sizeof(clock), e.first_seen);
        n += snprintf(sub + n, sizeof(sub) - n, i18n_get(STR_ST_SINCE_FMT), clock);
    }
    if (count > 1 && n < sizeof(sub)) {
        char more[24];
        snprintf(more, sizeof(more), i18n_get(STR_HOME_MORE_FAULTS), (unsigned)(count - 1));
        n += snprintf(sub + n, sizeof(sub) - n, "%s%s", n ? " \xC2\xB7 " : "", more);
    }
    set_text(state.fault_sub, sub);
    set_visible(state.fault_sub, sub[0] != '\0');
}

static void update_summary(const arctic::HeatPumpState& hp, const arctic::TelemetrySnapshot& snap,
                           uint32_t now_ms, bool fresh, uint32_t age_s) {
    char buf[96], span[24];

    if (!hp.connected) {
        set_text(state.meta, i18n_get(STR_HP_NOT_CONNECTED));
    } else if (!fresh) {
        fmt_span(span, sizeof(span), age_s);
        snprintf(buf, sizeof(buf), i18n_get(STR_ST_STALE_FMT), span);
        set_text(state.meta, buf);
    } else {
        snprintf(buf, sizeof(buf), i18n_get(STR_ST_UPDATED_FMT), (unsigned)age_s);
        set_text(state.meta, buf);
    }
    lv_obj_set_style_bg_color(state.meta_dot, fresh ? COLOR_OK : COLOR_ALERT, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.meta, fresh ? COLOR_TEXT_DIM : COLOR_ALERT, LV_PART_MAIN);

    fmt_span(span, sizeof(span), now_ms / 1000);
    snprintf(buf, sizeof(buf), i18n_get(STR_ST_UP_FMT), span);
    set_text(state.uptime, buf);

    set_text(state.now, now_text(hp));
    buf[0] = '\0';
    if (hp.connected) {
        size_t n = 0;
        const char* mode = mode_name(hp.working_mode);
        if (mode) {
            char m[48];
            snprintf(m, sizeof(m), i18n_get(STR_ST_MODE_FMT), mode);
            n += snprintf(buf + n, sizeof(buf) - n, "\xC2\xB7 %s", m);
        }
        if (snap.setpoint_valid && n < sizeof(buf)) {
            char t[24], tg[40];
            snprintf(t, sizeof(t), "%d %s", app_prefs_convert_temp(snap.active_setpoint_c), app_prefs_temp_unit_str());
            snprintf(tg, sizeof(tg), i18n_get(STR_ST_TARGET_FMT), t);
            n += snprintf(buf + n, sizeof(buf) - n, " \xC2\xB7 %s", tg);
        }
    }
    set_text(state.now_detail, buf);
    set_visible(state.now_detail, buf[0] != '\0');

    const bool on[C_COUNT] = {
        hp.connected && hp.isCompressorRunning(), hp.connected && hp.isFanRunning(),
        hp.connected && hp.isWaterPumpRunning(), hp.connected && hp.isBackupHeaterOn(),
        hp.connected && hp.isDefrosting()};
    for (int i = 0; i < C_COUNT; i++) {
        const lv_color_t lit = i == C_DEFROST ? COLOR_DEFROST : COLOR_OK;
        const lv_color_t edge = i == C_DEFROST ? COLOR_DEFROST_EDGE : COLOR_CHIP_ON;
        lv_obj_set_style_bg_color(state.chip_dot[i], on[i] ? lit : COLOR_CHIP_DOT_OFF, LV_PART_MAIN);
        lv_obj_set_style_border_color(state.chip[i], on[i] ? edge : COLOR_CHIP_BORDER, LV_PART_MAIN);
        lv_obj_set_style_text_color(state.chip_label[i], on[i] ? COLOR_TEXT : COLOR_TEXT_DIM, LV_PART_MAIN);
    }

    const bool defrost = hp.connected && hp.isDefrosting();
    set_visible(state.defrost, defrost);
    if (defrost) {
        buf[0] = '\0';
        if (hp.defrost_start_ms != 0) fmt_span(buf, sizeof(buf), (now_ms - hp.defrost_start_ms) / 1000);
        set_text(state.defrost_elapsed, buf);
        const char* u = app_prefs_temp_unit_str();
        snprintf(buf, sizeof(buf), "%d %s", app_prefs_convert_temp(hp.outdoor_coil_temp), u);
        set_text(state.defrost_val[D_COIL], buf);
        snprintf(buf, sizeof(buf), "%d %s", app_prefs_convert_temp(hp.outdoor_ambient_temp), u);
        set_text(state.defrost_val[D_AIR], buf);
        snprintf(buf, sizeof(buf), "%d / %d %s", app_prefs_convert_temp(hp.outlet_water_temp),
                 app_prefs_convert_temp(hp.inlet_water_temp), u);
        set_text(state.defrost_val[D_WATER], buf);
        set_visible(state.defrost_cell[D_START], hp.defrost_start_coil_valid);
        if (hp.defrost_start_coil_valid) {
            snprintf(buf, sizeof(buf), "%d %s", app_prefs_convert_temp(hp.defrost_start_coil_c), u);
            set_text(state.defrost_val[D_START], buf);
        }
    }
}

static void update_readings() {
    if (!state.shown) return;

    // Render from getState() only (the demo seeds live there too), so this
    // screen can't drift from the home screen.
    const arctic::HeatPumpState hp = arctic::getState();
    const arctic::TelemetrySnapshot snap = arctic::getTelemetrySnapshot();
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    const bool connected = hp.connected;
    const uint32_t age_s = connected && hp.last_successful_read_ms != 0
                               ? (now_ms - hp.last_successful_read_ms) / 1000 : 0;
    const bool fresh = connected && hp.last_successful_read_ms != 0 && age_s <= STALE_S;

    update_fault(hp);
    update_summary(hp, snap, now_ms, fresh, age_s);

    Cell c[R_COUNT];
    if (!connected) {
        for (int i = 0; i < R_COUNT; i++) c[i].color = COLOR_MUTED_VALUE;
        for (int i = 0; i < R_COUNT; i++) apply_cell((RowId)i, c[i]);
        set_text(state.rows[R_TANK].trend, "");
        return;
    }

    const bool defrost = hp.isDefrosting();
    const bool comp = hp.isCompressorRunning();
    const bool pump = hp.isWaterPumpRunning();
    const bool heating = hp.operation == arctic::HeatPumpOperation::HEATING;
    const bool cooling = hp.operation == arctic::HeatPumpOperation::COOLING;
    const bool steady_run = comp && !defrost && fresh;

    // Water loop. Supply/return/ΔT are the estimate's own temperatures:
    // external sensors (tenths) when in use, else the unit's outlet/inlet.
    cell_temp(c[R_OUTLET], hp.outlet_water_temp);
    cell_temp(c[R_INLET], hp.inlet_water_temp);
    const bool ext_rows = hp.perf_external && hp.supply_return_valid;
    lv_obj_t* const ext_row_objs[] = {state.rows[R_SUPPLY].row, state.rows[R_RETURN].row};
    for (lv_obj_t* o : ext_row_objs) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == ext_rows) set_visible(o, ext_rows);
    }
    if (ext_rows) {
        cell_tenths(c[R_SUPPLY], hp.supply_x10, false, false);
        cell_tenths(c[R_RETURN], hp.return_x10, false, false);
        c[R_SUPPLY].sub = c[R_RETURN].sub = i18n_get(STR_ST_SUB_EXTERNAL_SENSOR);
    }
    c[R_WATER_DT].sub = i18n_get(hp.perf_external ? STR_ST_SUB_SUPPLY_RETURN_EXT : STR_ST_SUB_OUTLET_INLET);
    if (hp.water_delta_valid && fresh) {
        const int16_t d = hp.supply_x10 - hp.return_x10;
        if (hp.perf_external) cell_tenths(c[R_WATER_DT], d, true, true);
        else cell_delta(c[R_WATER_DT], d / 10, true);
        const bool settled = hp.pump_on_since_ms != 0 && (now_ms - hp.pump_on_since_ms) / 1000 >= DT_SETTLE_S;
        if (defrost && d < 0) {
            c[R_WATER_DT].sub = i18n_get(STR_ST_SUB_REVERSED_DEFROST);
            c[R_WATER_DT].color = COLOR_WARN;
            c[R_WATER_DT].warn = true;
        } else if (!defrost && settled && comp && ((heating && d <= 0) || (cooling && d >= 0))) {
            c[R_WATER_DT].sub = i18n_get(d == 0 ? STR_ST_SUB_NO_DT : STR_ST_SUB_REVERSED);
            c[R_WATER_DT].color = COLOR_WARN;
            c[R_WATER_DT].warn = true;
        }
    }
    if (hp.flow_lpm_x10 % 10) {
        snprintf(c[R_FLOW].value, sizeof(c[R_FLOW].value), "%u.%u", hp.flow_lpm_x10 / 10, hp.flow_lpm_x10 % 10);
    } else {
        snprintf(c[R_FLOW].value, sizeof(c[R_FLOW].value), "%u", hp.flow_lpm_x10 / 10);
    }
    c[R_FLOW].unit = "L/min";
    c[R_FLOW].sub = i18n_get(STR_ST_SUB_ASSUMED);
    cell_temp(c[R_TANK], hp.water_tank_temp);
    const char* trend = snap.tank_trend == TempTrend::RISING ? i18n_get(STR_ST_TREND_WARMING)
                      : snap.tank_trend == TempTrend::FALLING ? i18n_get(STR_ST_TREND_COOLING) : "";
    set_text(state.rows[R_TANK].trend, trend);

    // Performance: meaningless while defrosting, idle, with no flow, or when
    // the compressor is barely drawing anything.
    const char* paused = defrost ? i18n_get(STR_ST_SUB_PAUSED_DEFROST)
                       : !comp ? i18n_get(STR_ST_SUB_COMP_OFF)
                       : !pump ? i18n_get(STR_ST_SUB_NO_FLOW)
                       : hp.realtime_power_w < MIN_POWER_W ? i18n_get(STR_ST_SUB_LOW_POWER)
                       : !hp.cop_valid ? i18n_get(STR_ST_SUB_WAITING) : nullptr;
    set_text(state.rows[R_HEAT_OUT].name, i18n_get(hp.thermal_w < 0 ? STR_ST_COOLING_OUTPUT : STR_ST_HEAT_OUTPUT));
    if (paused) {
        cell_hidden(c[R_HEAT_OUT], paused);
        cell_hidden(c[R_COP], paused);
    } else {
        fmt_milli_1(c[R_HEAT_OUT].value, sizeof(c[R_HEAT_OUT].value), labs(hp.thermal_w));
        c[R_HEAT_OUT].unit = "kW";
        c[R_HEAT_OUT].sub = i18n_get(hp.perf_external ? STR_PERF_CAPTION_EXTERNAL : STR_PERF_CAPTION_HEAT_PUMP);
        snprintf(c[R_COP].value, sizeof(c[R_COP].value), "%u.%u", (hp.cop_x100 + 5) / 100, ((hp.cop_x100 + 5) / 10) % 10);
        c[R_COP].sub = i18n_get(STR_ST_SUB_ESTIMATE);
    }
    fmt_milli_2(c[R_POWER].value, sizeof(c[R_POWER].value), hp.realtime_power_w);
    c[R_POWER].unit = "kW";
    if (hp.energy_today_valid) {
        fmt_milli_1(c[R_ENERGY].value, sizeof(c[R_ENERGY].value), (int32_t)hp.energy_today_wh);
        c[R_ENERGY].unit = "kWh";
    }

    // Refrigerant circuit
    fmt_int(c[R_COMP].value, sizeof(c[R_COMP].value), hp.compressor_freq);
    c[R_COMP].unit = "Hz";
    cell_temp(c[R_DISCHARGE], hp.discharge_temp);
    if (hp.discharge_temp >= DISCHARGE_WARN_C) {
        c[R_DISCHARGE].sub = i18n_get(STR_ST_SUB_NEAR_LIMIT);
        c[R_DISCHARGE].color = COLOR_WARN;
        c[R_DISCHARGE].warn = true;
    }
    cell_temp(c[R_SUCTION], hp.suction_temp);
    const char* hidden = defrost ? i18n_get(STR_ST_SUB_HIDDEN_DEFROST) : nullptr;
    auto estimate = [&](RowId id, bool show, int16_t diff, string_id_t sub) {
        // All three estimates are positive by construction in a steady run; a
        // reading below zero means the sensors don't fit the assumption.
        if (show && diff < 0) {
            cell_hidden(c[id], i18n_get(STR_ST_SUB_IMPLAUSIBLE));
        } else if (show) {
            cell_delta(c[id], diff, false);
            c[id].sub = i18n_get(sub);
        } else {
            cell_hidden(c[id], hidden ? hidden : i18n_get(sub));
        }
    };
    estimate(R_SUPERHEAT, steady_run && heating, hp.suction_temp - hp.outdoor_coil_temp, STR_ST_SUB_SUPERHEAT);
    cell_temp(c[R_WATER_HX], hp.indoor_coil_temp);
    c[R_WATER_HX].sub = i18n_get(STR_ST_SUB_WATER_HX);
    estimate(R_HX_APPROACH, steady_run && (heating || cooling),
             heating ? hp.indoor_coil_temp - hp.outlet_water_temp : hp.outlet_water_temp - hp.indoor_coil_temp,
             cooling ? STR_ST_SUB_HX_COOL : STR_ST_SUB_HX_HEAT);
    auto eev = [&](RowId id, bool valid, uint16_t steps) {
        if (!valid) return;
        fmt_int(c[id].value, sizeof(c[id].value), steps);
        c[id].unit = i18n_get(STR_HP_UNIT_STEPS);
        if (hp.eev_max_steps == 0) return;
        const unsigned pct = (steps * 100u + hp.eev_max_steps / 2) / hp.eev_max_steps;
        snprintf(c[id].sub_buf, sizeof(c[id].sub_buf), i18n_get(STR_ST_EEV_PCT_FMT),
                 pct > 100 ? 100u : pct);
    };
    eev(R_MAIN_EEV, hp.primary_eev_valid, hp.primary_eev_opening);
    eev(R_AUX_EEV, hp.aux_eev_valid, hp.aux_eev_opening);

    // Outdoor
    cell_temp(c[R_OUTDOOR_AIR], hp.outdoor_ambient_temp);
    cell_temp(c[R_OUTDOOR_COIL], hp.outdoor_coil_temp);
    estimate(R_COIL_APPROACH, steady_run && heating, hp.outdoor_ambient_temp - hp.outdoor_coil_temp,
             STR_ST_SUB_COIL_APPROACH);
    fmt_int(c[R_FAN].value, sizeof(c[R_FAN].value), hp.fan_speed);
    c[R_FAN].unit = i18n_get(STR_UNIT_RPM);

    // Electrical
    if (hp.ac_voltage_valid) {
        fmt_int(c[R_AC_V].value, sizeof(c[R_AC_V].value), hp.ac_voltage);
        c[R_AC_V].unit = "V";
    }
    if (hp.ac_current_valid) {
        fmt_int(c[R_AC_A].value, sizeof(c[R_AC_A].value), hp.ac_current);
        c[R_AC_A].unit = "A";
    }
    if (hp.dc_voltage_valid) {
        fmt_int(c[R_DC_V].value, sizeof(c[R_DC_V].value), hp.dc_voltage);
        c[R_DC_V].unit = "V";
    }
    cell_temp(c[R_IPM], hp.ipm_temp);
    if (hp.ipm_temp >= IPM_WARN_C) {
        c[R_IPM].sub = i18n_get(STR_ST_SUB_RUNNING_HOT);
        c[R_IPM].color = COLOR_WARN;
        c[R_IPM].warn = true;
    }

    for (int i = 0; i < R_COUNT; i++) apply_cell((RowId)i, c[i]);
}

static void update_timer_cb(lv_timer_t* timer) {
    (void)timer;
    update_readings();
}

// ============================================================================
// Public Functions
// ============================================================================

void heatpump_temps_create_in(lv_obj_t* parent) {
    if (state.shown) {
        return;
    }

    ESP_LOGI(TAG, "Building status tab");
    init_styles();
    state.on_close = nullptr;
    state.screen = parent;

    // Scrollable content fills the panel; reserve room for the persistent nav
    // bar (drawn by the tab shell) at the bottom.
    lv_obj_t* content = lv_obj_create(state.screen);
    state.content = content;
    lv_obj_set_size(content, LV_PCT(100), LV_PCT(100));
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(content, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_top(content, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(content, NAV_BAR_H + 20, LV_PART_MAIN);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 8, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
    set_tag(content, "st_content");

    create_fault_banner(content);
    create_summary(content);
    // The A11/A12 pressure readings are deliberately not shown: units without
    // transducers (all known installs) report a meaningless constant.
    Group last = Group::COUNT;
    for (int i = 0; i < R_COUNT; i++) {
        if (ROWS[i].group != last) {
            last = ROWS[i].group;
            create_header(content, last);
        }
        create_row(content, (RowId)i);
    }
    // Only shown while external sensors supply the loop temperatures.
    lv_obj_add_flag(state.rows[R_SUPPLY].row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(state.rows[R_RETURN].row, LV_OBJ_FLAG_HIDDEN);

    state.shown = true;
    state.active = false;
    state.update_timer = lv_timer_create(update_timer_cb, 1000, nullptr);
    update_readings();
}

void heatpump_temps_set_active(bool active) {
    state.active = active;
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

    ESP_LOGI(TAG, "Hiding status screen");

    if (state.update_timer) {
        lv_timer_del(state.update_timer);
        state.update_timer = nullptr;
    }

    heatpump_temps_close_cb_t cb = state.on_close;
    // Widgets are owned by the panel; forget them all.
    state = {};

    if (cb) {
        cb();
    }
}

bool heatpump_temps_is_shown(void) {
    return state.shown;
}
