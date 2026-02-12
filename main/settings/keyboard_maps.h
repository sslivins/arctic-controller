/**
 * Custom keyboard maps for WiFi password dialog
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

#ifdef __cplusplus
}
#endif
