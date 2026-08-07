/*
 * Arctic Heat Pump Controller
 * Tab shell — single-screen persistent tab container (approach B)
 *
 * Owns a content area that hosts the four tab panels (Home, Status, Control,
 * Events). All panels are built once and kept resident; switching a tab simply
 * shows the target panel (and resumes its update timer) while hiding the others
 * (pausing their timers). No screen teardown happens on navigation, so tab
 * switches are instant, preserve per-tab state (scroll position, etc.), and
 * cannot trigger the input-device use-after-free that plagued the old
 * hub-and-spoke / full-screen-swap navigation.
 */
#pragma once

#include <lvgl.h>
#include "nav_bar.h"

#ifdef __cplusplus
extern "C" {
#endif

// Y offset (px) at which tab content begins — clears the persistent top status
// bar. Matches the legacy Home content offset.
#define TAB_CONTENT_TOP 90

/**
 * @brief Build the tab shell on the given root screen.
 *        Creates the content area, all four tab panels, and the persistent
 *        bottom nav bar. Home is shown initially.
 * @param root The root LVGL screen (already hosts the top status bar).
 */
void tab_shell_create(lv_obj_t* root);

/**
 * @brief Switch the visible tab. Hides the current panel (pausing its timer)
 *        and shows the target panel (resuming its timer). No-op if already on
 *        the target tab.
 */
void tab_shell_select(nav_tab_t tab);

/**
 * @brief The currently visible tab.
 */
nav_tab_t tab_shell_current(void);

#ifdef __cplusplus
}
#endif
