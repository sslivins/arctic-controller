/*
 * Arctic Heat Pump Controller
 * Settings - Heat output & COP screen
 *
 * Main screen: current estimate, assumed flow / loop fluid (saved as they
 * change), and the two temperature sensors. Tapping a sensor opens a
 * full-screen editor (source, Modbus TCP address, advanced decoding, Test),
 * and the editor's text fields open a full-screen keyboard entry.
 *
 * Nothing here blocks: sensor tests are queued on the ext_temp worker and
 * polled with an lv_timer.
 * Portrait mode: 720x1280
 */
#include "settings_perf_screen.h"
#include "settings_menu.h"
#include "settings_common.h"
#include "ui_keyboard.h"
#include "i18n/i18n.h"
#include "fonts/fonts.h"
#include "../app_preferences.h"
#include "../ext_temp_sensors.h"
#include "../heatpump_controller.h"
#include "../perf_settings.h"
#include "../perf_source.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const char* TAG = "perf_screen";

#define COLOR_PERF_WARN     lv_color_hex(0xffc107)
#define COLOR_PERF_ROW      lv_color_hex(0x25335a)
#define COLOR_PERF_ROW_PRESSED lv_color_hex(0x2e3f6d)
#define COLOR_SEG_BG        lv_color_hex(0x1a1f26)
#define COLOR_SEG_BORDER    lv_color_hex(0x30363d)
#define COLOR_ON_ACCENT     lv_color_hex(0x0d1117)
#define COLOR_BTN_SECONDARY lv_color_hex(0x3d4f6f)

static constexpr uint32_t kSaveDelayMs = 800;
static constexpr uint32_t kTestPollMs = 200;
static constexpr uint32_t kTestTimeoutMs = 10000;
static constexpr uint32_t kBrowsePollMs = 200;
static constexpr uint32_t kBrowseTimeoutMs = 26000;
static constexpr uint16_t kFlowStepX10 = 10;

enum class Field : uint8_t { Host, Port, UnitId, Register };

struct Editor {
    lv_obj_t* root;
    int slot;
    perf::SensorConfig ed;
    perf::SensorConfig tested;  // what the running test was started with

    lv_obj_t* source_row;
    lv_obj_t* source_value;
    lv_obj_t* source_list;
    lv_obj_t* modbus_group;
    lv_obj_t* host_col;
    lv_obj_t* trio;
    lv_obj_t* host_val;
    lv_obj_t* port_val;
    lv_obj_t* unit_val;
    lv_obj_t* reg_col;
    lv_obj_t* reg_val;
    lv_obj_t* adv_summary;
    lv_obj_t* adv_chevron;
    lv_obj_t* adv_body;
    bool adv_open;
    lv_obj_t* regtype_btn[2];
    lv_obj_t* vt_roller;
    lv_obj_t* scale_roller;
    lv_obj_t* nr_roller;
    lv_obj_t* nr_row;
    lv_obj_t* browse_btn;
    lv_obj_t* browse_lbl;
    lv_obj_t* discover_btn;
    lv_obj_t* discover_lbl;
    lv_obj_t* discover_card;
    lv_obj_t* discover_list;
    lv_obj_t* browse_card;
    lv_obj_t* browse_list;
    lv_obj_t* browse_note;
    lv_obj_t* test_btn;
    lv_obj_t* test_lbl;
    lv_obj_t* result_card;
    lv_obj_t* result_title;
    lv_obj_t* result_body;
    lv_obj_t* error_lbl;

    uint32_t ticket;
    lv_timer_t* test_timer;
    uint32_t test_started_ms;
    lv_timer_t* browse_timer;
    lv_timer_t* discover_timer;
    uint32_t browse_started_ms;
    uint32_t discover_started_ms;
    ext_temp::BrowseResult* browse_result;
    volatile bool browse_done;
    volatile bool browse_running;
    ext_temp::DiscoverResult* discover_result;
    volatile bool discover_done;
    volatile bool discover_running;
    bool manual_mode;
};

struct TextEntry {
    lv_obj_t* root;
    lv_obj_t* ta;
    lv_obj_t* err;
    Field field;
};

typedef struct {
    bool visible;
    perf_screen_config_t config;

    lv_obj_t* screen;
    lv_obj_t* content;
    lv_timer_t* live_timer;
    lv_timer_t* save_timer;
    bool save_pending;

    // Working copy of the flow / fluid settings (saved after a short delay).
    uint16_t flow_x10;
    arctic::LoopFluid fluid;
    uint8_t glycol_pct;

    lv_obj_t* est_dot;
    lv_obj_t* est_src;
    lv_obj_t* est_out;
    lv_obj_t* est_cop;

    lv_obj_t* flow_val;
    lv_obj_t* fluid_roller;
    lv_obj_t* glycol_block;
    lv_obj_t* glycol_val;
    lv_obj_t* glycol_slider;

    lv_obj_t* sensor_sub[perf::kSlotCount];
    lv_obj_t* sensor_val[perf::kSlotCount];

    Editor editor;
    TextEntry entry;
} perf_screen_state_t;

static perf_screen_state_t s_state = {};

struct BrowseJob {
    volatile bool running;
    volatile bool done;
    char host[perf::kHostMax];
    uint16_t port;
    ext_temp::BrowseResult* result;
};

static BrowseJob s_browse_job = {};

struct DiscoverJob {
    ext_temp::DiscoverResult* result;
    volatile bool running;
    volatile bool done;
};

static DiscoverJob s_discover_job = {};

// ============================================================================
// Forward Declarations
// ============================================================================

static void create_header(void);
static void create_content(void);
static void back_btn_cb(lv_event_t* e);
static void refresh_live(void);
static void open_editor(int slot);
static void close_editor(void);
static void open_text_entry(Field f);
static void close_text_entry(void);

static void free_browse_result_if_idle(void)
{
    if (!s_browse_job.running && s_browse_job.result) {
        heap_caps_free(s_browse_job.result);
        s_browse_job.result = NULL;
    }
}

static void free_discover_result_if_idle(void)
{
    if (!s_discover_job.running && s_discover_job.result) {
        heap_caps_free(s_discover_job.result);
        s_discover_job.result = NULL;
    }
}

static void clear_editor_bacnet_identity()
{
    perf::SensorConfig& s = s_state.editor.ed;
    s.bacnet_device_known = false;
    s.bacnet_device_instance = perf::kBacnetDeviceWildcard;
    s.bacnet_object_name[0] = '\0';
    s.rom_known = false;
    memset(s.rom, 0, sizeof(s.rom));
}

// ============================================================================
// Small helpers
// ============================================================================

static const char* slot_name(int slot)
{
    return i18n_get(slot == 0 ? STR_PERF_SUPPLY : STR_PERF_RETURN);
}

static lv_obj_t* make_plain(lv_obj_t* parent)
{
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(o, 0, LV_PART_MAIN);
    disable_scrolling(o);
    return o;
}

static lv_obj_t* make_row(lv_obj_t* parent)
{
    lv_obj_t* row = make_plain(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

static lv_obj_t* make_column(lv_obj_t* parent, int32_t gap)
{
    lv_obj_t* col = make_plain(parent);
    lv_obj_set_size(col, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, gap, LV_PART_MAIN);
    return col;
}

static lv_obj_t* make_card(lv_obj_t* parent, const char* title)
{
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    disable_scrolling(card);

    lv_obj_t* t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(t, COLOR_ACCENT, LV_PART_MAIN);
    return card;
}

static lv_obj_t* make_label(lv_obj_t* parent, const char* text, const lv_font_t* font, lv_color_t color)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, color, LV_PART_MAIN);
    return l;
}

