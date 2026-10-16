/*
 * Arctic Heat Pump Controller
 * History tab: a Cycles | Events switch over the cycle-history chart and the
 * event log.
 */
#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HISTORY_SEG_CYCLES = 0,
    HISTORY_SEG_ENERGY,
    HISTORY_SEG_EVENTS,
    HISTORY_SEG_COUNT,
} history_seg_t;

void history_tab_create_in(lv_obj_t* parent);
void history_tab_set_active(bool active);
// Tears down both segments (language change).
void history_tab_hide(void);
// Switches to the History tab showing the given segment.
void history_tab_open(history_seg_t seg);
history_seg_t history_tab_segment(void);

#ifdef __cplusplus
}
#endif
