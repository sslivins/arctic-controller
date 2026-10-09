#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// 30x30 A8 icons for the home screen component indicators (regen_home_icons.py).
extern const lv_image_dsc_t home_icon_compressor;
extern const lv_image_dsc_t home_icon_fan;
extern const lv_image_dsc_t home_icon_pump;
extern const lv_image_dsc_t home_icon_heater;

#ifdef __cplusplus
}
#endif
