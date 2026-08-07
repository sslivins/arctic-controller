/*
 * Arctic Heat Pump Controller
 * Tab shell — single-screen persistent tab container (approach B) implementation
 */
#include "tab_shell.h"
#include "ui_common.h"
#include "heatpump_screen.h"
#include "heatpump_temps_screen.h"
#include "heatpump_control_screen.h"
#include "event_log_screen.h"
#include <esp_log.h>

static const char* TAG = "tab_shell";

static lv_obj_t* s_content_area = nullptr;
static lv_obj_t* s_panels[NAV_TAB_COUNT] = { nullptr, nullptr, nullptr, nullptr };
static nav_tab_t s_current = NAV_TAB_HOME;

// Enable/disable a tab's periodic updates so only the visible panel polls.
static void set_panel_active(nav_tab_t tab, bool active) {
    switch (tab) {
        case NAV_TAB_HOME:    heatpump_screen_set_active(active);     break;
        case NAV_TAB_STATUS:  heatpump_temps_set_active(active);      break;
        case NAV_TAB_CONTROL: heatpump_control_set_active(active);    break;
        case NAV_TAB_EVENTS:  event_log_screen_set_active(active);    break;
        default: break;
    }
}

static lv_obj_t* make_panel(void) {
    lv_obj_t* panel = lv_obj_create(s_content_area);
    lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

void tab_shell_create(lv_obj_t* root) {
    // Content area spans from just below the top status bar to the bottom of the
    // screen. The persistent nav bar is drawn last (on root) and overlays the
    // bottom NAV_BAR_H of this area; each panel reserves that space internally.
    int32_t disp_h = lv_display_get_vertical_resolution(lv_display_get_default());

    s_content_area = lv_obj_create(root);
    lv_obj_set_size(s_content_area, LV_PCT(100), disp_h - TAB_CONTENT_TOP);
    lv_obj_align(s_content_area, LV_ALIGN_TOP_MID, 0, TAB_CONTENT_TOP);
    lv_obj_set_style_bg_opa(s_content_area, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_content_area, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_content_area, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_content_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(s_content_area, (void*)"tab_content_area");

    // Build each panel + its content once.
    s_panels[NAV_TAB_HOME]    = make_panel();
    s_panels[NAV_TAB_STATUS]  = make_panel();
    s_panels[NAV_TAB_CONTROL] = make_panel();
    s_panels[NAV_TAB_EVENTS]  = make_panel();

    heatpump_screen_create(s_panels[NAV_TAB_HOME], 0);
    heatpump_temps_create_in(s_panels[NAV_TAB_STATUS]);
    heatpump_control_create_in(s_panels[NAV_TAB_CONTROL]);
    event_log_screen_create_in(s_panels[NAV_TAB_EVENTS]);

    // Show Home, hide the rest; only the active panel polls.
    for (int i = 0; i < NAV_TAB_COUNT; i++) {
        if (i == NAV_TAB_HOME) {
            lv_obj_clear_flag(s_panels[i], LV_OBJ_FLAG_HIDDEN);
            set_panel_active((nav_tab_t)i, true);
        } else {
            lv_obj_add_flag(s_panels[i], LV_OBJ_FLAG_HIDDEN);
            set_panel_active((nav_tab_t)i, false);
        }
    }
    s_current = NAV_TAB_HOME;

    // Persistent bottom nav bar (drawn on root, above the content area).
    nav_bar_create(root, NAV_TAB_HOME);

    ESP_LOGI(TAG, "tab shell created (content_h=%ld)", (long)(disp_h - TAB_CONTENT_TOP));
}

void tab_shell_select(nav_tab_t tab) {
    if (tab < 0 || tab >= NAV_TAB_COUNT) return;
    if (tab == s_current) return;
    if (!s_panels[tab] || !s_panels[s_current]) return;

    ESP_LOGI(TAG, "select %d -> %d", (int)s_current, (int)tab);

    // Hide + pause the outgoing panel.
    lv_obj_add_flag(s_panels[s_current], LV_OBJ_FLAG_HIDDEN);
    set_panel_active(s_current, false);

    // Show + resume the incoming panel.
    lv_obj_clear_flag(s_panels[tab], LV_OBJ_FLAG_HIDDEN);
    set_panel_active(tab, true);

    s_current = tab;
    nav_bar_set_active(tab);
}

nav_tab_t tab_shell_current(void) {
    return s_current;
}
