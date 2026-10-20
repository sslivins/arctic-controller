/*
 * Arctic Heat Pump Controller
 * Cycle-history window navigation: where an N-second window should end
 * after a step or a "centre here" gesture, clamped so it never runs past
 * the latest data.
 */
#pragma once

#include <stdint.h>

// Zoom presets for the cycle history (device screen and web API), widest
// first. The default is the widest.
static constexpr uint32_t HISTORY_WINDOW_PRESETS[] = {8 * 3600, 4 * 3600,
                                                     2 * 3600, 1 * 3600};
static constexpr uint32_t HISTORY_WINDOW_DEFAULT_SECONDS = HISTORY_WINDOW_PRESETS[0];

static inline bool history_window_is_preset(uint32_t window) {
    for (uint32_t preset : HISTORY_WINDOW_PRESETS) {
        if (preset == window) return true;
    }
    return false;
}

// Fine navigation step: an eighth of the window, but never under 15 min
// (8 h -> 1 h, 4 h -> 30 min, 2 h and 1 h -> 15 min).
static inline uint32_t history_window_step(uint32_t window) {
    const uint32_t step = window / 8;
    return step < 15 * 60 ? 15 * 60 : step;
}

// Spacing of the time-axis gridlines, so every preset gets 4-5 ticks
// (8 h -> 2 h, 4 h -> 1 h, 2 h -> 30 min, 1 h -> 15 min).
static inline uint32_t history_window_tick(uint32_t window) {
    return window / 4;
}

// Snap for "centre here" gestures: 15 min, or less for short windows so the
// tapped moment still lands near the middle (2 h -> 10 min, 1 h -> 5 min).
static inline uint32_t history_window_snap(uint32_t window) {
    const uint32_t snap = window / 12;
    return snap > 15 * 60 ? 15 * 60 : snap;
}

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
