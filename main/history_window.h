/*
 * Arctic Heat Pump Controller
 * Cycle-history window navigation: where an N-second window should end
 * after a step or a "centre here" gesture, clamped so it never runs past
 * the latest data.
 */
#pragma once

#include <stdint.h>

// End of the window shifted by `delta` seconds (negative = earlier),
// clamped to `latest_end` and to no earlier than one window from zero.
static inline uint32_t history_window_shift(uint32_t end, int32_t delta,
                                            uint32_t window,
                                            uint32_t latest_end) {
    int64_t e = (int64_t)end + delta;
    if (e > (int64_t)latest_end) e = latest_end;
    if (e < (int64_t)window) e = window;
    return (uint32_t)e;
}

// End of the window that puts `t` in the middle, snapped to `snap` seconds
// and clamped to `latest_end`.
static inline uint32_t history_window_center(uint32_t t, uint32_t window,
                                             uint32_t snap,
                                             uint32_t latest_end) {
    uint64_t e = (uint64_t)t + window / 2;
    if (snap > 0) e = (e + snap / 2) / snap * snap;
    if (e > latest_end) e = latest_end;
    if (e < window) e = window;
    return (uint32_t)e;
}
