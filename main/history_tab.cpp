/*
 * Arctic Heat Pump Controller
 * History tab: a Cycles | Events switch over the cycle-history chart and the
 * event log.
 */
#include "history_tab.h"
#include "tab_shell.h"
#include "nav_bar.h"
#include "ui_common.h"
#include "event_log_screen.h"
#include "heatpump_history_screen.h"
#include "energy_screen.h"
#include "i18n/i18n.h"
#include <esp_log.h>

static const char* TAG = "history_tab";

#define SEG_BAR_H        76
#define SEG_ACCENT       lv_color_hex(0xa78bfa)
#define SEG_COLOR_IDLE   lv_color_hex(0x2c3140)
#define SEG_BORDER_IDLE  lv_color_hex(0x4a5166)
#define SEG_TEXT_ACTIVE  lv_color_hex(0x14121c)

static const struct {
    string_id_t str_id;
    const char* tag;
} s_seg_defs[HISTORY_SEG_COUNT] = {
    { STR_HISTORY_SEG_CYCLES, "history_seg_cycles" },
    { STR_HP_ENERGY,          "history_seg_energy" },
    { STR_EVENT_LOG,          "history_seg_events" },
};

static lv_obj_t* s_btns[HISTORY_SEG_COUNT];
static lv_obj_t* s_labels[HISTORY_SEG_COUNT];
static lv_obj_t* s_hosts[HISTORY_SEG_COUNT];
// Survives tab switches and language rebuilds so the tab reopens where the
// user left it.
static history_seg_t s_seg = HISTORY_SEG_CYCLES;
static bool s_active = false;

static void style_buttons(void) {
    for (int i = 0; i < HISTORY_SEG_COUNT; i++) {
        if (!s_btns[i]) continue;
        const bool on = i == (int)s_seg;
        // Selected: solid accent. Idle: a raised outlined chip, so all three
        // read as buttons against the dark page.
        lv_obj_set_style_bg_color(s_btns[i], on ? SEG_ACCENT : SEG_COLOR_IDLE, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_btns[i], on ? 0 : 2, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_labels[i], on ? SEG_TEXT_ACTIVE : UI_COLOR_TEXT,
                                    LV_PART_MAIN);
    }
}

// Shows the selected segment and runs only it. The chart is built on demand
// and freed when hidden, since its sample buffers are large.
static void apply_segment(void) {
    if (!s_hosts[HISTORY_SEG_CYCLES]) return;
    style_buttons();
    for (int i = 0; i < HISTORY_SEG_COUNT; i++) {
        if (i == (int)s_seg && s_active) {
            lv_obj_clear_flag(s_hosts[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_hosts[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    const bool cycles = s_active && s_seg == HISTORY_SEG_CYCLES;
    const bool energy = s_active && s_seg == HISTORY_SEG_ENERGY;
    const bool events = s_active && s_seg == HISTORY_SEG_EVENTS;
    if (!cycles && heatpump_history_is_shown()) heatpump_history_hide();
    if (!energy && energy_screen_is_shown()) energy_screen_hide();
    event_log_screen_set_active(events);
    if (cycles && !heatpump_history_is_shown()) {
        heatpump_history_create_in(s_hosts[HISTORY_SEG_CYCLES]);
    }
    if (energy && !energy_screen_is_shown()) {
        energy_screen_create_in(s_hosts[HISTORY_SEG_ENERGY]);
    }
}

static void seg_cb(lv_event_t* e) {
    const history_seg_t seg = (history_seg_t)(intptr_t)lv_event_get_user_data(e);
    if (seg == s_seg) return;
    s_seg = seg;
    apply_segment();
}

void history_tab_create_in(lv_obj_t* parent) {
    lv_obj_update_layout(parent);
    const int32_t host_h = lv_obj_get_height(parent) - SEG_BAR_H;

    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), SEG_BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_hor(bar, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_top(bar, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_column(bar, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(bar, (void*)"history_seg_bar");

    for (int i = 0; i < HISTORY_SEG_COUNT; i++) {
        lv_obj_t* btn = lv_button_create(bar);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_height(btn, SEG_BAR_H - 14);
        lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, SEG_BORDER_IDLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x3a4053), LV_STATE_PRESSED);
        lv_obj_set_user_data(btn, (void*)s_seg_defs[i].tag);
        lv_obj_add_event_cb(btn, seg_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, i18n_get(s_seg_defs[i].str_id));
        lv_obj_set_style_text_font(lbl, UI_FONT_SMALL, LV_PART_MAIN);
        lv_obj_center(lbl);
        s_btns[i] = btn;
        s_labels[i] = lbl;

        lv_obj_t* host = lv_obj_create(parent);
        lv_obj_remove_style_all(host);
        lv_obj_set_size(host, LV_PCT(100), host_h);
        lv_obj_align(host, LV_ALIGN_TOP_MID, 0, SEG_BAR_H);
        lv_obj_clear_flag(host, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(host, LV_OBJ_FLAG_HIDDEN);
        s_hosts[i] = host;
    }

    // The event log keeps its filters and search between visits, so it is
    // built once like the other tabs; the chart is built when shown.
    event_log_screen_create_in(s_hosts[HISTORY_SEG_EVENTS]);
    apply_segment();
    ESP_LOGI(TAG, "history tab created (host_h=%ld)", (long)host_h);
}

void history_tab_set_active(bool active) {
    s_active = active;
    apply_segment();
}

void history_tab_hide(void) {
    s_active = false;
    if (heatpump_history_is_shown()) heatpump_history_hide();
    if (energy_screen_is_shown()) energy_screen_hide();
    event_log_screen_hide();
    for (int i = 0; i < HISTORY_SEG_COUNT; i++) {
        s_btns[i] = nullptr;
        s_labels[i] = nullptr;
        s_hosts[i] = nullptr;
    }
}

void history_tab_open(history_seg_t seg) {
    if (seg < 0 || seg >= HISTORY_SEG_COUNT) return;
    s_seg = seg;
    if (tab_shell_current() == NAV_TAB_HISTORY) {
        apply_segment();
    } else {
        tab_shell_select(NAV_TAB_HISTORY);
    }
}

history_seg_t history_tab_segment(void) {
    return s_seg;
}
