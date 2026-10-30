#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Builds the cycle-history chart into parent (the History tab's Cycles host)
// and returns its root, or NULL if it is already shown.
lv_obj_t* heatpump_history_create_in(lv_obj_t* parent);
// Tears the chart down and frees its sample buffers.
void heatpump_history_hide(void);
bool heatpump_history_is_shown(void);
// Opens the next chart (or re-centres the shown one) on the given moment
// instead of the latest window.
void heatpump_history_open_centered(uint32_t timestamp);
// Same, for a span: centred on it if it fits the window, otherwise on start.
void heatpump_history_open_span(uint32_t start, uint32_t end);

typedef struct {
    int32_t cursor;
    uint32_t samples;
    bool power_view;      // power pane shown

    bool cached;          // redraw copied the cached static layer
    uint32_t draw_calls;
    int64_t press_us;     // cursor move, including any static-layer rebuild
    int64_t build_us;     // static-layer rebuild alone
    int64_t refr_us;      // the forced screen refresh
    int64_t draw_us;      // chart draw callback within it
} history_cursor_profile_t;
bool heatpump_history_profile_cursor(int32_t x, history_cursor_profile_t* out);

#ifdef __cplusplus
}
#endif