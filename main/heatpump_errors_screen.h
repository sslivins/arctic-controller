/*
 * Arctic Heat Pump Controller
 * History › Faults: active faults and fault history, each card expanding in
 * place to show the resolution, a troubleshooting QR code and a jump to the
 * fault on the cycle chart.
 */
#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Builds the faults list into parent (the History tab's Faults host).
void heatpump_errors_create_in(lv_obj_t* parent);
// Tears the list down and stops its refresh timers.
void heatpump_errors_hide(void);
bool heatpump_errors_is_shown(void);

#ifdef __cplusplus
}
#endif