static void style_roller(lv_obj_t* roller, int32_t w)
{
    lv_obj_set_width(roller, w);
    lv_obj_set_style_bg_color(roller, COLOR_SEG_BG, LV_PART_MAIN);
    lv_obj_set_style_text_color(roller, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_border_color(roller, COLOR_SEG_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(roller, 1, LV_PART_MAIN);
    lv_obj_set_style_text_font(roller, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(roller, 18, LV_PART_MAIN);
    lv_obj_set_style_radius(roller, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(roller, COLOR_ACCENT, LV_PART_SELECTED);
    lv_obj_set_style_text_color(roller, COLOR_ON_ACCENT, LV_PART_SELECTED);
    // Height depends on the font and line space, so set it last.
    lv_roller_set_visible_row_count(roller, 3);
}

static lv_obj_t* make_button(lv_obj_t* parent, const char* text, const char* tag, lv_color_t bg,
                             lv_color_t fg, lv_event_cb_t cb, void* user)
{
    lv_obj_t* btn = lv_btn_create(parent);
    lv_obj_set_style_bg_color(btn, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_user_data(btn, (void*)tag);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, fg, LV_PART_MAIN);
    lv_obj_center(lbl);
    return btn;
}

// Two-option segmented control. The buttons are returned in `btn_out`.
static void make_segment(lv_obj_t* parent, const char* a, const char* b, const char* tag_a,
                         const char* tag_b, lv_event_cb_t cb, lv_obj_t* btn_out[2])
{
    lv_obj_t* seg = lv_obj_create(parent);
    lv_obj_set_size(seg, LV_PCT(100), 72);
    lv_obj_set_style_bg_color(seg, COLOR_SEG_BG, LV_PART_MAIN);
    lv_obj_set_style_border_color(seg, COLOR_SEG_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(seg, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(seg, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(seg, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_column(seg, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    disable_scrolling(seg);

    const char* texts[2] = {a, b};
    const char* tags[2] = {tag_a, tag_b};
    for (int i = 0; i < 2; ++i) {
        lv_obj_t* btn = make_button(seg, texts[i], tags[i], COLOR_ACCENT, COLOR_TEXT_DIM, cb,
                                    (void*)(intptr_t)i);
        lv_obj_set_height(btn, LV_PCT(100));
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);
        // The default theme paints CHECKED buttons in its secondary (red) colour.
        lv_obj_set_style_bg_color(btn, COLOR_ACCENT, LV_STATE_CHECKED);
        btn_out[i] = btn;
    }
}

static void make_segment3(lv_obj_t* parent, const char* a, const char* b, const char* c,
                          const char* tag_a, const char* tag_b, const char* tag_c,
                          lv_event_cb_t cb, lv_obj_t* btn_out[3])
{
    lv_obj_t* seg = lv_obj_create(parent);
    lv_obj_set_size(seg, LV_PCT(100), 72);
    lv_obj_set_style_bg_color(seg, COLOR_SEG_BG, LV_PART_MAIN);
    lv_obj_set_style_border_color(seg, COLOR_SEG_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(seg, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(seg, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(seg, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_column(seg, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    disable_scrolling(seg);

    const char* texts[3] = {a, b, c};
    const char* tags[3] = {tag_a, tag_b, tag_c};
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* btn = make_button(seg, texts[i], tags[i], COLOR_ACCENT, COLOR_TEXT_DIM, cb,
                                    (void*)(intptr_t)i);
        lv_obj_set_height(btn, LV_PCT(100));
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, COLOR_ACCENT, LV_STATE_CHECKED);
        btn_out[i] = btn;
    }
}

static void set_segment(lv_obj_t* const btns[2], int selected)
{
    for (int i = 0; i < 2; ++i) {
        bool on = i == selected;
        lv_obj_set_style_bg_opa(btns[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_t* lbl = lv_obj_get_child(btns[i], 0);
        lv_obj_set_style_text_color(lbl, on ? COLOR_ON_ACCENT : COLOR_TEXT_DIM, LV_PART_MAIN);
        if (on) {
            lv_obj_add_state(btns[i], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(btns[i], LV_STATE_CHECKED);
        }
    }
}

static void set_segment3(lv_obj_t* const btns[3], int selected)
{
    for (int i = 0; i < 3; ++i) {
        bool on = i == selected;
        lv_obj_set_style_bg_opa(btns[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_t* lbl = lv_obj_get_child(btns[i], 0);
        lv_obj_set_style_text_color(lbl, on ? COLOR_ON_ACCENT : COLOR_TEXT_DIM, LV_PART_MAIN);
        if (on) {
            lv_obj_add_state(btns[i], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(btns[i], LV_STATE_CHECKED);
        }
    }
}

static const char* source_label(perf::SensorSource source)
{
    switch (source) {
        case perf::SensorSource::ModbusTcp: return i18n_get(STR_PERF_OPT_MODBUS);
        case perf::SensorSource::BacnetIp: return i18n_get(STR_PERF_OPT_BACNET);
        case perf::SensorSource::HeatPump:
        default: return i18n_get(STR_PERF_OPT_HEAT_PUMP);
    }
}

static lv_obj_t* make_picker_row(lv_obj_t* parent, const char* text, const char* tag,
                                 bool selected, lv_event_cb_t cb, void* user)
{
    lv_obj_t* row = lv_btn_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 68);
    lv_obj_set_style_bg_color(row, selected ? COLOR_ACCENT : COLOR_PERF_ROW, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, COLOR_PERF_ROW_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_user_data(row, (void*)tag);
    if (cb) lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user);
    lv_obj_t* lbl = make_label(row, text, FONT_NORMAL, selected ? COLOR_ON_ACCENT : COLOR_TEXT);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    return row;
}

static float to_display_temp(float c)
{
    return app_prefs_get_temp_unit() == TEMP_UNIT_FAHRENHEIT ? c * 9.0f / 5.0f + 32.0f : c;
}

static void format_flow(uint16_t x10, char* buf, size_t size)
{
    if (x10 % 10 == 0) {
        snprintf(buf, size, "%u L/min", (unsigned)(x10 / 10));
    } else {
        snprintf(buf, size, "%u.%u L/min", (unsigned)(x10 / 10), (unsigned)(x10 % 10));
    }
}

static void format_sensor_sub(const perf::SensorConfig& c, char* buf, size_t size)
{
    if (c.source == perf::SensorSource::HeatPump) {
        snprintf(buf, size, "%s", i18n_get(STR_PERF_SUB_HEAT_PUMP));
    } else if (c.source == perf::SensorSource::BacnetIp) {
        const char* typ = c.bacnet_type == perf::BacnetObjectType::AnalogValue ? "AV" : "AI";
        if (c.bacnet_instance == perf::kBacnetUnsetInstance) {
            snprintf(buf, size, "BACnet/IP \xC2\xB7 %s:%u \xC2\xB7 --", c.host,
                     (unsigned)c.port);
        } else if (c.bacnet_object_name[0]) {
            snprintf(buf, size, "BACnet/IP \xC2\xB7 %s:%u \xC2\xB7 %s", c.host,
                     (unsigned)c.port, c.bacnet_object_name);
        } else {
            snprintf(buf, size, "BACnet/IP \xC2\xB7 %s:%u \xC2\xB7 %s %lu", c.host,
                     (unsigned)c.port, typ, (unsigned long)c.bacnet_instance);
        }
    } else {
        snprintf(buf, size, "Modbus \xC2\xB7 %s \xC2\xB7 reg %u", c.host, (unsigned)c.address);
    }
}

static const char* value_type_text(perf::ValueType t)
{
    switch (t) {
        case perf::ValueType::Uint16: return i18n_get(STR_PERF_VT_UINT16);
        case perf::ValueType::Float32: return i18n_get(STR_PERF_VT_FLOAT);
        case perf::ValueType::Float32Swapped: return i18n_get(STR_PERF_VT_FLOAT_SWAPPED);
        case perf::ValueType::Int16:
        default: return i18n_get(STR_PERF_VT_INT16);
    }
}

static const char* const kScaleText[] = {"\xC3\x97 1", "\xC3\x97 0.1", "\xC3\x97 0.01", "\xC3\x97 0.001"};

static bool is_float(perf::ValueType t)
{
    return t == perf::ValueType::Float32 || t == perf::ValueType::Float32Swapped;
}

static bool sensor_differs(const perf::SensorConfig& a, const perf::SensorConfig& b)
{
    return a.source != b.source || strncmp(a.host, b.host, perf::kHostMax) != 0 ||
           a.port != b.port || a.unit_id != b.unit_id || a.address != b.address ||
           a.reg_type != b.reg_type || a.bacnet_type != b.bacnet_type ||
           a.bacnet_instance != b.bacnet_instance || a.value_type != b.value_type ||
           a.scale_exp != b.scale_exp || a.no_reading != b.no_reading ||
           a.bacnet_device_known != b.bacnet_device_known ||
           (a.bacnet_device_known && a.bacnet_device_instance != b.bacnet_device_instance) ||
           strncmp(a.bacnet_object_name, b.bacnet_object_name, sizeof(a.bacnet_object_name)) != 0;
}

// ============================================================================
// Flow / fluid persistence
// ============================================================================

static void save_general(void)
{
    s_state.save_pending = false;
    if (s_state.save_timer) lv_timer_pause(s_state.save_timer);

    perf::Settings cur = ext_temp::settings();
    if (cur.flow_lpm_x10 == s_state.flow_x10 && cur.fluid == s_state.fluid &&
        cur.glycol_pct == s_state.glycol_pct) {
        return;
    }
    cur.flow_lpm_x10 = s_state.flow_x10;
    cur.fluid = s_state.fluid;
    cur.glycol_pct = s_state.glycol_pct;
    const bool edited[perf::kSlotCount] = {false, false};
    bool saved = false;
    perf::Invalid v = ext_temp::apply_settings(cur, edited, &saved);
    if (v != perf::Invalid::None || !saved) {
        ESP_LOGW(TAG, "Couldn't save flow/fluid (%s, saved=%d)", perf::invalid_name(v), saved);
    }
}

static void save_timer_cb(lv_timer_t* t)
{
    (void)t;
    save_general();
}

static void schedule_save(void)
{
    s_state.save_pending = true;
    if (!s_state.save_timer) return;
    lv_timer_reset(s_state.save_timer);
    lv_timer_resume(s_state.save_timer);
}

// ============================================================================
// Public API
// ============================================================================

void perf_screen_create(const perf_screen_config_t* config)
{
    if (s_state.visible) {
        ESP_LOGW(TAG, "Heat output & COP screen already visible");
        return;
    }

    ESP_LOGI(TAG, "Creating Heat output & COP screen");

    memset(&s_state, 0, sizeof(s_state));
    if (config) s_state.config = *config;

    perf::Settings cur = ext_temp::settings();
    s_state.flow_x10 = cur.flow_lpm_x10;
    s_state.fluid = cur.fluid;
    s_state.glycol_pct = cur.glycol_pct;

    s_state.screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_state.screen, COLOR_BG, LV_PART_MAIN);
    disable_scrolling(s_state.screen);

    create_header();
    create_content();

    s_state.live_timer = lv_timer_create([](lv_timer_t*) { refresh_live(); }, 1000, NULL);
    s_state.save_timer = lv_timer_create(save_timer_cb, kSaveDelayMs, NULL);
    lv_timer_pause(s_state.save_timer);
    refresh_live();

    lv_screen_load_anim(s_state.screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
    s_state.visible = true;
}

void perf_screen_close(void)
{
    if (!s_state.visible) return;

    ESP_LOGI(TAG, "Closing Heat output & COP screen");

    if (s_state.save_pending) save_general();
    if (s_state.editor.test_timer) lv_timer_delete(s_state.editor.test_timer);
    if (s_state.editor.browse_timer) lv_timer_delete(s_state.editor.browse_timer);
    if (s_state.editor.discover_timer) lv_timer_delete(s_state.editor.discover_timer);
    if (s_state.live_timer) lv_timer_delete(s_state.live_timer);
    if (s_state.save_timer) lv_timer_delete(s_state.save_timer);
    free_browse_result_if_idle();
    free_discover_result_if_idle();

    // The editor and keyboard entry are children of the screen, which is
    // auto-deleted by the next screen load.
    s_state = {};
}

bool perf_screen_is_visible(void)
{
    return s_state.visible;
}

// ============================================================================
// Main screen
// ============================================================================

static void create_header(void)
{
    lv_display_t* disp = lv_display_get_default();
    int32_t header_height = lv_display_get_vertical_resolution(disp) * HEADER_HEIGHT_PCT / 100;

    lv_obj_t* header = lv_obj_create(s_state.screen);
    lv_obj_set_size(header, LV_PCT(100), header_height);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, COLOR_HEADER, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(header, 15, LV_PART_MAIN);
    disable_scrolling(header);

    lv_obj_t* back_btn = lv_btn_create(header);
    lv_obj_set_size(back_btn, 50, 50);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(back_btn, COLOR_BTN_SECONDARY, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(back_btn, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(back_btn, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_border_opa(back_btn, LV_OPA_50, LV_PART_MAIN);
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_user_data(back_btn, (void*)"perf_back");

    lv_obj_t* back_icon = lv_label_create(back_btn);
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_icon, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(back_icon, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_center(back_icon);

    lv_obj_t* title = make_label(header, i18n_get(STR_SETTINGS_PERF), UI_FONT_HEADER, COLOR_TEXT);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
}

static void flow_step_cb(lv_event_t* e)
{
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    uint16_t v = s_state.flow_x10;
    if (dir > 0) {
        v = (uint16_t)((v / kFlowStepX10 + 1) * kFlowStepX10);
    } else {
        v = (uint16_t)(v % kFlowStepX10 ? (v / kFlowStepX10) * kFlowStepX10 : v - kFlowStepX10);
    }
    if (v < perf::kFlowMinX10) v = perf::kFlowMinX10;
    if (v > perf::kFlowMaxX10) v = perf::kFlowMaxX10;
    if (v == s_state.flow_x10) return;
    s_state.flow_x10 = v;
    char buf[24];
    format_flow(v, buf, sizeof(buf));
    lv_label_set_text(s_state.flow_val, buf);
    schedule_save();
}

static void update_glycol_enabled(void)
{
    bool water = s_state.fluid == arctic::LoopFluid::Water;
    lv_obj_set_style_opa(s_state.glycol_block, water ? LV_OPA_40 : LV_OPA_COVER, LV_PART_MAIN);
    if (water) {
        lv_obj_add_state(s_state.glycol_slider, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(s_state.glycol_slider, LV_STATE_DISABLED);
    }
}

static void fluid_roller_cb(lv_event_t* e)
{
    lv_obj_t* roller = (lv_obj_t*)lv_event_get_target(e);
    s_state.fluid = static_cast<arctic::LoopFluid>(lv_roller_get_selected(roller));
    update_glycol_enabled();
    save_general();
}

static void glycol_slider_cb(lv_event_t* e)
{
    int32_t pct = lv_slider_get_value(s_state.glycol_slider) * perf::kGlycolStep;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", (int)pct);
    lv_label_set_text(s_state.glycol_val, buf);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        s_state.glycol_pct = (uint8_t)pct;
        save_general();
    }
}

static void sensor_row_cb(lv_event_t* e)
{
    open_editor((int)(intptr_t)lv_event_get_user_data(e));
}

static void create_estimate_card(lv_obj_t* parent)
{
    lv_obj_t* card = make_card(parent, i18n_get(STR_PERF_CURRENT));

    lv_obj_t* src_row = make_plain(card);
    lv_obj_set_size(src_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(src_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(src_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(src_row, 12, LV_PART_MAIN);

    s_state.est_dot = lv_obj_create(src_row);
    lv_obj_set_size(s_state.est_dot, 18, 18);
    lv_obj_set_style_radius(s_state.est_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_state.est_dot, 0, LV_PART_MAIN);
    disable_scrolling(s_state.est_dot);

    s_state.est_src = make_label(src_row, "", FONT_NORMAL, COLOR_TEXT_DIM);
    lv_obj_set_flex_grow(s_state.est_src, 1);
    lv_label_set_long_mode(s_state.est_src, LV_LABEL_LONG_WRAP);
    lv_obj_set_user_data(s_state.est_src, (void*)"perf_source");

    lv_obj_t* vals = make_row(card);
    s_state.est_out = make_label(vals, "", FONT_NORMAL, COLOR_TEXT);
    lv_obj_set_user_data(s_state.est_out, (void*)"perf_output");
    s_state.est_cop = make_label(vals, "", FONT_NORMAL, COLOR_TEXT);
    lv_obj_set_user_data(s_state.est_cop, (void*)"perf_cop_value");
}

static void create_flow_card(lv_obj_t* parent)
{
    lv_obj_t* card = make_card(parent, i18n_get(STR_PERF_FLOW_FLUID));
    lv_obj_set_style_pad_row(card, 24, LV_PART_MAIN);

    // Assumed flow rate: [-] 40 L/min [+]
    lv_obj_t* flow_row = make_row(card);
    lv_obj_t* flow_lbl = make_label(flow_row, i18n_get(STR_PERF_FLOW), FONT_NORMAL, COLOR_TEXT);
    lv_obj_set_flex_grow(flow_lbl, 1);
    lv_label_set_long_mode(flow_lbl, LV_LABEL_LONG_WRAP);

    lv_obj_t* stepper = make_plain(flow_row);
    lv_obj_set_size(stepper, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(stepper, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(stepper, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(stepper, 8, LV_PART_MAIN);

    lv_obj_t* minus = make_button(stepper, LV_SYMBOL_MINUS, "perf_flow_minus", COLOR_BTN_SECONDARY,
                                  COLOR_TEXT, flow_step_cb, (void*)(intptr_t)-1);
    lv_obj_set_size(minus, 64, 60);
    lv_obj_add_event_cb(minus, flow_step_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void*)(intptr_t)-1);

    char buf[24];
    format_flow(s_state.flow_x10, buf, sizeof(buf));
    s_state.flow_val = make_label(stepper, buf, FONT_NORMAL, COLOR_ACCENT);
    lv_obj_set_width(s_state.flow_val, 150);
    lv_obj_set_style_text_align(s_state.flow_val, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_user_data(s_state.flow_val, (void*)"perf_flow_value");

    lv_obj_t* plus = make_button(stepper, LV_SYMBOL_PLUS, "perf_flow_plus", COLOR_BTN_SECONDARY,
                                 COLOR_TEXT, flow_step_cb, (void*)(intptr_t)1);
    lv_obj_set_size(plus, 64, 60);
    lv_obj_add_event_cb(plus, flow_step_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void*)(intptr_t)1);

    // Loop fluid roller
    lv_obj_t* fluid_row = make_row(card);
    lv_obj_t* fluid_lbl = make_label(fluid_row, i18n_get(STR_PERF_FLUID), FONT_NORMAL, COLOR_TEXT);
    lv_obj_set_flex_grow(fluid_lbl, 1);
    lv_label_set_long_mode(fluid_lbl, LV_LABEL_LONG_WRAP);

    char opts[128];
    snprintf(opts, sizeof(opts), "%s\n%s\n%s", i18n_get(STR_PERF_FLUID_WATER),
             i18n_get(STR_PERF_FLUID_PG), i18n_get(STR_PERF_FLUID_EG));
    s_state.fluid_roller = lv_roller_create(fluid_row);
    lv_roller_set_options(s_state.fluid_roller, opts, LV_ROLLER_MODE_NORMAL);
    style_roller(s_state.fluid_roller, 340);
    lv_roller_set_selected(s_state.fluid_roller, static_cast<uint32_t>(s_state.fluid), LV_ANIM_OFF);
    lv_obj_set_user_data(s_state.fluid_roller, (void*)"perf_fluid");
    lv_obj_add_event_cb(s_state.fluid_roller, fluid_roller_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Glycol concentration (dimmed and disabled for water)
    s_state.glycol_block = make_column(card, 14);
    lv_obj_t* g_title = make_row(s_state.glycol_block);
    lv_obj_t* g_lbl = make_label(g_title, i18n_get(STR_PERF_GLYCOL), FONT_NORMAL, COLOR_TEXT);
    lv_obj_set_flex_grow(g_lbl, 1);
    lv_label_set_long_mode(g_lbl, LV_LABEL_LONG_WRAP);
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)s_state.glycol_pct);
    s_state.glycol_val = make_label(g_title, buf, FONT_NORMAL, COLOR_ACCENT);
    lv_obj_set_user_data(s_state.glycol_val, (void*)"perf_glycol_value");

    lv_obj_t* g_row = make_row(s_state.glycol_block);
    lv_obj_set_style_pad_column(g_row, 25, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(g_row, 10, LV_PART_MAIN);
    make_label(g_row, "0%", FONT_NORMAL, COLOR_TEXT_DIM);
    s_state.glycol_slider = lv_slider_create(g_row);
    lv_obj_set_flex_grow(s_state.glycol_slider, 1);
    lv_obj_set_height(s_state.glycol_slider, 12);
    lv_slider_set_range(s_state.glycol_slider, 0, arctic::kGlycolPctMax / perf::kGlycolStep);
    lv_slider_set_value(s_state.glycol_slider, s_state.glycol_pct / perf::kGlycolStep, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_state.glycol_slider, COLOR_SEG_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_state.glycol_slider, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_state.glycol_slider, COLOR_ACCENT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_state.glycol_slider, 12, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s_state.glycol_slider, 20);
    lv_obj_set_user_data(s_state.glycol_slider, (void*)"perf_glycol");
    lv_obj_add_event_cb(s_state.glycol_slider, glycol_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_state.glycol_slider, glycol_slider_cb, LV_EVENT_RELEASED, NULL);
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)arctic::kGlycolPctMax);
    make_label(g_row, buf, FONT_NORMAL, COLOR_TEXT_DIM);

    update_glycol_enabled();
}

static void create_sensors_card(lv_obj_t* parent)
{
    lv_obj_t* card = make_card(parent, i18n_get(STR_PERF_SENSORS));
    lv_obj_t* hint = make_label(card, i18n_get(STR_PERF_SENSORS_HINT), UI_FONT_SMALL, COLOR_TEXT_DIM);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);

    static const char* const kRowTags[perf::kSlotCount] = {"perf_sensor_supply", "perf_sensor_return"};
    static const char* const kValTags[perf::kSlotCount] = {"perf_sensor_supply_value",
                                                           "perf_sensor_return_value"};
    for (int i = 0; i < perf::kSlotCount; ++i) {
        lv_obj_t* row = lv_obj_create(card);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(row, COLOR_PERF_ROW, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, COLOR_PERF_ROW_PRESSED, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 20, LV_PART_MAIN);
        lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        disable_scrolling(row);
        lv_obj_set_user_data(row, (void*)kRowTags[i]);
        lv_obj_add_event_cb(row, sensor_row_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        lv_obj_t* text = make_column(row, 10);
        lv_obj_set_width(text, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(text, 1);
        lv_obj_remove_flag(text, LV_OBJ_FLAG_CLICKABLE);
        make_label(text, slot_name(i), FONT_NORMAL, COLOR_TEXT);
        s_state.sensor_sub[i] = make_label(text, "", UI_FONT_SMALL, COLOR_TEXT_DIM);
        lv_obj_set_width(s_state.sensor_sub[i], LV_PCT(100));
        lv_label_set_long_mode(s_state.sensor_sub[i], LV_LABEL_LONG_DOT);

        s_state.sensor_val[i] = make_label(row, "--", FONT_NORMAL, COLOR_ACCENT);
        lv_obj_set_user_data(s_state.sensor_val[i], (void*)kValTags[i]);
        make_label(row, LV_SYMBOL_RIGHT, FONT_NORMAL, COLOR_TEXT_DIM);
    }
}

static void create_content(void)
{
    lv_display_t* disp = lv_display_get_default();
    int32_t screen_height = lv_display_get_vertical_resolution(disp);
    int32_t header_height = screen_height * HEADER_HEIGHT_PCT / 100;

    s_state.content = lv_obj_create(s_state.screen);
    lv_obj_set_size(s_state.content, LV_PCT(100), screen_height - header_height);
    lv_obj_set_pos(s_state.content, 0, header_height);
    lv_obj_set_style_bg_opa(s_state.content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_state.content, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_state.content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_state.content, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_state.content, 20, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_state.content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_state.content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_state.content, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_user_data(s_state.content, (void*)"perf_content");

    create_estimate_card(s_state.content);
    create_flow_card(s_state.content);
    create_sensors_card(s_state.content);
}

static void refresh_live(void)
{
    if (!s_state.screen) return;
    arctic::HeatPumpState hp = arctic::getState();

    // Where the estimate's temperatures come from
    lv_color_t dot = COLOR_TEXT_DIM;
    lv_color_t text = COLOR_TEXT_DIM;
    string_id_t src = STR_PERF_SRC_HEAT_PUMP;
    if (hp.perf_fallback) {
        dot = text = COLOR_PERF_WARN;
        src = STR_PERF_SRC_FALLBACK;
    } else if (hp.perf_pending) {
        src = STR_PERF_SRC_PENDING;
    } else if (hp.perf_external && hp.perf_settling) {
        src = STR_PERF_SRC_SETTLING;
    } else if (hp.perf_external) {
        dot = COLOR_SUCCESS;
        text = COLOR_TEXT;
        src = STR_PERF_SRC_EXTERNAL;
    }
    lv_obj_set_style_bg_color(s_state.est_dot, dot, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_state.est_src, text, LV_PART_MAIN);
    lv_label_set_text(s_state.est_src, i18n_get(src));

    bool cooling = hp.cop_valid ? hp.thermal_w < 0 : hp.working_mode == arctic::WorkingMode::COOLING;
    char buf[64];
    if (hp.cop_valid) {
        int32_t w = hp.thermal_w < 0 ? -hp.thermal_w : hp.thermal_w;
        long tenths = (long)((w + 50) / 100);
        snprintf(buf, sizeof(buf), "%s  %ld.%ld kW",
                 i18n_get(cooling ? STR_PERF_COOLING_OUTPUT : STR_PERF_HEATING_OUTPUT),
                 tenths / 10, tenths % 10);
    } else {
        snprintf(buf, sizeof(buf), "%s  --",
                 i18n_get(cooling ? STR_PERF_COOLING_OUTPUT : STR_PERF_HEATING_OUTPUT));
    }
    lv_label_set_text(s_state.est_out, buf);

    if (hp.cop_valid) {
        unsigned tenths = (hp.cop_x100 + 5) / 10;
        snprintf(buf, sizeof(buf), "COP  %u.%u", tenths / 10, tenths % 10);
    } else {
        snprintf(buf, sizeof(buf), "COP  --");
    }
    lv_label_set_text(s_state.est_cop, buf);

    // Sensor rows
    perf::Settings cfg = ext_temp::settings();
    ext_temp::SlotStatus st[perf::kSlotCount];
    ext_temp::slot_status(st);
    for (int i = 0; i < perf::kSlotCount; ++i) {
        const perf::SensorConfig& c = cfg.sensors[i];
        format_sensor_sub(c, buf, sizeof(buf));
        lv_label_set_text(s_state.sensor_sub[i], buf);

        lv_color_t color = COLOR_ACCENT;
        if (c.source == perf::SensorSource::HeatPump) {
            if (hp.connected) {
                int16_t t = i == 0 ? hp.outlet_water_temp : hp.inlet_water_temp;
                snprintf(buf, sizeof(buf), "%d %s", app_prefs_convert_temp(t), app_prefs_temp_unit_str());
            } else {
                snprintf(buf, sizeof(buf), "--");
                color = COLOR_TEXT_DIM;
            }
        } else if (st[i].error == ext_temp::Error::None && st[i].has_reading) {
            snprintf(buf, sizeof(buf), "%.2f %s", (double)to_display_temp(st[i].celsius),
                     app_prefs_temp_unit_str());
        } else {
            snprintf(buf, sizeof(buf), "--");
            color = st[i].error == ext_temp::Error::NotRead ? COLOR_TEXT_DIM : COLOR_PERF_WARN;
        }
        lv_label_set_text(s_state.sensor_val[i], buf);
        lv_obj_set_style_text_color(s_state.sensor_val[i], color, LV_PART_MAIN);
    }
}

static void back_btn_cb(lv_event_t* e)
{
    (void)e;
    void (*on_back)(void) = s_state.config.on_back;
    if (on_back) {
        on_back();
    } else {
        settings_menu_show();
    }
    perf_screen_close();
}

// ============================================================================
// Sensor editor
// ============================================================================

static void editor_refresh(void)
{
    Editor& ed = s_state.editor;
    const perf::SensorConfig& c = ed.ed;
    bool modbus = c.source == perf::SensorSource::ModbusTcp;
    bool bacnet = c.source == perf::SensorSource::BacnetIp;
    if (ed.source_value) lv_label_set_text(ed.source_value, source_label(c.source));
    if (ed.source_list) {
        for (uint32_t i = 0; i < 3; ++i) {
            lv_obj_t* row = lv_obj_get_child(ed.source_list, i);
            if (!row) continue;
            bool on = (i == 0 && c.source == perf::SensorSource::HeatPump) ||
                      (i == 1 && c.source == perf::SensorSource::ModbusTcp) ||
                      (i == 2 && c.source == perf::SensorSource::BacnetIp);
            lv_obj_set_style_bg_color(row, on ? COLOR_ACCENT : COLOR_PERF_ROW, LV_PART_MAIN);
            lv_obj_t* lbl = lv_obj_get_child(row, 0);
            if (lbl) lv_obj_set_style_text_color(lbl, on ? COLOR_ON_ACCENT : COLOR_TEXT,
                                                 LV_PART_MAIN);
        }
    }
    if (modbus || bacnet) {
        lv_obj_remove_flag(ed.modbus_group, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ed.modbus_group, LV_OBJ_FLAG_HIDDEN);
    }

    char buf[96];
    lv_label_set_text(ed.host_val, c.host);
    snprintf(buf, sizeof(buf), "%u", (unsigned)c.port);
    lv_label_set_text(ed.port_val, buf);
    snprintf(buf, sizeof(buf), "%u", (unsigned)c.unit_id);
    lv_label_set_text(ed.unit_val, buf);
    lv_obj_t* unit_col = lv_obj_get_parent(lv_obj_get_parent(ed.unit_val));
    if (bacnet) {
        lv_obj_add_flag(unit_col, LV_OBJ_FLAG_HIDDEN);
        if (ed.browse_btn) lv_obj_remove_flag(ed.browse_btn, LV_OBJ_FLAG_HIDDEN);
        if (ed.discover_btn) lv_obj_remove_flag(ed.discover_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(unit_col, LV_OBJ_FLAG_HIDDEN);
        if (ed.browse_btn) lv_obj_add_flag(ed.browse_btn, LV_OBJ_FLAG_HIDDEN);
        if (ed.discover_btn) lv_obj_add_flag(ed.discover_btn, LV_OBJ_FLAG_HIDDEN);
        if (ed.discover_card) lv_obj_add_flag(ed.discover_card, LV_OBJ_FLAG_HIDDEN);
        if (ed.browse_card) lv_obj_add_flag(ed.browse_card, LV_OBJ_FLAG_HIDDEN);
    }
    if (bacnet) {
        if (c.bacnet_instance == perf::kBacnetUnsetInstance) {
            snprintf(buf, sizeof(buf), "--");
        } else {
            snprintf(buf, sizeof(buf), "%lu", (unsigned long)c.bacnet_instance);
        }
    } else {
        snprintf(buf, sizeof(buf), "%u", (unsigned)c.address);
    }
    lv_label_set_text(ed.reg_val, buf);
    if (ed.reg_col) {
        lv_obj_t* lbl = lv_obj_get_child(ed.reg_col, 0);
        if (lbl) lv_label_set_text(lbl, i18n_get(bacnet ? STR_PERF_OBJECT : STR_PERF_REGISTER));
    }

    set_segment(ed.regtype_btn, bacnet ? (c.bacnet_type == perf::BacnetObjectType::AnalogValue ? 1 : 0)
                                       : (c.reg_type == perf::RegisterType::Holding ? 1 : 0));
    lv_roller_set_selected(ed.vt_roller, static_cast<uint32_t>(c.value_type), LV_ANIM_OFF);
    lv_roller_set_selected(ed.scale_roller, (uint32_t)(-c.scale_exp), LV_ANIM_OFF);
    lv_roller_set_selected(ed.nr_roller, static_cast<uint32_t>(c.no_reading), LV_ANIM_OFF);
    bool flt = is_float(c.value_type);
    lv_obj_set_style_opa(ed.nr_row, flt ? LV_OPA_40 : LV_OPA_COVER, LV_PART_MAIN);
    if (flt) {
        lv_obj_add_state(ed.nr_roller, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(ed.nr_roller, LV_STATE_DISABLED);
    }

    if (bacnet) {
        snprintf(buf, sizeof(buf), "%s \xC2\xB7 %s", i18n_get(STR_PERF_BACNET_OBJECT),
                 c.bacnet_type == perf::BacnetObjectType::AnalogValue ? "AV" : "AI");
    } else {
        snprintf(buf, sizeof(buf), "%s \xC2\xB7 %s \xC2\xB7 %s",
                 i18n_get(c.reg_type == perf::RegisterType::Holding ? STR_PERF_REG_HOLDING
                                                                    : STR_PERF_REG_INPUT),
                 value_type_text(c.value_type), kScaleText[-c.scale_exp]);
    }
    lv_label_set_text(ed.adv_summary, buf);
    lv_label_set_text(ed.adv_chevron, ed.adv_open ? LV_SYMBOL_UP : LV_SYMBOL_DOWN);
    if (ed.adv_open) {
        lv_obj_remove_flag(ed.adv_body, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ed.adv_body, LV_OBJ_FLAG_HIDDEN);
    }
}

static void editor_hide_result(void)
{
    Editor& ed = s_state.editor;
    if (ed.result_card) lv_obj_add_flag(ed.result_card, LV_OBJ_FLAG_HIDDEN);
    if (ed.error_lbl) lv_obj_add_flag(ed.error_lbl, LV_OBJ_FLAG_HIDDEN);
}

// Any change to the configuration makes an earlier test result stale.
static void editor_changed(void)
{
    editor_hide_result();
    editor_refresh();
}

static void editor_show_error(const char* msg)
{
    Editor& ed = s_state.editor;
    lv_label_set_text(ed.error_lbl, msg);
    lv_obj_remove_flag(ed.error_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_view_recursive(ed.error_lbl, LV_ANIM_ON);
}

static void show_result(bool ok, const char* title, const char* body)
{
    Editor& ed = s_state.editor;
    lv_color_t border = ok ? COLOR_SUCCESS : COLOR_PERF_WARN;
    lv_obj_set_style_bg_color(ed.result_card, ok ? lv_color_hex(0x173a2a) : lv_color_hex(0x3d2e14),
                              LV_PART_MAIN);
    lv_obj_set_style_border_color(ed.result_card, border, LV_PART_MAIN);
    lv_obj_set_style_text_color(ed.result_title, border, LV_PART_MAIN);
    lv_label_set_text(ed.result_title, title);
    lv_label_set_text(ed.result_body, body);
    lv_obj_set_user_data(ed.result_card, (void*)(ok ? "perf_test_ok" : "perf_test_fail"));
    lv_obj_remove_flag(ed.result_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_view_recursive(ed.result_card, LV_ANIM_ON);
}

static void set_testing(bool testing)
{
    Editor& ed = s_state.editor;
    lv_label_set_text(ed.test_lbl, i18n_get(testing ? STR_PERF_TESTING : STR_PERF_TEST));
    if (testing) {
        lv_obj_add_state(ed.test_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(ed.test_btn, LV_STATE_DISABLED);
    }
}

static void render_test_result(const ext_temp::TestResult& r, bool reg_type_switched)
{
    char title[64];
    char body[256];
    using ext_temp::Error;
    namespace tx = perf::thermux;

    if (r.error == Error::None) {
        snprintf(title, sizeof(title), LV_SYMBOL_OK "  %.2f %s", (double)to_display_temp(r.celsius),
                 app_prefs_temp_unit_str());
        int n = 0;
        if (r.thermux) {
            n = snprintf(body, sizeof(body), i18n_get(STR_PERF_THERMUX_CHANNEL), r.channel);
            if (r.thermux_age_s != 0xFFFF && n > 0 && (size_t)n < sizeof(body)) {
                n += snprintf(body + n, sizeof(body) - n, " \xC2\xB7 ");
                n += snprintf(body + n, sizeof(body) - n, i18n_get(STR_PERF_READ_AGO),
                              (unsigned)r.thermux_age_s);
            }
            if (r.rom_valid && n > 0 && (size_t)n < sizeof(body)) {
                snprintf(body + n, sizeof(body) - n, "\n");
                n += 1;
                n += snprintf(body + n, sizeof(body) - n, i18n_get(STR_PERF_SENSOR_ID), r.rom_hex);
            }
        } else {
            n = snprintf(body, sizeof(body), "%s", i18n_get(STR_PERF_TEST_OK));
        }
        if (reg_type_switched && n > 0 && (size_t)n < sizeof(body)) {
            n += snprintf(body + n, sizeof(body) - n, "\n");
            snprintf(body + n, sizeof(body) - n, i18n_get(STR_PERF_REG_SWITCHED),
                     i18n_get(r.reg_type == perf::RegisterType::Holding ? STR_PERF_REG_HOLDING
                                                                        : STR_PERF_REG_INPUT));
        }
        show_result(true, title, body);
        return;
    }

    snprintf(title, sizeof(title), LV_SYMBOL_WARNING "  %s", i18n_get(STR_PERF_TEST_FAIL));
    auto status = static_cast<tx::ChannelStatus>(r.thermux_status);
    if (r.thermux && status == tx::ChannelStatus::Unassigned) {
        snprintf(body, sizeof(body), i18n_get(STR_PERF_ERR_UNASSIGNED), r.channel);
    } else if (r.thermux && status != tx::ChannelStatus::Ok) {
        snprintf(body, sizeof(body), i18n_get(STR_PERF_ERR_CHANNEL), r.channel);
    } else {
        switch (r.error) {
            case Error::Resolve: snprintf(body, sizeof(body), "%s", i18n_get(STR_PERF_ERR_RESOLVE)); break;
            case Error::Connect: snprintf(body, sizeof(body), "%s", i18n_get(STR_PERF_ERR_CONNECT)); break;
            case Error::Timeout: snprintf(body, sizeof(body), "%s", i18n_get(STR_PERF_ERR_TIMEOUT)); break;
            case Error::Exception:
                snprintf(body, sizeof(body), i18n_get(STR_PERF_ERR_EXCEPTION), (unsigned)r.exception);
                break;
            case Error::NoReading: snprintf(body, sizeof(body), "%s", i18n_get(STR_PERF_ERR_NO_READING)); break;
            case Error::OutOfRange: snprintf(body, sizeof(body), "%s", i18n_get(STR_PERF_ERR_OUT_OF_RANGE)); break;
            case Error::Busy: snprintf(body, sizeof(body), "%s", i18n_get(STR_PERF_ERR_BUSY)); break;
            default: snprintf(body, sizeof(body), "%s", i18n_get(STR_PERF_ERR_PROTOCOL)); break;
        }
    }
    show_result(false, title, body);
}

// The fields a test result describes. The rest of the editor stays usable while
// a test runs, so a result only applies if these haven't changed meanwhile.
static bool same_test_target(const perf::SensorConfig& a, const perf::SensorConfig& b)
{
    if (a.source != b.source || a.port != b.port || strncmp(a.host, b.host, perf::kHostMax) != 0) {
        return false;
    }
    if (a.source == perf::SensorSource::BacnetIp) {
        return a.bacnet_type == b.bacnet_type && a.bacnet_instance == b.bacnet_instance;
    }
    return a.unit_id == b.unit_id && a.address == b.address && a.value_type == b.value_type;
}

static void test_poll_cb(lv_timer_t* t)
{
    (void)t;
    Editor& ed = s_state.editor;
    ext_temp::TestResult r = {};
    bool done = ext_temp::test_result(ed.ticket, &r);
    if (!done && lv_tick_elaps(ed.test_started_ms) < kTestTimeoutMs) return;
    if (!done) {
        r = {};
        r.error = ext_temp::Error::Timeout;
    }
    lv_timer_delete(ed.test_timer);
    ed.test_timer = NULL;
    ed.ticket = 0;
    set_testing(false);
    bool current = same_test_target(ed.tested, ed.ed);
    // The test found the value in the other register table: use that.
    bool switched = done && current && r.reg_type != ed.ed.reg_type;
    if (switched) {
        ed.ed.reg_type = r.reg_type;
        editor_refresh();
    }
    if (done && current && r.error == ext_temp::Error::None &&
        ed.ed.source == perf::SensorSource::BacnetIp && r.bacnet_device_known) {
        ed.ed.bacnet_device_known = true;
        ed.ed.bacnet_device_instance = r.bacnet_device_instance;
        strlcpy(ed.ed.bacnet_object_name, r.object_name, sizeof(ed.ed.bacnet_object_name));
        if (r.rom_valid) {
            for (size_t i = 0; i < perf::kRomLen; ++i) {
                char tmp[3] = {r.rom_hex[2 * i], r.rom_hex[2 * i + 1], 0};
                ed.ed.rom[i] = (uint8_t)strtoul(tmp, NULL, 16);
            }
            ed.ed.rom_known = true;
        } else {
            ed.ed.rom_known = false;
            memset(ed.ed.rom, 0, sizeof(ed.ed.rom));
        }
    }
    render_test_result(r, switched);
}

static void test_btn_cb(lv_event_t* e)
{
    (void)e;
    Editor& ed = s_state.editor;
    if (ed.test_timer) return;
    editor_hide_result();
    if (!perf::host_valid(ed.ed.host)) {
        editor_show_error(i18n_get(STR_PERF_INVALID_HOST));
        return;
    }
    if (perf::validate_sensor(ed.ed) == perf::Invalid::Register) {
        editor_show_error(i18n_get(STR_PERF_INVALID_REGISTER));
        return;
    }
    perf::SensorConfig cfg = ed.ed;
    ed.tested = cfg;
    ed.ticket = ext_temp::test_start(cfg);
    if (ed.ticket == 0) {
        ext_temp::TestResult r = {};
        r.error = ext_temp::Error::Busy;
        render_test_result(r, false);
        return;
    }
    ed.test_started_ms = lv_tick_get();
    ed.test_timer = lv_timer_create(test_poll_cb, kTestPollMs, NULL);
    set_testing(true);
}

static void set_browsing(bool browsing)
{
    Editor& ed = s_state.editor;
    if (!ed.browse_btn || !ed.browse_lbl) return;
    lv_label_set_text(ed.browse_lbl, i18n_get(browsing ? STR_PERF_FINDING_SENSORS : STR_PERF_FIND_SENSORS));
    if (browsing) {
        lv_obj_add_state(ed.browse_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(ed.browse_btn, LV_STATE_DISABLED);
    }
}

static void browse_btn_cb(lv_event_t* e);

static void set_discovering(bool searching)
{
    Editor& ed = s_state.editor;
    if (!ed.discover_btn || !ed.discover_lbl) return;
    lv_label_set_text(ed.discover_lbl, searching ? i18n_get(STR_PERF_SEARCHING)
                                                  : i18n_get(STR_PERF_FIND_DEVICES));
    if (searching) {
        lv_obj_add_state(ed.discover_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(ed.discover_btn, LV_STATE_DISABLED);
    }
}

static void discover_task(void*)
{
    if (!s_discover_job.result) {
        s_discover_job.done = true;
        s_discover_job.running = false;
        vTaskDelete(NULL);
    }
    bool ok = ext_temp::discover_blocking(s_discover_job.result, 12000);
    if (!ok && s_discover_job.result->error == ext_temp::Error::None) {
        s_discover_job.result->error = ext_temp::Error::Timeout;
    }
    s_discover_job.done = true;
    s_discover_job.running = false;
    vTaskDelete(NULL);
}

static void discover_pick_cb(lv_event_t* e)
{
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (!s_discover_job.result || idx < 0 || (size_t)idx >= s_discover_job.result->count) return;
    const ext_temp::DiscoverDevice& d = s_discover_job.result->devices[idx];
    Editor& ed = s_state.editor;
    ed.ed.source = perf::SensorSource::BacnetIp;
    strlcpy(ed.ed.host, d.host, sizeof(ed.ed.host));
    ed.ed.port = d.port;
    ed.ed.bacnet_device_known = true;
    ed.ed.bacnet_device_instance = d.device_instance;
    ed.ed.rom_known = false;
    memset(ed.ed.rom, 0, sizeof(ed.ed.rom));
    if (ed.discover_card) lv_obj_add_flag(ed.discover_card, LV_OBJ_FLAG_HIDDEN);
    editor_changed();
    browse_btn_cb(nullptr);
}

static void manual_cb(lv_event_t*)
{
    Editor& ed = s_state.editor;
    ed.manual_mode = true;
    if (ed.discover_card) lv_obj_add_flag(ed.discover_card, LV_OBJ_FLAG_HIDDEN);
    if (ed.host_col) lv_obj_scroll_to_view_recursive(ed.host_col, LV_ANIM_ON);
}

static void render_discover_result(const ext_temp::DiscoverResult& r)
{
    Editor& ed = s_state.editor;
    if (!ed.discover_card || !ed.discover_list) return;
    lv_obj_clean(ed.discover_list);
    if (r.error != ext_temp::Error::None) {
        const char* msg = r.error == ext_temp::Error::Timeout ? i18n_get(STR_PERF_ERR_TIMEOUT)
                                                              : i18n_get(STR_PERF_ERR_CONNECT);
        make_label(ed.discover_list, msg, FONT_NORMAL, COLOR_PERF_WARN);
        lv_obj_remove_flag(ed.discover_card, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    make_label(ed.discover_list,
               r.count ? i18n_get(STR_PERF_DEVICES_FOUND) : i18n_get(STR_PERF_NO_BACNET_DEVICES),
               FONT_NORMAL, r.count ? COLOR_TEXT : COLOR_TEXT_DIM);
    char line[96];
    static const char* kDeviceTags[16] = {
        "perf_bacnet_device_0", "perf_bacnet_device_1", "perf_bacnet_device_2",
        "perf_bacnet_device_3", "perf_bacnet_device_4", "perf_bacnet_device_5",
        "perf_bacnet_device_6", "perf_bacnet_device_7", "perf_bacnet_device_8",
        "perf_bacnet_device_9", "perf_bacnet_device_10", "perf_bacnet_device_11",
        "perf_bacnet_device_12", "perf_bacnet_device_13", "perf_bacnet_device_14",
        "perf_bacnet_device_15",
    };
    for (size_t i = 0; i < r.count; ++i) {
        const auto& d = r.devices[i];
        snprintf(line, sizeof(line), "%s \xC2\xB7 %s",
                 d.device_name[0] ? d.device_name : i18n_get(STR_PERF_BACNET_DEVICE), d.host);
        make_picker_row(ed.discover_list, line, i < 16 ? kDeviceTags[i] : "perf_bacnet_device", false, discover_pick_cb,
                        (void*)(intptr_t)i);
    }
    make_picker_row(ed.discover_list, i18n_get(STR_PERF_BACNET_MANUAL), "perf_bacnet_manual", false,
                    manual_cb, NULL);
    lv_obj_remove_flag(ed.discover_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_view_recursive(ed.discover_card, LV_ANIM_ON);
}

static void discover_poll_cb(lv_timer_t* t)
{
    (void)t;
    Editor& ed = s_state.editor;
    if (!s_discover_job.done && lv_tick_elaps(ed.discover_started_ms) < 14000) return;
    if (!s_discover_job.done && s_discover_job.result) {
        s_discover_job.result->error = ext_temp::Error::Timeout;
        s_discover_job.done = true;
    }
    lv_timer_delete(ed.discover_timer);
    ed.discover_timer = NULL;
    set_discovering(false);
    if (s_discover_job.result) render_discover_result(*s_discover_job.result);
}

static void discover_btn_cb(lv_event_t*)
{
    Editor& ed = s_state.editor;
    if (ed.ed.source != perf::SensorSource::BacnetIp || ed.discover_timer) return;
    if (s_discover_job.running) {
        editor_show_error(i18n_get(STR_PERF_ERR_BUSY));
        return;
    }
    free_discover_result_if_idle();
    s_discover_job.result = (ext_temp::DiscoverResult*)heap_caps_calloc(
        1, sizeof(ext_temp::DiscoverResult), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_discover_job.result) {
        editor_show_error(i18n_get(STR_PERF_SAVE_FAILED));
        return;
    }
    s_discover_job.done = false;
    s_discover_job.running = true;
    if (xTaskCreateWithCaps(discover_task, "perf_disc", 8192, NULL, 5, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        s_discover_job.running = false;
        free_discover_result_if_idle();
        editor_show_error(i18n_get(STR_PERF_SAVE_FAILED));
        return;
    }
    if (ed.discover_card) lv_obj_add_flag(ed.discover_card, LV_OBJ_FLAG_HIDDEN);
    ed.discover_started_ms = lv_tick_get();
    ed.discover_timer = lv_timer_create(discover_poll_cb, kBrowsePollMs, NULL);
    set_discovering(true);
}

static void browse_task(void*)
{
    if (!s_browse_job.result) {
        s_browse_job.done = true;
        s_browse_job.running = false;
        vTaskDelete(NULL);
        return;
    }
    bool ok = ext_temp::browse_blocking(s_browse_job.host, s_browse_job.port, s_browse_job.result,
                                        kBrowseTimeoutMs - 500);
    if (!ok && s_browse_job.result->error == ext_temp::Error::None) {
        s_browse_job.result->error = ext_temp::Error::Timeout;
    }
    s_browse_job.done = true;
    s_browse_job.running = false;
    vTaskDelete(NULL);
}

static void browse_pick_cb(lv_event_t* e)
{
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (!s_browse_job.result || idx < 0 || (size_t)idx >= s_browse_job.result->count) return;
    const ext_temp::BrowseSensor& s = s_browse_job.result->sensors[idx];
    Editor& ed = s_state.editor;
    ed.ed.source = perf::SensorSource::BacnetIp;
    ed.ed.bacnet_type = s.object_type;
    ed.ed.bacnet_instance = s.object_instance;
    ed.ed.bacnet_device_known = s_browse_job.result->device_instance_known;
    ed.ed.bacnet_device_instance = s_browse_job.result->device_instance;
    strlcpy(ed.ed.bacnet_object_name, s.object_name, sizeof(ed.ed.bacnet_object_name));
    if (s.rom_valid) {
        for (size_t i = 0; i < perf::kRomLen; ++i) {
            char tmp[3] = {s.rom_hex[2 * i], s.rom_hex[2 * i + 1], 0};
            ed.ed.rom[i] = (uint8_t)strtoul(tmp, NULL, 16);
        }
        ed.ed.rom_known = true;
    } else {
        ed.ed.rom_known = false;
        memset(ed.ed.rom, 0, sizeof(ed.ed.rom));
    }
    if (ed.browse_card) lv_obj_add_flag(ed.browse_card, LV_OBJ_FLAG_HIDDEN);
    editor_changed();
}

static void render_browse_result(const ext_temp::BrowseResult& r)
{
    Editor& ed = s_state.editor;
    if (!ed.browse_card || !ed.browse_list || !ed.browse_note) return;
    lv_obj_clean(ed.browse_list);
    lv_label_set_text(ed.browse_note, "");
    lv_obj_add_flag(ed.browse_note, LV_OBJ_FLAG_HIDDEN);

    if (r.error != ext_temp::Error::None) {
        const char* msg = i18n_get(STR_PERF_ERR_PROTOCOL);
        switch (r.error) {
            case ext_temp::Error::Resolve: msg = i18n_get(STR_PERF_ERR_RESOLVE); break;
            case ext_temp::Error::Connect: msg = i18n_get(STR_PERF_ERR_CONNECT); break;
            case ext_temp::Error::Timeout: msg = i18n_get(STR_PERF_ERR_TIMEOUT); break;
            case ext_temp::Error::Busy: msg = i18n_get(STR_PERF_ERR_BUSY); break;
            default: break;
        }
        make_label(ed.browse_list, msg, FONT_NORMAL, COLOR_PERF_WARN);
        lv_obj_remove_flag(ed.browse_card, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (r.count == 0) {
        make_label(ed.browse_list, i18n_get(STR_PERF_NO_BACNET_SENSORS), FONT_NORMAL, COLOR_TEXT_DIM);
        lv_obj_remove_flag(ed.browse_card, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    char line[96];
    for (size_t i = 0; i < r.count; ++i) {
        const ext_temp::BrowseSensor& s = r.sensors[i];
        bool selected = ed.ed.source == perf::SensorSource::BacnetIp &&
                        ed.ed.bacnet_type == s.object_type &&
                        ed.ed.bacnet_instance == s.object_instance;
        const char* name = s.object_name[0] ? s.object_name : "--";
        snprintf(line, sizeof(line), "%s \xE2\x80\x94 %.1f %s", name,
                 (double)to_display_temp(s.celsius), app_prefs_temp_unit_str());
        make_picker_row(ed.browse_list, line, "perf_bacnet_sensor", selected, browse_pick_cb,
                        (void*)(intptr_t)i);
    }

    if (r.truncated) {
        char note[192];
        snprintf(note, sizeof(note), i18n_get(STR_PERF_BACNET_SHOWING_FIRST), (unsigned)r.count);
        size_t n = strlen(note);
        if (n + 2 < sizeof(note)) {
            note[n++] = '\n';
            note[n] = '\0';
            snprintf(note + n, sizeof(note) - n, "%s", i18n_get(STR_PERF_BACNET_TOO_MANY));
        }
        lv_label_set_text(ed.browse_note, note);
        lv_obj_remove_flag(ed.browse_note, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(ed.browse_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_view_recursive(ed.browse_card, LV_ANIM_ON);
}

static void browse_poll_cb(lv_timer_t* t)
{
    (void)t;
    Editor& ed = s_state.editor;
    if (!s_browse_job.done && lv_tick_elaps(ed.browse_started_ms) < kBrowseTimeoutMs) return;
    if (!s_browse_job.done && s_browse_job.result) {
        s_browse_job.result->error = ext_temp::Error::Timeout;
        s_browse_job.done = true;
    }
    lv_timer_delete(ed.browse_timer);
    ed.browse_timer = NULL;
    set_browsing(false);
    if (s_browse_job.result) render_browse_result(*s_browse_job.result);
}

static void browse_btn_cb(lv_event_t* e)
{
    (void)e;
    Editor& ed = s_state.editor;
    if (ed.ed.source != perf::SensorSource::BacnetIp || ed.browse_timer) return;
    editor_hide_result();
    if (!perf::host_valid(ed.ed.host)) {
        editor_show_error(i18n_get(STR_PERF_INVALID_HOST));
        return;
    }
    if (s_browse_job.running) {
        editor_show_error(i18n_get(STR_PERF_ERR_BUSY));
        return;
    }
    free_browse_result_if_idle();
    s_browse_job.result = (ext_temp::BrowseResult*)heap_caps_calloc(
        1, sizeof(ext_temp::BrowseResult), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_browse_job.result) {
        editor_show_error(i18n_get(STR_PERF_SAVE_FAILED));
        return;
    }
    snprintf(s_browse_job.host, sizeof(s_browse_job.host), "%s", ed.ed.host);
    s_browse_job.port = ed.ed.port;
    s_browse_job.done = false;
    s_browse_job.running = true;
    if (xTaskCreateWithCaps(browse_task, "perf_browse", 8192, NULL, 5, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        s_browse_job.running = false;
        free_browse_result_if_idle();
        editor_show_error(i18n_get(STR_PERF_SAVE_FAILED));
        return;
    }
    lv_obj_add_flag(ed.browse_card, LV_OBJ_FLAG_HIDDEN);
    ed.browse_started_ms = lv_tick_get();
    ed.browse_timer = lv_timer_create(browse_poll_cb, kBrowsePollMs, NULL);
    set_browsing(true);
}

static void source_row_cb(lv_event_t* e)
{
    (void)e;
    Editor& ed = s_state.editor;
    if (!ed.source_list) return;
    if (lv_obj_has_flag(ed.source_list, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(ed.source_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_scroll_to_view_recursive(ed.source_list, LV_ANIM_ON);
    } else {
        lv_obj_add_flag(ed.source_list, LV_OBJ_FLAG_HIDDEN);
    }
}

static void source_pick_cb(lv_event_t* e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    perf::SensorConfig& s = s_state.editor.ed;
    perf::SensorSource old = s.source;
    s.source = idx == 2 ? perf::SensorSource::BacnetIp
                        : (idx == 1 ? perf::SensorSource::ModbusTcp : perf::SensorSource::HeatPump);
    if (old != s.source) {
        clear_editor_bacnet_identity();
        if (s.source == perf::SensorSource::BacnetIp) {
            s.port = perf::kDefaultBacnetPort;
        } else if (s.source == perf::SensorSource::ModbusTcp) {
            s.port = perf::kDefaultPort;
        }
    }
    if (s_state.editor.source_list) lv_obj_add_flag(s_state.editor.source_list, LV_OBJ_FLAG_HIDDEN);
    editor_changed();
    if (s.source == perf::SensorSource::BacnetIp && s.host[0] == '\0') {
        discover_btn_cb(nullptr);
    }
}

static void regtype_seg_cb(lv_event_t* e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_state.editor.ed.source == perf::SensorSource::BacnetIp) {
        s_state.editor.ed.bacnet_type =
            idx == 1 ? perf::BacnetObjectType::AnalogValue : perf::BacnetObjectType::AnalogInput;
        clear_editor_bacnet_identity();
    } else {
        s_state.editor.ed.reg_type = idx == 1 ? perf::RegisterType::Holding : perf::RegisterType::Input;
    }
    editor_changed();
}

static void adv_toggle_cb(lv_event_t* e)
{
    (void)e;
    s_state.editor.adv_open = !s_state.editor.adv_open;
    editor_refresh();
}

static void adv_roller_cb(lv_event_t* e)
{
    Editor& ed = s_state.editor;
    lv_obj_t* roller = (lv_obj_t*)lv_event_get_target(e);
    uint32_t sel = lv_roller_get_selected(roller);
    if (roller == ed.vt_roller) {
        ed.ed.value_type = static_cast<perf::ValueType>(sel);
    } else if (roller == ed.scale_roller) {
        ed.ed.scale_exp = (int8_t)-(int)sel;
    } else if (roller == ed.nr_roller) {
        ed.ed.no_reading = static_cast<perf::NoReading>(sel);
    }
    editor_changed();
}

static void field_click_cb(lv_event_t* e)
{
    open_text_entry(static_cast<Field>((intptr_t)lv_event_get_user_data(e)));
}

static void editor_cancel_cb(lv_event_t* e)
{
    (void)e;
    close_editor();
}

static void editor_save_cb(lv_event_t* e)
{
    (void)e;
    Editor& ed = s_state.editor;
    editor_hide_result();

    perf::Settings cur = ext_temp::settings();
    perf::SensorConfig next = ed.ed;
    bool edited[perf::kSlotCount] = {false, false};
    edited[ed.slot] = sensor_differs(cur.sensors[ed.slot], next);
    if (!edited[ed.slot]) {
        close_editor();
        return;
    }
    cur.sensors[ed.slot] = next;
    // Flow / fluid changes still waiting on the save timer go in with this save.
    cur.flow_lpm_x10 = s_state.flow_x10;
    cur.fluid = s_state.fluid;
    cur.glycol_pct = s_state.glycol_pct;

    bool saved = false;
    perf::Invalid v = ext_temp::apply_settings(cur, edited, &saved);
    switch (v) {
        case perf::Invalid::None:
            if (!saved) {
                editor_show_error(i18n_get(STR_PERF_SAVE_FAILED));
                return;
            }
            s_state.save_pending = false;
            if (s_state.save_timer) lv_timer_pause(s_state.save_timer);
            close_editor();
            refresh_live();
            return;
        case perf::Invalid::Host:
            editor_show_error(i18n_get(STR_PERF_INVALID_HOST));
            return;
        case perf::Invalid::Register:
            editor_show_error(i18n_get(STR_PERF_INVALID_REGISTER));
            return;
        default:
            ESP_LOGW(TAG, "Sensor settings rejected: %s", perf::invalid_name(v));
            editor_show_error(i18n_get(STR_PERF_SAVE_FAILED));
            return;
    }
}

static lv_obj_t* make_field(lv_obj_t* parent, const char* title, const char* tag, Field f, lv_obj_t** val_out)
{
    lv_obj_t* col = make_column(parent, 8);
    make_label(col, title, UI_FONT_SMALL, COLOR_TEXT_DIM);

    lv_obj_t* box = lv_obj_create(col);
    lv_obj_set_size(box, LV_PCT(100), 64);
    lv_obj_set_style_bg_color(box, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(box, lv_color_hex(0xdde6f0), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(box, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(box, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(box, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(box, 0, LV_PART_MAIN);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    disable_scrolling(box);
    lv_obj_set_user_data(box, (void*)tag);
    lv_obj_add_event_cb(box, field_click_cb, LV_EVENT_CLICKED, (void*)(intptr_t)f);

    lv_obj_t* val = make_label(box, "", FONT_NORMAL, lv_color_hex(0x111111));
    lv_obj_set_width(val, LV_PCT(100));
    lv_label_set_long_mode(val, LV_LABEL_LONG_DOT);
    lv_obj_align(val, LV_ALIGN_LEFT_MID, 0, 0);
    *val_out = val;
    return col;
}

static lv_obj_t* make_roller_row(lv_obj_t* parent, const char* title, const char* opts, const char* tag,
                                 lv_obj_t** roller_out)
{
    lv_obj_t* row = make_row(parent);
    lv_obj_t* lbl = make_label(row, title, FONT_NORMAL, COLOR_TEXT);
    lv_obj_set_flex_grow(lbl, 1);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);

    lv_obj_t* roller = lv_roller_create(row);
    lv_roller_set_options(roller, opts, LV_ROLLER_MODE_NORMAL);
    style_roller(roller, 360);
    lv_obj_set_user_data(roller, (void*)tag);
    lv_obj_add_event_cb(roller, adv_roller_cb, LV_EVENT_VALUE_CHANGED, NULL);
    *roller_out = roller;
    return row;
}

static void open_editor(int slot)
{
    if (!s_state.screen || s_state.editor.root) return;
    Editor& ed = s_state.editor;
    ed = {};
    ed.slot = slot;
    ed.ed = ext_temp::settings().sensors[slot];
    if (ed.ed.port == 0) ed.ed.port = perf::kDefaultPort;

    lv_display_t* disp = lv_display_get_default();
    int32_t screen_h = lv_display_get_vertical_resolution(disp);
    int32_t header_h = screen_h * HEADER_HEIGHT_PCT / 100;
    const int32_t bar_h = 110;

    ed.root = lv_obj_create(s_state.screen);
    lv_obj_set_size(ed.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(ed.root, 0, 0);
    lv_obj_set_style_bg_color(ed.root, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ed.root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ed.root, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(ed.root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ed.root, 0, LV_PART_MAIN);
    disable_scrolling(ed.root);
    lv_obj_set_user_data(ed.root, (void*)"perf_editor");

    lv_obj_t* header = lv_obj_create(ed.root);
    lv_obj_set_size(header, LV_PCT(100), header_h);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, COLOR_HEADER, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN);
    disable_scrolling(header);
    lv_obj_t* title = make_label(header, slot_name(slot), UI_FONT_HEADER, COLOR_TEXT);
    lv_obj_center(title);

    lv_obj_t* body = lv_obj_create(ed.root);
    lv_obj_set_size(body, LV_PCT(100), screen_h - header_h - bar_h);
    lv_obj_set_pos(body, 0, header_h);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(body, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(body, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(body, 30, LV_PART_MAIN);
    lv_obj_set_style_pad_row(body, 20, LV_PART_MAIN);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);

    // Source
    lv_obj_t* src_col = make_column(body, 10);
    make_label(src_col, i18n_get(STR_PERF_SOURCE), UI_FONT_SMALL, COLOR_TEXT_DIM);
    ed.source_row = lv_btn_create(src_col);
    lv_obj_set_size(ed.source_row, LV_PCT(100), 72);
    lv_obj_set_style_bg_color(ed.source_row, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ed.source_row, COLOR_PERF_ROW_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(ed.source_row, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(ed.source_row, COLOR_SEG_BORDER, LV_PART_MAIN);
    lv_obj_set_style_radius(ed.source_row, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(ed.source_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(ed.source_row, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(ed.source_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ed.source_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_user_data(ed.source_row, (void*)"perf_source_row");
    lv_obj_add_event_cb(ed.source_row, source_row_cb, LV_EVENT_CLICKED, NULL);
    ed.source_value = make_label(ed.source_row, "", FONT_NORMAL, COLOR_TEXT);
    lv_obj_set_flex_grow(ed.source_value, 1);
    lv_label_set_long_mode(ed.source_value, LV_LABEL_LONG_DOT);
    make_label(ed.source_row, LV_SYMBOL_RIGHT, FONT_NORMAL, COLOR_TEXT_DIM);

    ed.source_list = make_column(src_col, 8);
    lv_obj_set_user_data(ed.source_list, (void*)"perf_source_list");
    make_picker_row(ed.source_list, i18n_get(STR_PERF_OPT_HEAT_PUMP), "perf_src_heat_pump",
                    ed.ed.source == perf::SensorSource::HeatPump, source_pick_cb, (void*)0);
    make_picker_row(ed.source_list, i18n_get(STR_PERF_OPT_MODBUS), "perf_src_modbus",
                    ed.ed.source == perf::SensorSource::ModbusTcp, source_pick_cb, (void*)1);
    make_picker_row(ed.source_list, i18n_get(STR_PERF_OPT_BACNET), "perf_src_bacnet",
                    ed.ed.source == perf::SensorSource::BacnetIp, source_pick_cb, (void*)2);
    lv_obj_add_flag(ed.source_list, LV_OBJ_FLAG_HIDDEN);

    // Modbus TCP details
    ed.modbus_group = make_column(body, 20);
    ed.host_col = make_field(ed.modbus_group, i18n_get(STR_PERF_HOST), "perf_host", Field::Host,
                             &ed.host_val);

    lv_obj_t* trio = make_plain(ed.modbus_group);
    ed.trio = trio;
    lv_obj_set_size(trio, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(trio, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(trio, 30, LV_PART_MAIN);
    lv_obj_t* c1 = make_field(trio, i18n_get(STR_PERF_PORT), "perf_port", Field::Port, &ed.port_val);
    lv_obj_t* c2 = make_field(trio, i18n_get(STR_PERF_UNIT_ID), "perf_unit_id", Field::UnitId, &ed.unit_val);
    lv_obj_t* c3 = make_field(trio, i18n_get(STR_PERF_REGISTER), "perf_register", Field::Register, &ed.reg_val);
    ed.reg_col = c3;
    lv_obj_t* cols[3] = {c1, c2, c3};
    for (lv_obj_t* c : cols) {
        lv_obj_set_width(c, 0);
        lv_obj_set_flex_grow(c, 1);
    }

    ed.browse_btn = make_button(ed.modbus_group, i18n_get(STR_PERF_FIND_SENSORS), "perf_bacnet_browse",
                                COLOR_BTN_SECONDARY, COLOR_TEXT, browse_btn_cb, NULL);
    lv_obj_set_size(ed.browse_btn, LV_PCT(100), 72);
    lv_obj_set_style_bg_opa(ed.browse_btn, LV_OPA_50, LV_STATE_DISABLED);
    ed.browse_lbl = lv_obj_get_child(ed.browse_btn, 0);

    ed.discover_btn = make_button(ed.modbus_group, i18n_get(STR_PERF_FIND_DEVICES),
                                  "perf_bacnet_discover", COLOR_BTN_SECONDARY, COLOR_TEXT,
                                  discover_btn_cb, NULL);
    lv_obj_set_size(ed.discover_btn, LV_PCT(100), 72);
    lv_obj_set_style_bg_opa(ed.discover_btn, LV_OPA_50, LV_STATE_DISABLED);
    ed.discover_lbl = lv_obj_get_child(ed.discover_btn, 0);

    ed.discover_card = lv_obj_create(ed.modbus_group);
    lv_obj_set_size(ed.discover_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(ed.discover_card, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_width(ed.discover_card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(ed.discover_card, COLOR_SEG_BORDER, LV_PART_MAIN);
    lv_obj_set_style_radius(ed.discover_card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ed.discover_card, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_row(ed.discover_card, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(ed.discover_card, LV_FLEX_FLOW_COLUMN);
    disable_scrolling(ed.discover_card);
    lv_obj_set_user_data(ed.discover_card, (void*)"perf_bacnet_devices");
    ed.discover_list = make_column(ed.discover_card, 8);
    lv_obj_set_user_data(ed.discover_list, (void*)"perf_bacnet_device_list");
    make_picker_row(ed.discover_list, i18n_get(STR_PERF_BACNET_MANUAL), "perf_bacnet_manual",
                    false, manual_cb, NULL);
    lv_obj_add_flag(ed.discover_card, LV_OBJ_FLAG_HIDDEN);

    ed.browse_card = lv_obj_create(ed.modbus_group);
    lv_obj_set_size(ed.browse_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(ed.browse_card, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_width(ed.browse_card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(ed.browse_card, COLOR_SEG_BORDER, LV_PART_MAIN);
    lv_obj_set_style_radius(ed.browse_card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ed.browse_card, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_row(ed.browse_card, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(ed.browse_card, LV_FLEX_FLOW_COLUMN);
    disable_scrolling(ed.browse_card);
    lv_obj_set_user_data(ed.browse_card, (void*)"perf_bacnet_results");
    ed.browse_list = make_column(ed.browse_card, 8);
    lv_obj_set_user_data(ed.browse_list, (void*)"perf_bacnet_list");
    ed.browse_note = make_label(ed.browse_card, "", UI_FONT_SMALL, COLOR_TEXT_DIM);
    lv_obj_set_width(ed.browse_note, LV_PCT(100));
    lv_label_set_long_mode(ed.browse_note, LV_LABEL_LONG_WRAP);
    lv_obj_set_user_data(ed.browse_note, (void*)"perf_bacnet_note");
    lv_obj_add_flag(ed.browse_note, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ed.browse_card, LV_OBJ_FLAG_HIDDEN);

    // Advanced (collapsed by default)
    lv_obj_t* adv = lv_obj_create(ed.modbus_group);
    lv_obj_set_size(adv, LV_PCT(100), 72);
    lv_obj_set_style_bg_color(adv, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_color(adv, COLOR_PERF_ROW_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(adv, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(adv, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(adv, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_column(adv, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(adv, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(adv, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(adv, LV_OBJ_FLAG_CLICKABLE);
    disable_scrolling(adv);
    lv_obj_set_user_data(adv, (void*)"perf_advanced");
    lv_obj_add_event_cb(adv, adv_toggle_cb, LV_EVENT_CLICKED, NULL);
    make_label(adv, i18n_get(STR_PERF_ADVANCED), FONT_NORMAL, COLOR_TEXT);
    ed.adv_summary = make_label(adv, "", UI_FONT_SMALL, COLOR_TEXT_DIM);
    lv_obj_set_flex_grow(ed.adv_summary, 1);
    lv_label_set_long_mode(ed.adv_summary, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(ed.adv_summary, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    ed.adv_chevron = make_label(adv, LV_SYMBOL_DOWN, UI_FONT_SMALL, COLOR_TEXT_DIM);

    ed.adv_body = make_column(ed.modbus_group, 20);
    lv_obj_t* rt_col = make_column(ed.adv_body, 10);
    make_label(rt_col, i18n_get(STR_PERF_REG_TYPE), UI_FONT_SMALL, COLOR_TEXT_DIM);
    char reg_a[48];
    char reg_b[48];
    snprintf(reg_a, sizeof(reg_a), "%s (04)", i18n_get(STR_PERF_REG_INPUT));
    snprintf(reg_b, sizeof(reg_b), "%s (03)", i18n_get(STR_PERF_REG_HOLDING));
    make_segment(rt_col, reg_a, reg_b, "perf_reg_input", "perf_reg_holding", regtype_seg_cb,
                 ed.regtype_btn);

    char opts[192];
    snprintf(opts, sizeof(opts), "%s\n%s\n%s\n%s", i18n_get(STR_PERF_VT_INT16),
             i18n_get(STR_PERF_VT_UINT16), i18n_get(STR_PERF_VT_FLOAT),
             i18n_get(STR_PERF_VT_FLOAT_SWAPPED));
    make_roller_row(ed.adv_body, i18n_get(STR_PERF_VALUE_TYPE), opts, "perf_value_type", &ed.vt_roller);
    snprintf(opts, sizeof(opts), "%s\n%s\n%s\n%s", kScaleText[0], kScaleText[1], kScaleText[2],
             kScaleText[3]);
    make_roller_row(ed.adv_body, i18n_get(STR_PERF_SCALE), opts, "perf_scale", &ed.scale_roller);
    snprintf(opts, sizeof(opts), "%s\n0x8000\n0x7FFF\n0xFFFF", i18n_get(STR_PERF_NO_READING_NONE));
    ed.nr_row = make_roller_row(ed.adv_body, i18n_get(STR_PERF_NO_READING), opts, "perf_no_reading",
                                &ed.nr_roller);

    // Test
    ed.test_btn = make_button(ed.modbus_group, i18n_get(STR_PERF_TEST), "perf_test", COLOR_BTN_SECONDARY,
                              COLOR_TEXT, test_btn_cb, NULL);
    lv_obj_set_size(ed.test_btn, LV_PCT(100), 72);
    lv_obj_set_style_bg_opa(ed.test_btn, LV_OPA_50, LV_STATE_DISABLED);
    ed.test_lbl = lv_obj_get_child(ed.test_btn, 0);

    ed.result_card = lv_obj_create(ed.modbus_group);
    lv_obj_set_size(ed.result_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(ed.result_card, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(ed.result_card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ed.result_card, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_row(ed.result_card, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(ed.result_card, LV_FLEX_FLOW_COLUMN);
    disable_scrolling(ed.result_card);
    ed.result_title = make_label(ed.result_card, "", FONT_LARGE, COLOR_TEXT);
    ed.result_body = make_label(ed.result_card, "", FONT_NORMAL, COLOR_TEXT);
    lv_obj_set_width(ed.result_body, LV_PCT(100));
    lv_label_set_long_mode(ed.result_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_user_data(ed.result_body, (void*)"perf_test_message");
    lv_obj_add_flag(ed.result_card, LV_OBJ_FLAG_HIDDEN);

    ed.error_lbl = make_label(body, "", FONT_NORMAL, COLOR_ERROR);
    lv_obj_set_width(ed.error_lbl, LV_PCT(100));
    lv_label_set_long_mode(ed.error_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_user_data(ed.error_lbl, (void*)"perf_editor_error");
    lv_obj_add_flag(ed.error_lbl, LV_OBJ_FLAG_HIDDEN);

    // Cancel / Save
    lv_obj_t* bar = lv_obj_create(ed.root);
    lv_obj_set_size(bar, LV_PCT(100), bar_h);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, COLOR_HEADER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bar, 30, LV_PART_MAIN);
    disable_scrolling(bar);

    lv_obj_t* cancel = make_button(bar, i18n_get(STR_CANCEL), "perf_editor_cancel", COLOR_HEADER,
                                   COLOR_TEXT, editor_cancel_cb, NULL);
    lv_obj_set_size(cancel, 300, 80);
    lv_obj_align(cancel, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_border_width(cancel, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(cancel, COLOR_TEXT_DIM, LV_PART_MAIN);

    lv_obj_t* save = make_button(bar, i18n_get(STR_SAVE), "perf_editor_save", COLOR_ACCENT, COLOR_BG,
                                 editor_save_cb, NULL);
    lv_obj_set_size(save, 300, 80);
    lv_obj_align(save, LV_ALIGN_RIGHT_MID, 0, 0);

    editor_refresh();
}

static void close_editor(void)
{
    Editor& ed = s_state.editor;
    close_text_entry();
    if (ed.test_timer) lv_timer_delete(ed.test_timer);
    if (ed.browse_timer) lv_timer_delete(ed.browse_timer);
    if (ed.discover_timer) lv_timer_delete(ed.discover_timer);
    if (ed.root) lv_obj_delete(ed.root);
    ed = {};
    free_browse_result_if_idle();
    free_discover_result_if_idle();
}

// ============================================================================
// Text entry (full screen with keyboard)
// ============================================================================

static void entry_cancel_cb(lv_event_t* e)
{
    (void)e;
    close_text_entry();
}

static bool parse_uint(const char* s, uint32_t lo, uint32_t hi, uint32_t* out)
{
    if (!s || !*s) return false;
    char* end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (*end != '\0' || v < lo || v > hi) return false;
    *out = (uint32_t)v;
    return true;
}

static void entry_error(const char* msg)
{
    lv_label_set_text(s_state.entry.err, msg);
    lv_obj_remove_flag(s_state.entry.err, LV_OBJ_FLAG_HIDDEN);
}

static void entry_save_cb(lv_event_t* e)
{
    (void)e;
    TextEntry& te = s_state.entry;
    Editor& ed = s_state.editor;
    if (!te.ta) return;
    const char* text = lv_textarea_get_text(te.ta);
    char buf[64];

    if (te.field == Field::Host) {
        // Trim surrounding spaces from a pasted or mistyped address.
        while (*text == ' ') ++text;
        snprintf(buf, sizeof(buf), "%s", text);
        size_t n = strlen(buf);
        while (n > 0 && buf[n - 1] == ' ') buf[--n] = '\0';
        if (!perf::host_valid(buf)) {
            entry_error(i18n_get(STR_PERF_INVALID_HOST));
            return;
        }
        if (ed.ed.source == perf::SensorSource::BacnetIp &&
            strncmp(ed.ed.host, buf, sizeof(ed.ed.host)) != 0) {
            clear_editor_bacnet_identity();
        }
        snprintf(ed.ed.host, sizeof(ed.ed.host), "%s", buf);
    } else {
        uint32_t lo = 0;
        uint32_t hi = 65535;
        if (te.field == Field::Port) lo = 1;
        if (te.field == Field::UnitId) hi = 255;
        if (te.field == Field::Register && ed.ed.source == perf::SensorSource::BacnetIp) {
            hi = perf::kBacnetInstanceMax;
        }
        uint32_t v = 0;
        if (!parse_uint(text, lo, hi, &v)) {
            snprintf(buf, sizeof(buf), i18n_get(STR_PERF_INVALID_NUMBER), (unsigned)lo, (unsigned)hi);
            entry_error(buf);
            return;
        }
        if (te.field == Field::Port) {
            if (ed.ed.source == perf::SensorSource::BacnetIp && ed.ed.port != (uint16_t)v) {
                clear_editor_bacnet_identity();
            }
            ed.ed.port = (uint16_t)v;
        }
        if (te.field == Field::UnitId) ed.ed.unit_id = (uint8_t)v;
        if (te.field == Field::Register) {
            if (ed.ed.source == perf::SensorSource::BacnetIp) {
                if (ed.ed.bacnet_instance != v) clear_editor_bacnet_identity();
                ed.ed.bacnet_instance = v;
            } else {
                ed.ed.address = (uint16_t)v;
            }
        }
    }
    close_text_entry();
    editor_changed();
}

static void open_text_entry(Field f)
{
    Editor& ed = s_state.editor;
    TextEntry& te = s_state.entry;
    if (!ed.root || te.root) return;
    te.field = f;

    string_id_t title_id = STR_PERF_HOST_TITLE;
    char value[perf::kHostMax];
    switch (f) {
        case Field::Host: snprintf(value, sizeof(value), "%s", ed.ed.host); break;
        case Field::Port:
            title_id = STR_PERF_PORT;
            snprintf(value, sizeof(value), "%u", (unsigned)ed.ed.port);
            break;
        case Field::UnitId:
            title_id = STR_PERF_UNIT_ID;
            snprintf(value, sizeof(value), "%u", (unsigned)ed.ed.unit_id);
            break;
        case Field::Register:
        default:
            title_id = ed.ed.source == perf::SensorSource::BacnetIp ? STR_PERF_OBJECT : STR_PERF_REGISTER;
            if (ed.ed.source == perf::SensorSource::BacnetIp) {
                snprintf(value, sizeof(value), "%lu",
                         (unsigned long)(ed.ed.bacnet_instance == perf::kBacnetUnsetInstance
                                             ? 0
                                             : ed.ed.bacnet_instance));
            } else {
                snprintf(value, sizeof(value), "%u", (unsigned)ed.ed.address);
            }
            break;
    }

    te.root = lv_obj_create(ed.root);
    lv_obj_set_size(te.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(te.root, 0, 0);
    lv_obj_set_style_bg_color(te.root, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(te.root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(te.root, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(te.root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(te.root, 0, LV_PART_MAIN);
    disable_scrolling(te.root);
    lv_obj_set_user_data(te.root, (void*)"perf_entry");

    lv_obj_t* title = make_label(te.root, i18n_get(title_id), UI_FONT_HEADER, COLOR_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    int32_t ta_y = 100;
    if (f == Field::Host) {
        lv_obj_t* hint = make_label(te.root, i18n_get(STR_PERF_HOST_HINT), UI_FONT_SMALL, COLOR_TEXT_DIM);
        lv_obj_set_width(hint, LV_PCT(90));
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 95);
        ta_y = 150;
    }

    te.ta = lv_textarea_create(te.root);
    lv_obj_set_size(te.ta, LV_PCT(90), 80);
    lv_obj_align(te.ta, LV_ALIGN_TOP_MID, 0, ta_y);
    lv_textarea_set_one_line(te.ta, true);
    lv_obj_set_style_text_font(te.ta, FONT_NORMAL, LV_PART_MAIN);
    if (f == Field::Host) {
        lv_textarea_set_max_length(te.ta, perf::kHostMax - 1);
        lv_textarea_set_accepted_chars(
            te.ta, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_");
    } else {
        lv_textarea_set_max_length(te.ta, f == Field::Register &&
                                              ed.ed.source == perf::SensorSource::BacnetIp
                                          ? 7
                                          : 5);
        lv_textarea_set_accepted_chars(te.ta, "0123456789");
    }
    lv_textarea_set_text(te.ta, value);
    lv_obj_set_user_data(te.ta, (void*)"perf_entry_input");

    te.err = make_label(te.root, "", FONT_NORMAL, COLOR_ERROR);
    lv_obj_set_width(te.err, LV_PCT(90));
    lv_label_set_long_mode(te.err, LV_LABEL_LONG_WRAP);
    lv_obj_align(te.err, LV_ALIGN_TOP_MID, 0, ta_y + 95);
    lv_obj_add_flag(te.err, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_user_data(te.err, (void*)"perf_entry_error");

    lv_obj_t* kb = ui_keyboard_create(te.root, te.ta,
        f == Field::Host ? UiKeyboardKind::Text : UiKeyboardKind::Number, COLOR_CARD);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_user_data(kb, (void*)"perf_entry_keyboard");
    lv_obj_add_event_cb(kb, entry_save_cb, LV_EVENT_READY, NULL);

    lv_obj_t* bar = lv_obj_create(te.root);
    lv_obj_set_size(bar, LV_PCT(100), 110);
    lv_obj_align_to(bar, kb, LV_ALIGN_OUT_TOP_MID, 0, -8);
    lv_obj_set_style_bg_color(bar, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bar, 30, LV_PART_MAIN);
    disable_scrolling(bar);

    lv_obj_t* cancel = make_button(bar, i18n_get(STR_CANCEL), "perf_entry_cancel", COLOR_CARD, COLOR_TEXT,
                                   entry_cancel_cb, NULL);
    lv_obj_set_size(cancel, 300, 80);
    lv_obj_align(cancel, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_border_width(cancel, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(cancel, COLOR_TEXT_DIM, LV_PART_MAIN);

    lv_obj_t* save = make_button(bar, i18n_get(STR_SAVE), "perf_entry_save", COLOR_ACCENT, COLOR_BG,
                                 entry_save_cb, NULL);
    lv_obj_set_size(save, 300, 80);
    lv_obj_align(save, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_move_foreground(kb);
}

static void close_text_entry(void)
{
    TextEntry& te = s_state.entry;
    if (te.root) lv_obj_delete(te.root);
    te = {};
}
