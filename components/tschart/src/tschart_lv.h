/*
 * The one place tschart reaches into LVGL internals: subclassing needs
 * lv_obj_class_private.h, the cached static image needs the display's
 * layer list and the draw dispatcher. Pinned to LVGL 9.5 on purpose.
 */
#pragma once

#include "lvgl.h"
#include "lvgl_private.h"

#if LVGL_VERSION_MAJOR != 9 || LVGL_VERSION_MINOR != 5
#error "tschart uses LVGL 9.5 internals; check tschart_lv.h before upgrading"
#endif
