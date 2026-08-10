#pragma once

#include <stdbool.h>
#include <stdint.h>

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
 * Turn the backlight fully off and enter the second idle stage.
 * Intended for test instrumentation.
 */
void display_idle_force_off(void);

/**
 * @return true while the display is idle-dimmed.
 */
bool display_idle_is_dimmed(void);

/**
 * @return true while the display backlight is fully off.
 */
bool display_idle_is_off(void);

/**
 * Get or update the persisted staged idle timers.
 * A value of 0 means Never; valid non-zero values are 1-5 minutes.
 * The off timer starts only after the display has dimmed.
 */
uint8_t display_idle_get_dim_minutes(void);
uint8_t display_idle_get_off_minutes(void);
bool display_idle_set_timeouts(uint8_t dim_minutes, uint8_t off_minutes);

#ifdef __cplusplus
}
#endif
