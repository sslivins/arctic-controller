/**
 * Shared on-screen keyboard maps. Prefer ui_keyboard_create() (ui_keyboard.h)
 * over using these directly.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const char* const kb_map_lc[];
extern const lv_buttonmatrix_ctrl_t kb_ctrl_lc[];

extern const char* const kb_map_uc[];
extern const lv_buttonmatrix_ctrl_t kb_ctrl_uc[];

extern const char* const kb_map_spec[];
extern const lv_buttonmatrix_ctrl_t kb_ctrl_spec[];

extern const char* const kb_map_num[];
extern const lv_buttonmatrix_ctrl_t kb_ctrl_num[];

#ifdef __cplusplus
}
#endif
