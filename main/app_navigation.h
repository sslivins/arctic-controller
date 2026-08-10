#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Dismiss transient UI and return to the persistent Home tab.
// Must be called from the LVGL context or while holding the display lock.
void app_navigation_return_home(void);

#ifdef __cplusplus
}
#endif
