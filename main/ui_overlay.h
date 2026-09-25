#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Lifetime-bound overlay bookkeeping.
//
// Overlays (sub-screens, dialogs) can be torn down by paths their owner never
// sees: display-off's app_navigation_return_home(), lv_obj_clean() of the top
// layer, or deletion of a parent. Any owner state restored only from the
// owner's own close callback is then left stale -- e.g. a tab's content that
// stays hidden forever. These helpers hook LV_EVENT_DELETE on the overlay so
// the restore happens however the overlay goes away.

// Hide `covered` now; show it again when `overlay` is deleted. Safe if
// `covered` is deleted first.
void ui_overlay_cover(lv_obj_t* overlay, lv_obj_t* covered);

// Set `*handle` to NULL when `overlay` is deleted, so the owner never holds a
// dangling pointer. `handle` must outlive the overlay (e.g. file-static state).
void ui_overlay_track(lv_obj_t* overlay, lv_obj_t** handle);

#ifdef __cplusplus
}
#endif
