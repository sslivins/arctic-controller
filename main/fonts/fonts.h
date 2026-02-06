/*
 * Arctic Heat Pump Controller
 * Custom Fonts with Latin Extended Support
 * 
 * These fonts include accented characters for French, Spanish, etc.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Montserrat fonts with Latin-1 Supplement (0x20-0xFF)
// Includes: é, è, à, ù, ñ, ü, ö, etc.
LV_FONT_DECLARE(montserrat_16_latin);
LV_FONT_DECLARE(montserrat_24_latin);

#ifdef __cplusplus
}
#endif
