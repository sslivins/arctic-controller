/*
 * Arctic Heat Pump Controller
 * Energy (History tab segment): daily electricity and estimated heat from the
 * energy ledger, two weeks at a time, with a per-day detail and its runs.
 */
#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Builds the energy view into parent and returns its root, or NULL if it is
// already shown.
lv_obj_t* energy_screen_create_in(lv_obj_t* parent);
void energy_screen_hide(void);
bool energy_screen_is_shown(void);

#ifdef __cplusplus
}
#endif
