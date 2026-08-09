#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize display inactivity tracking and pointer input interception.
 * Must be called after the LVGL display and input devices are created.
 */
void display_idle_init(void);

/**
 * Record user activity.
 * @return true when this activity woke a dimmed display and must be consumed.
 */
bool display_idle_handle_activity(void);

/**
 * Dim immediately using the normal idle behavior.
 * Intended for test instrumentation.
 */
void display_idle_force_dim(void);

/**
 * @return true while the display is idle-dimmed.
 */
bool display_idle_is_dimmed(void);

#ifdef __cplusplus
}
#endif
