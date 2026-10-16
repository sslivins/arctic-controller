#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Builds the cycle-history chart into parent (the History tab's Cycles host)
// and returns its root, or NULL if it is already shown.
lv_obj_t* heatpump_history_create_in(lv_obj_t* parent);
// Tears the chart down and frees its sample buffers.
void heatpump_history_hide(void);
bool heatpump_history_is_shown(void);
// Opens the next chart (or re-centres the shown one) on the given moment
// instead of the latest window.
void heatpump_history_open_centered(uint32_t timestamp);

#ifdef __cplusplus
}
#endif