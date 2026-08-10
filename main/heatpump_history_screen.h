#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*heatpump_history_close_cb_t)(void);

void heatpump_history_show(lv_obj_t* parent, heatpump_history_close_cb_t on_close);
void heatpump_history_hide(void);
bool heatpump_history_is_shown(void);

#ifdef __cplusplus
}
#endif
