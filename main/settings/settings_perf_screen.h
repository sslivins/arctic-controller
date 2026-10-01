/*
 * Arctic Heat Pump Controller
 * Settings - Heat output & COP screen
 *
 * Assumed loop flow, loop fluid, and where the supply and return temperatures
 * for the estimate come from (the heat pump's own sensors or any Modbus TCP
 * device), with a live view of the current estimate.
 */
#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*on_back)(void);  // Called when back button is pressed
} perf_screen_config_t;

void perf_screen_create(const perf_screen_config_t* config);

// Deletes timers and pending edits; the screen itself goes with the next
// screen load (auto-delete).
void perf_screen_close(void);

bool perf_screen_is_visible(void);

#ifdef __cplusplus
}
#endif
