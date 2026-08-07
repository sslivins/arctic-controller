/*
 * Arctic Heat Pump Controller
 * Persistent bottom navigation bar (prototype)
 *
 * A shared tab bar drawn once at the bottom of the root screen. Tapping a tab
 * drives the tab shell (see tab_shell.h) to show the corresponding panel. The
 * bar itself is persistent and is never rebuilt on navigation; only the active
 * highlight is restyled via nav_bar_set_active().
 */
#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Height (px) of the bottom nav bar. The tab content area reserves this much
// room at the bottom so nothing hides behind the bar.
#define NAV_BAR_H 84

typedef enum {
    NAV_TAB_HOME = 0,
    NAV_TAB_STATUS,
    NAV_TAB_CONTROL,
    NAV_TAB_EVENTS,
    NAV_TAB_COUNT,
} nav_tab_t;

// Build the persistent bottom navigation bar on `screen`, highlighting `active`.
// Returns the bar object.
lv_obj_t* nav_bar_create(lv_obj_t* screen, nav_tab_t active);

// Restyle the bar so `active` is highlighted and the others are quiet. Called by
// the tab shell whenever the visible tab changes.
void nav_bar_set_active(nav_tab_t active);

#ifdef __cplusplus
}
#endif
