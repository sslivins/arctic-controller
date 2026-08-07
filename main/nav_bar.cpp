/*
 * Arctic Heat Pump Controller
 * Persistent bottom navigation bar — implementation
 */
#include "nav_bar.h"
#include "tab_shell.h"
#include "ui_common.h"
#include "i18n/i18n.h"
#include <esp_log.h>

static const char* TAG = "nav_bar";

typedef struct {
    nav_tab_t     tab;
    string_id_t   str_id;    // i18n string id (label text, may embed an icon)
    const char*   test_tag;  // widget tag for test addressability
    lv_color_t    accent;    // active-state accent color
} nav_tab_def_t;

static const nav_tab_def_t s_tabs[NAV_TAB_COUNT] = {
    { NAV_TAB_HOME,    STR_NAV_HOME,        "nav_home",    UI_COLOR_ACCENT },
    { NAV_TAB_STATUS,  STR_HP_BTN_STATUS,   "nav_status",  UI_COLOR_ACCENT },
    { NAV_TAB_CONTROL, STR_HP_BTN_ADVANCED, "nav_control", UI_COLOR_SUCCESS },
    { NAV_TAB_EVENTS,  STR_EVENT_LOG,       "nav_events",  lv_color_hex(0xa78bfa) },
};

// Persistent button + label handles so the active highlight can be restyled
// in place (the bar is never rebuilt on navigation).
static lv_obj_t* s_btns[NAV_TAB_COUNT]   = { nullptr, nullptr, nullptr, nullptr };
static lv_obj_t* s_labels[NAV_TAB_COUNT] = { nullptr, nullptr, nullptr, nullptr };

static void apply_tab_style(nav_tab_t tab, bool is_active) {
    lv_obj_t* btn = s_btns[tab];
    lv_obj_t* lbl = s_labels[tab];
    if (!btn || !lbl) return;
    lv_color_t accent = s_tabs[tab].accent;
    lv_obj_set_style_bg_color(btn, is_active ? lv_color_hex(0x24344f) : UI_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, is_active ? 2 : 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, accent, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, is_active ? accent : UI_COLOR_TEXT_DIM, LV_PART_MAIN);
}

void nav_bar_set_active(nav_tab_t active) {
    for (int i = 0; i < NAV_TAB_COUNT; i++) {
        apply_tab_style((nav_tab_t)i, (nav_tab_t)i == active);
    }
}

void nav_bar_refresh_labels(void) {
    for (int i = 0; i < NAV_TAB_COUNT; i++) {
        if (s_labels[i]) {
            lv_label_set_text(s_labels[i], i18n_get(s_tabs[i].str_id));
        }
    }
}

static void nav_btn_cb(lv_event_t* e) {
    nav_tab_t target = (nav_tab_t)(intptr_t)lv_event_get_user_data(e);
    // No screen teardown happens on a tab switch (panels are persistent), so it
    // is safe to select synchronously from within the click callback.
    tab_shell_select(target);
}

lv_obj_t* nav_bar_create(lv_obj_t* screen, nav_tab_t active) {
    lv_obj_t* bar = lv_obj_create(screen);
    lv_obj_set_size(bar, LV_PCT(100), NAV_BAR_H);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, UI_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, lv_color_hex(0x0f3460), LV_PART_MAIN);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(bar, 6, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_user_data(bar, (void*)"nav_bar");

    for (int i = 0; i < NAV_TAB_COUNT; i++) {
        const nav_tab_def_t* t = &s_tabs[i];

        lv_obj_t* btn = lv_btn_create(bar);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_height(btn, LV_PCT(100));
        lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(btn, 2, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, UI_COLOR_BTN_PRESSED, LV_STATE_PRESSED);
        lv_obj_set_user_data(btn, (void*)t->test_tag);
        lv_obj_add_event_cb(btn, nav_btn_cb, LV_EVENT_CLICKED, (void*)(intptr_t)t->tab);

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, i18n_get(t->str_id));
        lv_obj_set_style_text_font(lbl, UI_FONT_SMALL, LV_PART_MAIN);
        lv_obj_center(lbl);

        s_btns[i]   = btn;
        s_labels[i] = lbl;
    }

    nav_bar_set_active(active);
    ESP_LOGI(TAG, "nav bar created (active=%d)", (int)active);
    return bar;
}
