/*
 * Arctic Heat Pump Controller
 * Custom Fonts with Latin Extended Support
 * 
 * These fonts include accented characters for French, Spanish, etc.
 * Also includes curly quotes (U+2018-U+201F) and FontAwesome icons.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Montserrat fonts with Latin-1 Supplement (0x20-0xFF) + Greek Delta (U+0394)
// + Curly Quotes (0x2018-0x201F) + FontAwesome icons
LV_FONT_DECLARE(montserrat_16_latin);
LV_FONT_DECLARE(montserrat_24_latin);
LV_FONT_DECLARE(montserrat_32_latin);

// Misc glyph fonts: geometric shapes (U+25CB ○, U+25CF ●) etc.
// These are set as fallback fonts on the latin fonts at compile time.
LV_FONT_DECLARE(montserrat_16_misc);
LV_FONT_DECLARE(montserrat_24_misc);
LV_FONT_DECLARE(montserrat_32_misc);

// FontAwesome symbols included in the fonts above
// Lock icon (U+F023 = 61475)
#define FA_SYMBOL_LOCK "\xEF\x80\xA3"

#ifdef __cplusplus
}
#endif
