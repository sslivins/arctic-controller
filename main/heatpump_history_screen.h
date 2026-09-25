#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*heatpump_history_close_cb_t)(void);

// Returns the overlay object (NULL if already shown) so the caller can bind
// state to its lifetime, e.g. with ui_overlay_cover().
lv_obj_t* heatpump_history_show(lv_obj_t* parent, heatpump_history_close_cb_t on_close);
// Tears the overlay down and always invokes on_close, whoever calls it.
void heatpump_history_hide(void);
bool heatpump_history_is_shown(void);

#ifdef __cplusplus
}
#endif
