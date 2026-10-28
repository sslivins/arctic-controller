#include "heatpump_history_screen.h"

#include "app_preferences.h"
#include "chart_axis.h"
#include "chart_fault_zone.h"
#include "history_window.h"
#include "fault_history.h"
#include "fonts/fonts.h"
#include "macon_faults.h"
#include "history_storage.h"
#include "i18n/i18n.h"
#include "nav_bar.h"
#include "time_manager.h"
#include "ui_common.h"
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <sdkconfig.h>
#include <bsp/m5stack_tab5.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <limits.h>
#include <lvgl_private.h>  // layer/display internals for static_cache_build
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static constexpr int32_t LATEST_BUTTON_H = 52;
static constexpr uint32_t RETENTION_SECONDS =
    HISTORY_TELEMETRY_RETENTION_DAYS * 24 * 60 * 60;
static constexpr uint32_t CONTIGUOUS_SECONDS =
    HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC + 15;

#define COLOR_BG        lv_color_hex(0x1a1a2e)
#define COLOR_PANEL     lv_color_hex(0x16213e)
#define COLOR_GRID      lv_color_hex(0x3d4f6f)
// Neutral hues: red/blue would read as heating/cooling and clash with the
// mode bands.
#define COLOR_INLET     lv_color_hex(0xd946ef)
#define COLOR_OUTLET    lv_color_hex(0xf1f5f9)
#define COLOR_SETPOINT  lv_color_hex(0x4ade80)
#define COLOR_HEATING   lv_color_hex(0xf97316)
#define COLOR_COOLING   lv_color_hex(0x3b82f6)
#define COLOR_HOT_WATER lv_color_hex(0xfbbf24)
// Red is reserved for faults, drawn as hatched zones with a solid start line
// so they never read as a mode band.
#define COLOR_FAULT     UI_COLOR_ERROR
// Icy teal, as on the home screen's defrost tile.
#define COLOR_DEFROST       lv_color_hex(0x4fb3c9)
#define COLOR_POWER     lv_color_hex(0x60a5fa)

static constexpr size_t MAX_FAULTS = 16;
static constexpr size_t MAX_DEFROSTS = 24;
// Height of the strip above the plot that holds the fault-code labels.
static constexpr int32_t FAULT_STRIP_H = 38;

struct QueryResult {
    uint32_t generation;
    uint32_t start;
    uint32_t end;
    esp_err_t error;
    size_t count;
    history_telemetry_sample_t* samples;
    size_t fault_count;
    fault_interval_t faults[MAX_FAULTS];
    size_t defrost_count;
    fault_interval_t* defrosts;  // PSRAM, MAX_DEFROSTS; handed to state
};

static struct {
    bool shown = false;
    lv_obj_t* overlay = nullptr;
    lv_obj_t* chart = nullptr;
    lv_obj_t* range_label = nullptr;
    lv_obj_t* status_label = nullptr;
    lv_obj_t* previous_button = nullptr;
    lv_obj_t* back_hour_button = nullptr;
    lv_obj_t* forward_hour_button = nullptr;
    lv_obj_t* next_button = nullptr;
    lv_obj_t* latest_button = nullptr;
    history_telemetry_sample_t* samples = nullptr;
    // Plateau-interpolated inlet/outlet (deci-C) for drawing; see
    // interpolate_series(). INT32_MIN marks "no value".
    int32_t* interp_inlet = nullptr;
    int32_t* interp_outlet = nullptr;
    size_t sample_count = 0;
    int32_t cursor = -1;          // touched sample index, -1 = none
    uint32_t window_start = 0;
    uint32_t window_end = 0;
    uint32_t latest_end = 0;
    uint32_t generation = 0;
    bool query_running = false;
    bool pending_query = false;
    uint32_t pending_end = 0;
    size_t fault_count = 0;
    fault_interval_t faults[MAX_FAULTS] = {};
    size_t defrost_count = 0;
    fault_interval_t* defrosts = nullptr;  // PSRAM
    // Power view: electrical draw instead of temperatures. Kept across
    // hide/show so the chart reopens the way the user left it.
    bool power_view = false;
    lv_obj_t* view_temps_button = nullptr;
    lv_obj_t* view_power_button = nullptr;
    lv_obj_t* legend_lines = nullptr;
    lv_obj_t* legend_power = nullptr;
    lv_obj_t* power_label = nullptr;
    uint32_t window_wh = 0;
    // Moment to centre the next opened window on (0 = latest).
    uint32_t pending_center = 0;
    // Zoom preset; kept across hide/show like the view.
    uint32_t window_seconds = HISTORY_WINDOW_DEFAULT_SECONDS;
    lv_obj_t* zoom_buttons[sizeof(HISTORY_WINDOW_PRESETS) /
                           sizeof(HISTORY_WINDOW_PRESETS[0])] = {};
} state;

static void request_window(uint32_t end);

static int32_t display_deci_c(int32_t deci_c) {
    if (app_prefs_get_temp_unit() == TEMP_UNIT_FAHRENHEIT) {
        return (deci_c * 9) / 5 + 320;
    }
    return deci_c;
}

static int32_t deci_c_from_display(int32_t value) {
    if (app_prefs_get_temp_unit() == TEMP_UNIT_FAHRENHEIT) {
        const int32_t num = (value - 320) * 5;
        return (num >= 0 ? num + 4 : num - 4) / 9;
    }
    return value;
}

static lv_color_t mode_color(uint8_t mode) {
    switch (mode) {
        case HISTORY_TELEMETRY_MODE_HEATING: return COLOR_HEATING;
        case HISTORY_TELEMETRY_MODE_COOLING: return COLOR_COOLING;
        case HISTORY_TELEMETRY_MODE_HOT_WATER: return COLOR_HOT_WATER;
        default: return COLOR_GRID;
    }
}

static bool running_sample(const history_telemetry_sample_t& sample) {
    return (sample.flags & HISTORY_TELEMETRY_COMPRESSOR_VALID) &&
           (sample.flags & HISTORY_TELEMETRY_COMPRESSOR_RUNNING) &&
           sample.mode != HISTORY_TELEMETRY_MODE_UNKNOWN;
}

static int32_t map_x(uint32_t timestamp, const lv_area_t& plot) {
    if (timestamp <= state.window_start) return plot.x1;
    if (timestamp >= state.window_end) return plot.x2;
    return plot.x1 + (int32_t)(((uint64_t)(timestamp - state.window_start) *
                                (uint32_t)lv_area_get_width(&plot)) /
                               (state.window_end - state.window_start));
}

static int32_t map_y(int32_t value, int32_t min_value, int32_t max_value,
                     const lv_area_t& plot) {
    if (max_value <= min_value) return plot.y2;
    return plot.y2 - (int32_t)(((int64_t)(value - min_value) *
                                (int32_t)lv_area_get_height(&plot)) /
                               (max_value - min_value));
}

static void draw_line(lv_layer_t* layer, lv_point_t p1, lv_point_t p2,
                      lv_color_t color, int32_t width) {
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width;
    dsc.round_start = true;
    dsc.round_end = true;
    dsc.p1.x = p1.x;
    dsc.p1.y = p1.y;
    dsc.p2.x = p2.x;
    dsc.p2.y = p2.y;
    lv_draw_line(layer, &dsc);
}

static void draw_label(lv_layer_t* layer, const lv_area_t& area,
                       const char* text, lv_color_t color,
                       lv_text_align_t align,
                       const lv_font_t* font = &montserrat_16_latin) {
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.color = color;
    dsc.font = font;
    dsc.text = text;
    dsc.text_local = true;
    dsc.align = align;
    lv_draw_label(layer, &dsc, &area);
}

static void format_clock(char* buf, size_t len, uint32_t timestamp) {
    time_t t = timestamp;
    struct tm local = {};
    localtime_r(&t, &local);
    if (time_mgr_get_24h_format()) {
        strftime(buf, len, "%H:%M", &local);
    } else {
        int hour12 = local.tm_hour % 12;
        if (hour12 == 0) hour12 = 12;
        snprintf(buf, len, "%d:%02d %s", hour12, local.tm_min,
                 local.tm_hour < 12 ? "AM" : "PM");
    }
}

static const char* fault_code(const fault_interval_t& f) {
    const arctic::MaconFaultBit* fb = arctic::macon_fault_bit_for_site(
        static_cast<arctic::MaconFaultSiteId>(f.site));
    return fb != nullptr ? fb->code : "?";
}

// A still-active fault runs to "now", which is the right edge at most.
static uint32_t fault_end(const fault_interval_t& f) {
    if (f.end != 0) return f.end;
    time_t now;
    time(&now);
    uint32_t end = (uint32_t)now;
    return end > state.window_end || end < f.start ? state.window_end : end;
}

static bool sample_value(const history_telemetry_sample_t& sample,
                         uint8_t valid_flag, int16_t value,
                         int32_t* out) {
    if (!(sample.flags & valid_flag)) return false;
    *out = value;
    return true;
}

static constexpr int32_t NO_VALUE = INT32_MIN;

// Inlet/outlet are recorded in whole degrees, so a raw line staircases. Each
// run of identical readings is a plateau the true temperature crossed
// somewhere inside, so the line is drawn through each plateau's midpoint
// (30 for 10 min then 31 reads 30.5 at the changeover). Plateau values,
// including peaks, are kept exactly; a gap or invalid sample breaks the line.
static void interpolate_series(uint8_t valid_flag,
                               int16_t history_telemetry_sample_t::* member,
                               int32_t* out) {
    const history_telemetry_sample_t* s = state.samples;
    const size_t n = state.sample_count;
    for (size_t k = 0; k < n; k++) out[k] = NO_VALUE;

    auto valid = [&](size_t k) { return (s[k].flags & valid_flag) != 0; };
    size_t a = 0;
    while (a < n) {
        if (!valid(a)) { a++; continue; }
        size_t b = a;
        while (b + 1 < n && valid(b + 1) &&
               s[b + 1].timestamp - s[b].timestamp <= CONTIGUOUS_SECONDS) {
            b++;
        }
        // Anchors: segment start, each plateau midpoint, segment end. Times
        // are doubled so plateau midpoints stay integral.
        int64_t t0 = 2 * (int64_t)s[a].timestamp;
        int32_t v0 = s[a].*member;
        size_t k = a;
        auto emit_to = [&](int64_t t1, int32_t v1) {
            while (k <= b && 2 * (int64_t)s[k].timestamp <= t1) {
                const int64_t t = 2 * (int64_t)s[k].timestamp;
                out[k] = (t1 > t0)
                    ? v0 + (int32_t)(((int64_t)(v1 - v0) * (t - t0)) / (t1 - t0))
                    : v1;
                k++;
            }
            t0 = t1;
            v0 = v1;
        };
        for (size_t i = a; i <= b;) {
            size_t j = i;
            while (j + 1 <= b && s[j + 1].*member == s[i].*member) j++;
            emit_to((int64_t)s[i].timestamp + s[j].timestamp, s[i].*member);
            i = j + 1;
        }
        emit_to(2 * (int64_t)s[b].timestamp, s[b].*member);
        a = b + 1;
    }
}

static void recompute_interpolation() {
    if (state.interp_inlet == nullptr || state.interp_outlet == nullptr) return;
    interpolate_series(HISTORY_TELEMETRY_INLET_VALID,
                       &history_telemetry_sample_t::inlet_deci_c,
                       state.interp_inlet);
    interpolate_series(HISTORY_TELEMETRY_OUTLET_VALID,
                       &history_telemetry_sample_t::outlet_deci_c,
                       state.interp_outlet);
}

static lv_area_t plot_area(lv_obj_t* chart) {
    lv_area_t coords;
    lv_obj_get_coords(chart, &coords);
    // The top margin is the fault-label strip, so codes never sit on a line.
    return {coords.x1 + 72, coords.y1 + FAULT_STRIP_H, coords.x2 - 18,
            coords.y2 - 48};
}

// Whole-degree reading in the user's unit, e.g. "37°C".
static void format_reading(char* buf, size_t len, const history_telemetry_sample_t& s,
                           uint8_t valid_flag, int16_t deci_c) {
    if (!(s.flags & valid_flag)) {
        snprintf(buf, len, "--");
        return;
    }
    const int32_t v = display_deci_c(deci_c);
    const long whole = (v >= 0) ? (v + 5) / 10 : -((-v + 5) / 10);
    snprintf(buf, len, "%ld°%s", whole,
             app_prefs_get_temp_unit() == TEMP_UNIT_FAHRENHEIT ? "F" : "C");
}

// Everything but the cursor, drawn once into PSRAM so dragging a finger along
// the chart only redraws the cursor over a copy (a full redraw is 100-160 ms,
// mostly hatching and line segments). Rebuilt when anything under it changes.
struct StaticKey {
    uint32_t data_gen, window_start, window_end;
    int32_t w, h;
    bool power_view;
    uint8_t temp_unit;
    bool operator==(const StaticKey&) const = default;
};
static struct {
    lv_obj_t* canvas = nullptr;  // hidden; renders into buf
    lv_draw_buf_t buf = {};
    void* data = nullptr;
    size_t data_size = 0;
    bool valid = false;
    StaticKey key = {};
} s_static;
static uint32_t s_data_gen = 0;
// Scales the cursor needs, set by whichever static draw ran last.
static int32_t s_temp_min = 0, s_temp_max = 500, s_power_max_w = 1000;

#if CONFIG_TEST_ENDPOINTS
static struct {
    uint32_t calls;
    int64_t total_us;
    int64_t build_us;
    bool cached;
} s_prof;
#endif

static int32_t cursor_at(int32_t px);
static void draw_static(lv_layer_t* layer, const lv_area_t& coords,
                        const lv_area_t& plot);

static StaticKey static_key() {
    lv_area_t coords;
    lv_obj_get_coords(state.chart, &coords);
    return {s_data_gen, state.window_start, state.window_end,
            lv_area_get_width(&coords), lv_area_get_height(&coords),
            state.power_view, (uint8_t)app_prefs_get_temp_unit()};
}

static bool static_cache_ready() {
    return s_static.valid && s_static.key == static_key();
}

static void static_cache_free() {
    s_static.canvas = nullptr;  // deleted with the overlay
    s_static.valid = false;
    heap_caps_free(s_static.data);
    s_static.data = nullptr;
    s_static.data_size = 0;
}

static void static_cache_build() {
    if (static_cache_ready()) return;
#if CONFIG_TEST_ENDPOINTS
    const int64_t t0 = esp_timer_get_time();
#endif
    const StaticKey key = static_key();
    s_static.valid = false;
    const uint32_t stride = (uint32_t)key.w * 4;
    const size_t size = (size_t)stride * (size_t)key.h;
    if (key.w <= 0 || key.h <= 0) return;
    if (size != s_static.data_size) {
        heap_caps_free(s_static.data);
        s_static.data = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, size,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_static.data_size = s_static.data ? size : 0;
        if (s_static.data == nullptr) return;  // keep drawing directly
    }
    memset(s_static.data, 0, size);
    lv_draw_buf_init(&s_static.buf, (uint32_t)key.w, (uint32_t)key.h,
                     LV_COLOR_FORMAT_ARGB8888, stride, s_static.data, size);
    if (s_static.canvas == nullptr) {
        s_static.canvas = lv_canvas_create(state.overlay);
        lv_obj_add_flag(s_static.canvas, LV_OBJ_FLAG_HIDDEN);
    }
    lv_canvas_set_draw_buf(s_static.canvas, &s_static.buf);
    lv_layer_t layer;
    lv_canvas_init_layer(s_static.canvas, &layer);
    lv_area_t coords;
    lv_obj_get_coords(state.chart, &coords);
    lv_area_t plot = plot_area(state.chart);
    lv_area_move(&plot, -coords.x1, -coords.y1);
    const lv_area_t local = {0, 0, key.w - 1, key.h - 1};
    // Pose as a refresh of this layer, as lv_snapshot does, so each draw task
    // runs as it's added. Otherwise thousands queue up and LVGL's list
    // handling goes quadratic (8 h chart: ~10 s vs ~0.15 s).
    lv_display_t* disp = lv_obj_get_display(state.chart);
    lv_display_t* disp_old = lv_refr_get_disp_refreshing();
    lv_layer_t* layer_old = disp->layer_head;
    disp->layer_head = &layer;
    lv_refr_set_disp_refreshing(disp);
    draw_static(&layer, local, plot);
    layer.all_tasks_added = true;
    while (layer.draw_task_head) {
        lv_draw_dispatch_wait_for_request();
        lv_draw_dispatch();
    }
    disp->layer_head = layer_old;
    lv_refr_set_disp_refreshing(disp_old);
    lv_canvas_finish_layer(s_static.canvas, &layer);
    s_static.key = key;
    s_static.valid = true;
#if CONFIG_TEST_ENDPOINTS
    s_prof.build_us += esp_timer_get_time() - t0;
#endif
}

static bool cursor_dirty_area(lv_area_t* out);
static lv_area_t s_cursor_area[2];
static bool s_cursor_area_valid = false;

static void invalidate_cursor_area() {
    if (!s_cursor_area_valid) return;
    for (auto& a : s_cursor_area) lv_obj_invalidate_area(state.chart, &a);
}

// Repaints only the old and new crosshair + popup; the rest comes from the
// cache unchanged.
static void set_cursor(int32_t best) {
    if (best == state.cursor) return;
    if (best >= 0) static_cache_build();
    invalidate_cursor_area();
    state.cursor = best;
    s_cursor_area_valid = cursor_dirty_area(s_cursor_area);
    invalidate_cursor_area();
}

// Touch gestures on the chart: a quick horizontal drag pans through time
// (the cached image slides with the finger, the new window loads on
// release); press-and-hold first, then drag, scrubs the cursor.
static constexpr int32_t PAN_START_PX = 14;
static struct {
    int32_t x0 = 0;
    int32_t dx = 0;
    bool panning = false;
    bool scrubbing = false;
    uint32_t ended_ms = 0;
} s_drag;

// Pan offset limits in px: no further forward than now, no further back
// than retention.
static void pan_limits(int32_t* min_dx, int32_t* max_dx) {
    const lv_area_t plot = plot_area(state.chart);
    const int64_t w = lv_area_get_width(&plot);
    const uint32_t earliest = state.latest_end > RETENTION_SECONDS
                                  ? state.latest_end - RETENTION_SECONDS
                                  : 0;
    const uint32_t fwd = state.latest_end > state.window_end
                             ? state.latest_end - state.window_end : 0;
    const uint32_t back = state.window_start > earliest
                              ? state.window_start - earliest : 0;
    *min_dx = -(int32_t)((int64_t)fwd * w / state.window_seconds);
    *max_dx = (int32_t)((int64_t)back * w / state.window_seconds);
}

static void pan_end(bool apply) {
    if (!s_drag.panning) return;
    s_drag.panning = false;
    s_drag.ended_ms = lv_tick_get();
    const int32_t dx = s_drag.dx;
    s_drag.dx = 0;
    lv_obj_invalidate(state.chart);
    if (!apply || dx == 0) return;
    const lv_area_t plot = plot_area(state.chart);
    const uint32_t snap = history_window_snap(state.window_seconds);
    const int64_t secs = -(int64_t)dx * state.window_seconds /
                         lv_area_get_width(&plot);
    const int64_t steps = (secs >= 0 ? secs + snap / 2 : secs - (int64_t)snap / 2) / snap;
    if (steps == 0) return;
    request_window(history_window_shift(state.window_end, (int32_t)(steps * snap),
                                        state.window_seconds, state.latest_end));
}

static void chart_press_cb(lv_event_t* event) {
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    switch (lv_event_get_code(event)) {
        case LV_EVENT_PRESSED:
            s_drag.x0 = point.x;
            s_drag.dx = 0;
            s_drag.panning = false;
            s_drag.scrubbing = false;
            break;
        case LV_EVENT_LONG_PRESSED:
            if (!s_drag.panning) s_drag.scrubbing = true;
            return;
        case LV_EVENT_RELEASED:
        case LV_EVENT_PRESS_LOST:
            pan_end(lv_event_get_code(event) == LV_EVENT_RELEASED);
            s_drag.scrubbing = false;
            return;
        default:
            break;
    }
    if (s_drag.panning) {
        int32_t lo, hi;
        pan_limits(&lo, &hi);
        int32_t dx = point.x - s_drag.x0;
        if (dx < lo) dx = lo;
        if (dx > hi) dx = hi;
        if (dx != s_drag.dx) {
            s_drag.dx = dx;
            lv_obj_invalidate(state.chart);
        }
        return;
    }
    if (!s_drag.scrubbing && abs(point.x - s_drag.x0) > PAN_START_PX) {
        set_cursor(-1);
        static_cache_build();
        if (static_cache_ready()) {
            s_drag.panning = true;
            s_drag.x0 = point.x;  // start sliding from here, no jump
            return;
        }
    }
    if (state.sample_count == 0) return;
    set_cursor(cursor_at(point.x));
}

static int32_t cursor_at(int32_t px) {
    lv_point_t point = {px, 0};
    const lv_area_t plot = plot_area(state.chart);
    int32_t best = -1;
    int32_t best_dx = INT32_MAX;
    if (point.x >= plot.x1 && point.x <= plot.x2) {
        const uint8_t wanted = state.power_view
            ? HISTORY_TELEMETRY_POWER_VALID
            : (HISTORY_TELEMETRY_INLET_VALID | HISTORY_TELEMETRY_OUTLET_VALID);
        for (size_t i = 0; i < state.sample_count; i++) {
            const auto& s = state.samples[i];
            if (!(s.flags & wanted)) continue;
            const int32_t dx = abs(map_x(s.timestamp, plot) - point.x);
            if (dx < best_dx) { best_dx = dx; best = (int32_t)i; }
        }
    }
    // Only snap to a sample near the finger, so tapping an empty stretch of
    // the chart clears the crosshair.
    if (best_dx > 30) best = -1;
    return best;
}

static void draw_dot(lv_layer_t* layer, int32_t x, int32_t y, lv_color_t color) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = LV_RADIUS_CIRCLE;
    dsc.border_color = COLOR_PANEL;
    dsc.border_width = 2;
    lv_area_t area = {x - 7, y - 7, x + 7, y + 7};
    lv_draw_rect(layer, &dsc, &area);
}

// What the unit was doing at a sample, as the home screen names it. Defrost
// is checked first: it pauses the run, so its samples aren't "running".
static const char* sample_state_name(const history_telemetry_sample_t& s,
                                     lv_color_t* color) {
    for (size_t i = 0; i < state.defrost_count; i++) {
        const auto& d = state.defrosts[i];
        const uint32_t end = d.end != 0 ? d.end : state.window_end;
        if (d.start <= s.timestamp && s.timestamp <= end) {
            *color = COLOR_DEFROST;
            return i18n_get(STR_HOME_DEFROSTING);
        }
    }
    if (!running_sample(s)) {
        *color = UI_COLOR_TEXT_DIM;
        return i18n_get(STR_HOME_IDLE);
    }
    *color = mode_color(s.mode);
    switch (s.mode) {
        case HISTORY_TELEMETRY_MODE_HEATING: return i18n_get(STR_HISTORY_LEGEND_HEATING);
        case HISTORY_TELEMETRY_MODE_COOLING: return i18n_get(STR_HISTORY_LEGEND_COOLING);
        default: return i18n_get(STR_HISTORY_LEGEND_HOT_WATER);
    }
}

// The operating-state row of a cursor popup: a band swatch and the name.
static void draw_state_row(lv_layer_t* layer, const lv_area_t& row,
                           const history_telemetry_sample_t& s,
                           const char* length) {
    lv_color_t color;
    const char* name = sample_state_name(s, &color);
    lv_draw_rect_dsc_t sw;
    lv_draw_rect_dsc_init(&sw);
    sw.bg_color = color;
    sw.radius = 3;
    const int32_t cy = (row.y1 + row.y2) / 2;
    const lv_area_t swatch = {row.x1, cy - 8, row.x1 + 16, cy + 8};
    lv_draw_rect(layer, &sw, &swatch);
    const lv_area_t name_area = {row.x1 + 24, row.y1, row.x2, row.y2};
    draw_label(layer, name_area, name, color, LV_TEXT_ALIGN_LEFT,
               &montserrat_24_latin);
    if (length[0] != '\0') {
        draw_label(layer, row, length, UI_COLOR_TEXT, LV_TEXT_ALIGN_RIGHT,
                   &montserrat_24_latin);
    }
}

static bool compressor_on(const history_telemetry_sample_t& s) {
    return (s.flags & HISTORY_TELEMETRY_COMPRESSOR_VALID) &&
           (s.flags & HISTORY_TELEMETRY_COMPRESSOR_RUNNING);
}

// Start/end of the compressor run holding sample `i`, as the web dashboard
// groups them (a defrost's mode-less samples included). In the compressor
// pause inside a defrost, the defrost itself. A run beginning at the window's
// left edge may have started earlier (clip_start); `open` means it is still
// going at the window's last sample.
struct RunSpan { uint32_t start, end; bool clip_start, open; };
static bool cursor_run(size_t i, RunSpan* run) {
    static constexpr uint32_t GAP = HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC * 5 / 2;
    const uint32_t t = state.samples[i].timestamp;
    const fault_interval_t* defrost = nullptr;
    for (size_t d = 0; d < state.defrost_count && defrost == nullptr; d++) {
        const auto& df = state.defrosts[d];
        if (df.start <= t && t <= (df.end != 0 ? df.end : state.window_end)) {
            defrost = &df;
        }
    }
    // Matches the popup's state row, which reads "Idle" otherwise.
    if (compressor_on(state.samples[i]) &&
        (running_sample(state.samples[i]) || defrost != nullptr)) {
        size_t lo = i, hi = i;
        while (lo > 0 && compressor_on(state.samples[lo - 1]) &&
               state.samples[lo].timestamp - state.samples[lo - 1].timestamp <= GAP) {
            lo--;
        }
        while (hi + 1 < state.sample_count && compressor_on(state.samples[hi + 1]) &&
               state.samples[hi + 1].timestamp - state.samples[hi].timestamp <= GAP) {
            hi++;
        }
        run->start = state.samples[lo].timestamp;
        run->end = state.samples[hi].timestamp + HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC;
        run->clip_start = run->start < state.window_start + GAP;
        run->open = hi + 1 == state.sample_count;
        return true;
    }
    if (defrost == nullptr) return false;
    run->start = defrost->start;
    run->end = defrost->end != 0 ? defrost->end : state.window_end;
    run->clip_start = defrost->start < state.window_start;
    run->open = defrost->end == 0;
    return true;
}

// Popup text for the run under sample `i`: "…2:00 AM – 2:25 AM" (… where the
// window cuts it off, "now" while it runs in the live window) and "25 min".
static bool format_cursor_run(size_t i, char* span, size_t span_len,
                              char* length, size_t length_len) {
    RunSpan run;
    if (!cursor_run(i, &run)) return false;
    const bool live = state.window_end >= state.latest_end;
    char from[12], to[12];
    format_clock(from, sizeof(from), run.start);
    if (run.open && live) {
        snprintf(to, sizeof(to), "%s", i18n_get(STR_HOME_NOW));
    } else {
        format_clock(to, sizeof(to), run.end);
    }
    snprintf(span, span_len, "%s%s \xE2\x80\x93 %s%s",
             run.clip_start ? "\xE2\x80\xA6" : "", from, to,
             run.open && !live ? "\xE2\x80\xA6" : "");
    snprintf(length, length_len, i18n_get(STR_HOME_MINUTES),
             (unsigned)((run.end - run.start + 30) / 60));
    if (run.clip_start || (run.open && !live)) strlcat(length, "+", length_len);
    return true;
}

// The crosshair strip (line + dots) plus the popup box, so a cursor move can
// invalidate just what it covers.
static void cursor_bounds(int32_t x, const lv_area_t& plot, const lv_area_t& box,
                          lv_area_t* out) {
    out[0] = {x - 10, plot.y1 - 10, x + 10, plot.y2 + 10};
    out[1] = box;
}

// With layer == nullptr, only measures: stores the crosshair strip and popup
// box in bounds[0..1].
static void draw_cursor(lv_layer_t* layer, const lv_area_t& plot,
                        int32_t min_value, int32_t max_value,
                        lv_area_t* bounds = nullptr) {
    if (state.cursor < 0 || (size_t)state.cursor >= state.sample_count) return;
    const size_t i = (size_t)state.cursor;
    const auto& s = state.samples[i];
    const int32_t x = map_x(s.timestamp, plot);

    char when[12];
    format_clock(when, sizeof(when), s.timestamp);
    char outlet[16], inlet[16], setpoint[16];
    format_reading(outlet, sizeof(outlet), s, HISTORY_TELEMETRY_OUTLET_VALID,
                   s.outlet_deci_c);
    format_reading(inlet, sizeof(inlet), s, HISTORY_TELEMETRY_INLET_VALID,
                   s.inlet_deci_c);
    format_reading(setpoint, sizeof(setpoint), s,
                   HISTORY_TELEMETRY_SETPOINT_VALID, s.setpoint_deci_c);

    // Faults active at the touched moment, by code (the caption names them).
    static constexpr int MAX_FAULT_ROWS = 2;
    char fault_rows[MAX_FAULT_ROWS][16];
    int fault_row_count = 0;
    for (size_t f = 0; f < state.fault_count && fault_row_count < MAX_FAULT_ROWS; f++) {
        if (state.faults[f].start <= s.timestamp &&
            s.timestamp <= fault_end(state.faults[f])) {
            snprintf(fault_rows[fault_row_count++], sizeof(fault_rows[0]),
                     LV_SYMBOL_WARNING " %s", fault_code(state.faults[f]));
        }
    }

    // The run under the cursor: "…2:00 AM – 2:25 AM" and its length.
    char span[48] = "", length[16] = "";
    const bool has_run = format_cursor_run(i, span, sizeof(span), length, sizeof(length));

    static constexpr int32_t TIME_H = 30, ROW_H = 30, PAD = 10;
    const int32_t BOX_W = has_run ? 300 : 250;
    const int32_t BOX_H = 6 + TIME_H + (4 + (has_run ? 1 : 0) + fault_row_count) * ROW_H + 8;
    int32_t bx = x + 16;
    if (bx + BOX_W > plot.x2) bx = x - 16 - BOX_W;
    if (bx < plot.x1) bx = plot.x1;
    const lv_area_t box = {bx, plot.y1 + 4, bx + BOX_W, plot.y1 + 4 + BOX_H};
    if (bounds) cursor_bounds(x, plot, box, bounds);
    if (layer == nullptr) return;

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = UI_COLOR_TEXT_DIM;
    line.width = 2;
    line.dash_width = 6;
    line.dash_gap = 5;
    line.p1.x = x; line.p1.y = plot.y1;
    line.p2.x = x; line.p2.y = plot.y2;
    lv_draw_line(layer, &line);

    if (s.flags & HISTORY_TELEMETRY_SETPOINT_VALID) {
        draw_dot(layer, x, map_y(s.setpoint_deci_c, min_value, max_value, plot),
                 COLOR_SETPOINT);
    }
    if (state.interp_outlet && state.interp_outlet[i] != NO_VALUE) {
        draw_dot(layer, x, map_y(state.interp_outlet[i], min_value, max_value, plot),
                 COLOR_OUTLET);
    }
    if (state.interp_inlet && state.interp_inlet[i] != NO_VALUE) {
        draw_dot(layer, x, map_y(state.interp_inlet[i], min_value, max_value, plot),
                 COLOR_INLET);
    }

    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = COLOR_BG;
    bg.bg_opa = LV_OPA_90;
    bg.radius = 10;
    bg.border_color = COLOR_GRID;
    bg.border_width = 1;
    lv_draw_rect(layer, &bg, &box);

    const lv_area_t time_row = {box.x1 + PAD, box.y1 + 6, box.x2 - PAD,
                                box.y1 + 6 + TIME_H};
    draw_label(layer, time_row, when, UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_LEFT,
               &montserrat_24_latin);
    lv_area_t row = {time_row.x1, time_row.y2 - ROW_H, time_row.x2, time_row.y2};
    row.y1 += ROW_H;
    row.y2 += ROW_H;
    draw_state_row(layer, row, s, length);
    if (has_run) {
        row.y1 += ROW_H;
        row.y2 += ROW_H;
        draw_label(layer, row, span, UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_LEFT,
                   &montserrat_24_latin);
    }
    struct Row { lv_color_t color; const char* name; const char* value; };
    const Row rows[] = {
        {COLOR_OUTLET, i18n_get(STR_HISTORY_OUTLET), outlet},
        {COLOR_INLET, i18n_get(STR_HISTORY_INLET), inlet},
        {COLOR_SETPOINT, i18n_get(STR_HISTORY_SETPOINT), setpoint},
    };
    for (const auto& r : rows) {
        row.y1 += ROW_H;
        row.y2 += ROW_H;
        lv_draw_rect_dsc_t sw;
        lv_draw_rect_dsc_init(&sw);
        sw.bg_color = r.color;
        sw.radius = 2;
        const int32_t cy = (row.y1 + row.y2) / 2;
        const lv_area_t swatch = {row.x1, cy - 2, row.x1 + 16, cy + 2};
        lv_draw_rect(layer, &sw, &swatch);
        lv_area_t name_area = {row.x1 + 24, row.y1, row.x2, row.y2};
        draw_label(layer, name_area, r.name, UI_COLOR_TEXT, LV_TEXT_ALIGN_LEFT,
                   &montserrat_24_latin);
        draw_label(layer, row, r.value, UI_COLOR_TEXT, LV_TEXT_ALIGN_RIGHT,
                   &montserrat_24_latin);
    }
    for (int f = 0; f < fault_row_count; f++) {
        row.y1 += ROW_H;
        row.y2 += ROW_H;
        draw_label(layer, row, fault_rows[f], COLOR_FAULT, LV_TEXT_ALIGN_LEFT,
                   &montserrat_24_latin);
    }
}

// A bar along the top edge per fault, labelled with its code. Faults whose
// labels would collide share one label: the code if they are all the same
// fault, otherwise the count ("×3").
static void draw_fault_zones(lv_layer_t* layer, const lv_area_t& plot) {
    for (size_t i = 0; i < state.fault_count; i++) {
        const auto& f = state.faults[i];
        chart_draw_fault_zone(layer, map_x(f.start, plot),
                              map_x(fault_end(f), plot), plot.y1, plot.y2,
                              COLOR_FAULT);
    }
}

// Defrost: an icy block over the run it interrupts, at least 3 px wide so a
// short one still shows at wide zoom.
static void draw_defrost_zones(lv_layer_t* layer, const lv_area_t& plot) {
    for (size_t i = 0; i < state.defrost_count; i++) {
        const auto& d = state.defrosts[i];
        int32_t x1 = map_x(d.start, plot);
        int32_t x2 = map_x(d.end != 0 ? d.end : state.window_end, plot);
        if (x2 - x1 < 3) x2 = x1 + 3;
        x1 = LV_MAX(x1, plot.x1);
        x2 = LV_MIN(x2, plot.x2);
        if (x2 <= x1) continue;
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = COLOR_DEFROST;
        dsc.bg_opa = LV_OPA_40;
        const lv_area_t zone = {x1, plot.y1, x2, plot.y2};
        lv_draw_rect(layer, &dsc, &zone);
    }
}

static void draw_fault_labels(lv_layer_t* layer, const lv_area_t& plot) {
    static constexpr int32_t LABEL_W = 100;
    size_t i = 0;
    while (i < state.fault_count) {
        const int32_t x = map_x(state.faults[i].start, plot);
        size_t j = i + 1;
        bool same_code = true;
        while (j < state.fault_count &&
               map_x(state.faults[j].start, plot) < x + LABEL_W) {
            if (state.faults[j].site != state.faults[i].site) same_code = false;
            j++;
        }
        char text[24];
        if (same_code) {
            snprintf(text, sizeof(text), LV_SYMBOL_WARNING " %s",
                     fault_code(state.faults[i]));
        } else {
            snprintf(text, sizeof(text), LV_SYMBOL_WARNING " \xC3\x97%u",
                     (unsigned)(j - i));
        }
        int32_t lx = x + 6;
        if (lx + LABEL_W > plot.x2) lx = plot.x2 - LABEL_W;
        const lv_area_t label = {lx, plot.y1 - FAULT_STRIP_H + 4, lx + LABEL_W,
                                 plot.y1 - 4};
        draw_label(layer, label, text, COLOR_FAULT, LV_TEXT_ALIGN_LEFT,
                   &montserrat_24_latin);
        // Carry the fault line up into the strip so the code flags it.
        for (size_t k = i; k < j; k++) {
            const int32_t fx = map_x(state.faults[k].start, plot);
            draw_line(layer, {fx, plot.y1 - FAULT_STRIP_H + 8}, {fx, plot.y1},
                      COLOR_FAULT, 2);
        }
        i = j;
    }
}

static void draw_mode_bands(lv_layer_t* layer, const lv_area_t& plot) {
    for (size_t i = 0; i < state.sample_count; i++) {
        const auto& first = state.samples[i];
        if (!running_sample(first)) continue;
        uint32_t band_start = first.timestamp;
        uint32_t band_end = first.timestamp + HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC;
        size_t j = i + 1;
        while (j < state.sample_count) {
            const auto& next = state.samples[j];
            const auto& previous = state.samples[j - 1];
            if (!running_sample(next) || next.mode != first.mode ||
                next.timestamp - previous.timestamp > CONTIGUOUS_SECONDS) {
                break;
            }
            band_end = next.timestamp + HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC;
            j++;
        }
        lv_area_t band = {
            map_x(band_start, plot), plot.y1,
            map_x(band_end, plot), plot.y2,
        };
        lv_draw_rect_dsc_t band_dsc;
        lv_draw_rect_dsc_init(&band_dsc);
        band_dsc.bg_color = mode_color(first.mode);
        band_dsc.bg_opa = LV_OPA_20;
        lv_draw_rect(layer, &band_dsc, &band);
        i = j - 1;
    }
}

// X-axis gridlines/labels are anchored to round local-time boundaries
// (multiples of the zoom's tick step: 2 h at 8 h down to 15 min at 1 h)
// rather than to the ragged window edges, so the axis always reads in clean
// times (e.g. 08:00, 10:00, 12:00) regardless of the current minute. The data
// window itself still ends at "now".
static void draw_time_axis(lv_layer_t* layer, const lv_area_t& coords,
                           const lv_area_t& plot) {
    const uint32_t tick_step = history_window_tick(state.window_seconds);
    time_t tick_time;
    {
        time_t start = state.window_start;
        struct tm t0 = {};
        localtime_r(&start, &t0);
        // Round up to the next multiple of the step since local midnight.
        const uint32_t since_midnight =
            (uint32_t)(t0.tm_hour * 3600 + t0.tm_min * 60 + t0.tm_sec);
        tick_time = start + (tick_step - since_midnight % tick_step) % tick_step;
    }
    for (; tick_time > 0 && (uint32_t)tick_time <= state.window_end;
         tick_time += tick_step) {
        if ((uint32_t)tick_time < state.window_start) continue;
        int32_t x = map_x((uint32_t)tick_time, plot);
        draw_line(layer, {x, plot.y1}, {x, plot.y2}, COLOR_GRID, 1);
        struct tm local = {};
        localtime_r(&tick_time, &local);
        char label[12];
        if (time_mgr_get_24h_format()) {
            strftime(label, sizeof(label), "%H:%M", &local);
        } else {
            // On the hour the minutes carry no information in 12-hour form --
            // "8 AM" rather than "8:00 AM".
            int hour12 = local.tm_hour % 12;
            if (hour12 == 0) hour12 = 12;
            const char* suffix = local.tm_hour < 12 ? "AM" : "PM";
            if (local.tm_min == 0) {
                snprintf(label, sizeof(label), "%d %s", hour12, suffix);
            } else {
                snprintf(label, sizeof(label), "%d:%02d %s", hour12,
                         local.tm_min, suffix);
            }
        }
        lv_area_t label_area = {x - 60, plot.y2 + 10, x + 60, coords.y2};
        draw_label(layer, label_area, label, UI_COLOR_TEXT_DIM,
                   LV_TEXT_ALIGN_CENTER);
    }
}

// Tenths of a unit as "12.5" (or "12" when whole).
static void format_tenths(char* buf, size_t len, uint32_t tenths, bool keep_decimal) {
    if (!keep_decimal && tenths % 10 == 0) {
        snprintf(buf, len, "%lu", (unsigned long)(tenths / 10));
    } else {
        snprintf(buf, len, "%lu.%lu", (unsigned long)(tenths / 10),
                 (unsigned long)(tenths % 10));
    }
}

static void draw_power_cursor(lv_layer_t* layer, const lv_area_t& plot,
                              int32_t max_w, lv_area_t* bounds = nullptr) {
    if (state.cursor < 0 || (size_t)state.cursor >= state.sample_count) return;
    const auto& s = state.samples[state.cursor];
    if (!(s.flags & HISTORY_TELEMETRY_POWER_VALID)) return;
    const int32_t x = map_x(s.timestamp, plot);

    char when[12];
    format_clock(when, sizeof(when), s.timestamp);
    char kw[16], power[24];
    format_tenths(kw, sizeof(kw), ((uint32_t)s.average_power_w + 50) / 100, true);
    snprintf(power, sizeof(power), "%s kW", kw);

    static constexpr int32_t TIME_H = 30, ROW_H = 30, PAD = 10;
    char span[48] = "", length[16] = "";
    const bool has_run = format_cursor_run((size_t)state.cursor, span, sizeof(span),
                                           length, sizeof(length));
    const int32_t BOX_W = has_run ? 300 : 250;
    const int32_t BOX_H = 6 + TIME_H + (has_run ? 3 : 2) * ROW_H + 8;
    int32_t bx = x + 16;
    if (bx + BOX_W > plot.x2) bx = x - 16 - BOX_W;
    if (bx < plot.x1) bx = plot.x1;
    const lv_area_t box = {bx, plot.y1 + 4, bx + BOX_W, plot.y1 + 4 + BOX_H};
    if (bounds) cursor_bounds(x, plot, box, bounds);
    if (layer == nullptr) return;

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = UI_COLOR_TEXT_DIM;
    line.width = 2;
    line.dash_width = 6;
    line.dash_gap = 5;
    line.p1.x = x; line.p1.y = plot.y1;
    line.p2.x = x; line.p2.y = plot.y2;
    lv_draw_line(layer, &line);
    draw_dot(layer, x, map_y(s.average_power_w, 0, max_w, plot), COLOR_POWER);

    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = COLOR_BG;
    bg.bg_opa = LV_OPA_90;
    bg.radius = 10;
    bg.border_color = COLOR_GRID;
    bg.border_width = 1;
    lv_draw_rect(layer, &bg, &box);

    lv_area_t row = {box.x1 + PAD, box.y1 + 6, box.x2 - PAD, box.y1 + 6 + TIME_H};
    draw_label(layer, row, when, UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_LEFT,
               &montserrat_24_latin);
    row.y1 += TIME_H;
    row.y2 = row.y1 + ROW_H;
    draw_label(layer, row, power, COLOR_POWER, LV_TEXT_ALIGN_LEFT,
               &montserrat_24_latin);
    row.y1 += ROW_H;
    row.y2 += ROW_H;
    draw_state_row(layer, row, s, length);
    if (has_run) {
        row.y1 += ROW_H;
        row.y2 += ROW_H;
        draw_label(layer, row, span, UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_LEFT,
                   &montserrat_24_latin);
    }
}

// Electrical draw: an area per pixel column (the tallest sample in it) under
// a line, broken wherever samples are missing.
static void draw_power_static(lv_layer_t* layer, const lv_area_t& coords,
                              const lv_area_t& plot) {
    uint32_t peak = 0;
    for (size_t i = 0; i < state.sample_count; i++) {
        const auto& s = state.samples[i];
        if ((s.flags & HISTORY_TELEMETRY_POWER_VALID) && s.average_power_w > peak) {
            peak = s.average_power_w;
        }
    }
    // Tenths of a kW, so the round steps read 0.5 / 1 / 2 / 5 kW. A little
    // headroom keeps the peak off the top edge.
    const ChartAxis axis =
        chart_axis_nice(0, LV_MAX((int32_t)((peak + peak / 20 + 99) / 100), 10), 5);
    const int32_t max_w = axis.hi * 100;

    draw_mode_bands(layer, plot);
    draw_defrost_zones(layer, plot);
    draw_fault_zones(layer, plot);
    for (int32_t value = axis.lo; value <= axis.hi; value += axis.step) {
        const int32_t y = map_y(value * 100, 0, max_w, plot);
        draw_line(layer, {plot.x1, y}, {plot.x2, y}, COLOR_GRID, 1);
        char num[12], label[20];
        format_tenths(num, sizeof(num), (uint32_t)value, false);
        snprintf(label, sizeof(label), "%s kW", num);
        lv_area_t label_area = {coords.x1, y - 12, plot.x1 - 8, y + 12};
        draw_label(layer, label_area, label, UI_COLOR_TEXT_DIM,
                   LV_TEXT_ALIGN_RIGHT);
    }
    draw_time_axis(layer, coords, plot);

    lv_draw_rect_dsc_t fill;
    lv_draw_rect_dsc_init(&fill);
    fill.bg_color = COLOR_POWER;
    fill.bg_opa = LV_OPA_30;
    bool have_column = false;
    int32_t col_x = 0, col_top = 0;
    bool have_previous = false;
    uint32_t prev_t = 0;
    lv_point_t prev_p = {0, 0};
    for (size_t i = 0; i < state.sample_count; i++) {
        const auto& s = state.samples[i];
        if (!(s.flags & HISTORY_TELEMETRY_POWER_VALID)) {
            have_previous = false;
            continue;
        }
        const lv_point_t p = {map_x(s.timestamp, plot),
                              map_y(s.average_power_w, 0, max_w, plot)};
        const bool contiguous =
            have_previous && s.timestamp - prev_t <= CONTIGUOUS_SECONDS;
        if (have_column && p.x != col_x) {
            // Fill to the next column only when the samples join up.
            const int32_t right = contiguous ? p.x - 1 : col_x;
            const lv_area_t area = {col_x, col_top, LV_MAX(right, col_x), plot.y2};
            lv_draw_rect(layer, &fill, &area);
            have_column = false;
        }
        if (!have_column) {
            have_column = true;
            col_x = p.x;
            col_top = p.y;
        } else if (p.y < col_top) {
            col_top = p.y;
        }
        if (contiguous) draw_line(layer, prev_p, p, COLOR_POWER, 3);
        have_previous = true;
        prev_t = s.timestamp;
        prev_p = p;
    }
    if (have_column) {
        const lv_area_t area = {col_x, col_top, col_x, plot.y2};
        lv_draw_rect(layer, &fill, &area);
    }

    draw_fault_labels(layer, plot);
    s_power_max_w = max_w;
}

static void chart_draw_cb_impl(lv_event_t* event);
static void chart_draw_cb(lv_event_t* event) {
#if CONFIG_TEST_ENDPOINTS
    const int64_t t0 = esp_timer_get_time();
    chart_draw_cb_impl(event);
    s_prof.total_us += esp_timer_get_time() - t0;
    s_prof.calls++;
#else
    chart_draw_cb_impl(event);
#endif
}

static void chart_draw_cb_impl(lv_event_t* event) {
    lv_obj_t* chart = (lv_obj_t*)lv_event_get_target(event);
    lv_layer_t* layer = lv_event_get_layer(event);
    lv_area_t coords;
    lv_obj_get_coords(chart, &coords);
    const lv_area_t plot = plot_area(chart);
    const bool cached = static_cache_ready();
    if (cached && s_drag.panning && s_drag.dx != 0) {
        // Axis labels stay put; the plot band (data, bands, time axis)
        // slides with the finger.
        lv_draw_image_dsc_t image;
        lv_draw_image_dsc_init(&image);
        image.src = &s_static.buf;
        const lv_area_t saved = layer->_clip_area;
        const lv_area_t fixed[2] = {
            {coords.x1, coords.y1, plot.x1 - 1, coords.y2},
            {plot.x2 + 1, coords.y1, coords.x2, coords.y2},
        };
        for (const auto& part : fixed) {
            if (lv_area_intersect(&layer->_clip_area, &saved, &part)) {
                lv_draw_image(layer, &image, &coords);
            }
        }
        const lv_area_t band = {plot.x1, coords.y1, plot.x2, coords.y2};
        if (lv_area_intersect(&layer->_clip_area, &saved, &band)) {
            lv_area_t shifted = coords;
            lv_area_move(&shifted, s_drag.dx, 0);
            lv_draw_image(layer, &image, &shifted);
        }
        layer->_clip_area = saved;
        return;
    }
    if (cached) {
        lv_draw_image_dsc_t image;
        lv_draw_image_dsc_init(&image);
        image.src = &s_static.buf;
        lv_draw_image(layer, &image, &coords);
    } else {
        draw_static(layer, coords, plot);
    }
#if CONFIG_TEST_ENDPOINTS
    s_prof.cached = cached;
#endif
    if (state.power_view) {
        draw_power_cursor(layer, plot, s_power_max_w);
    } else {
        draw_cursor(layer, plot, s_temp_min, s_temp_max);
    }
}

static bool cursor_dirty_area(lv_area_t* out) {
    if (state.chart == nullptr) return false;
    const lv_area_t plot = plot_area(state.chart);
    lv_area_t area[2] = {{0, 0, -1, -1}, {0, 0, -1, -1}};
    if (state.power_view) {
        draw_power_cursor(nullptr, plot, s_power_max_w, area);
    } else {
        draw_cursor(nullptr, plot, s_temp_min, s_temp_max, area);
    }
    if (area[0].x2 < area[0].x1) return false;
    out[0] = area[0];
    out[1] = area[1];
    return true;
}

static void draw_static(lv_layer_t* layer, const lv_area_t& coords,
                        const lv_area_t& plot) {
    if (state.power_view) {
        draw_power_static(layer, coords, plot);
        return;
    }

    int32_t min_value = INT32_MAX;
    int32_t max_value = INT32_MIN;
    for (size_t i = 0; i < state.sample_count; i++) {
        const auto& sample = state.samples[i];
        int32_t values[3] = {
            sample.inlet_deci_c,
            sample.outlet_deci_c,
            sample.setpoint_deci_c,
        };
        uint8_t flags[3] = {
            HISTORY_TELEMETRY_INLET_VALID,
            HISTORY_TELEMETRY_OUTLET_VALID,
            HISTORY_TELEMETRY_SETPOINT_VALID,
        };
        for (int j = 0; j < 3; j++) {
            if (!(sample.flags & flags[j])) continue;
            if (values[j] < min_value) min_value = values[j];
            if (values[j] > max_value) max_value = values[j];
        }
    }
    if (min_value == INT32_MAX) {
        min_value = 0;
        max_value = 500;
    } else {
        int32_t span = max_value - min_value;
        if (span < 100) {
            int32_t center = (min_value + max_value) / 2;
            min_value = center - 50;
            max_value = center + 50;
        }
        int32_t padding = (max_value - min_value) / 10;
        if (padding < 20) padding = 20;
        min_value -= padding;
        max_value += padding;
    }
    // Gridlines on round degrees of the display unit.
    const ChartAxis axis =
        chart_axis_nice(display_deci_c(min_value), display_deci_c(max_value));
    min_value = deci_c_from_display(axis.lo);
    max_value = deci_c_from_display(axis.hi);
    s_temp_min = min_value;
    s_temp_max = max_value;

    draw_mode_bands(layer, plot);
    draw_defrost_zones(layer, plot);
    draw_fault_zones(layer, plot);

    for (int32_t value = axis.lo; value <= axis.hi; value += axis.step) {
        int32_t y = plot.y2 -
            (int32_t)((int64_t)(value - axis.lo) * lv_area_get_height(&plot) /
                      (axis.hi - axis.lo));
        draw_line(layer, {plot.x1, y}, {plot.x2, y}, COLOR_GRID, 1);
        char label[16];
        snprintf(label, sizeof(label), "%ld°", (long)(value / 10));
        lv_area_t label_area = {coords.x1, y - 12, plot.x1 - 8, y + 12};
        draw_label(layer, label_area, label, UI_COLOR_TEXT_DIM,
                   LV_TEXT_ALIGN_RIGHT);
    }
    draw_time_axis(layer, coords, plot);

    struct Series {
        uint8_t valid_flag;
        int16_t history_telemetry_sample_t::* value;
        lv_color_t color;
        bool step;
        const int32_t* interp;
    };
    const Series series[] = {
        {HISTORY_TELEMETRY_INLET_VALID,
         &history_telemetry_sample_t::inlet_deci_c, COLOR_INLET, false,
         state.interp_inlet},
        {HISTORY_TELEMETRY_OUTLET_VALID,
         &history_telemetry_sample_t::outlet_deci_c, COLOR_OUTLET, false,
         state.interp_outlet},
        {HISTORY_TELEMETRY_SETPOINT_VALID,
         &history_telemetry_sample_t::setpoint_deci_c, COLOR_SETPOINT, true,
         nullptr},
    };

    for (const auto& item : series) {
        bool have_previous = false;
        history_telemetry_sample_t previous = {};
        int32_t previous_value = 0;
        for (size_t i = 0; i < state.sample_count; i++) {
            const auto& current = state.samples[i];
            int32_t current_value;
            if (!sample_value(current, item.valid_flag, current.*(item.value),
                              &current_value)) {
                have_previous = false;
                continue;
            }
            if (item.interp && item.interp[i] != NO_VALUE) {
                current_value = item.interp[i];
            }
            if (have_previous &&
                current.timestamp - previous.timestamp <= CONTIGUOUS_SECONDS) {
                lv_point_t p1 = {
                    map_x(previous.timestamp, plot),
                    map_y(previous_value, min_value, max_value, plot),
                };
                lv_point_t p2 = {
                    map_x(current.timestamp, plot),
                    map_y(current_value, min_value, max_value, plot),
                };
                if (item.step) {
                    lv_point_t corner = {p2.x, p1.y};
                    draw_line(layer, p1, corner, item.color, 3);
                    draw_line(layer, corner, p2, item.color, 3);
                } else {
                    draw_line(layer, p1, p2, item.color, 3);
                }
            }
            previous = current;
            previous_value = current_value;
            have_previous = true;
        }
    }

    draw_fault_labels(layer, plot);
}

#if CONFIG_TEST_ENDPOINTS
// Test hook: move the cursor as a finger at screen x would, then force a
// refresh and time both. Caller holds the display lock.
extern "C" bool heatpump_history_profile_cursor(int32_t x,
                                                history_cursor_profile_t* out) {
    if (state.chart == nullptr || state.sample_count == 0) return false;
    memset(&s_prof, 0, sizeof(s_prof));
    int64_t t0 = esp_timer_get_time();
    set_cursor(cursor_at(x));
    out->press_us = esp_timer_get_time() - t0;
    t0 = esp_timer_get_time();
    lv_refr_now(nullptr);
    out->refr_us = esp_timer_get_time() - t0;
    out->cursor = state.cursor;
    out->samples = (uint32_t)state.sample_count;
    out->power_view = state.power_view;
    out->draw_calls = s_prof.calls;
    out->draw_us = s_prof.total_us;
    out->build_us = s_prof.build_us;
    out->cached = s_prof.cached;
    return true;
}
#endif

static void update_range_label() {
    time_t start = state.window_start;
    time_t end = state.window_end;
    struct tm start_tm = {};
    struct tm end_tm = {};
    localtime_r(&start, &start_tm);
    localtime_r(&end, &end_tm);
    char start_text[32];
    char end_text[32];
    // The window edges are ragged (the range ends at "now"), so unlike the axis
    // ticks the minutes are meaningful here and the full form is used.
    const bool use_24h = time_mgr_get_24h_format();
    i18n_format_date_time(start_text, sizeof(start_text), &start_tm, use_24h);
    i18n_format_date_time(end_text, sizeof(end_text), &end_tm, use_24h);
    char range[80];
    snprintf(range, sizeof(range), "%s - %s", start_text, end_text);
    lv_label_set_text(state.range_label, range);
}

static void update_navigation() {
    const uint32_t earliest =
        state.latest_end > RETENTION_SECONDS
            ? state.latest_end - RETENTION_SECONDS
            : 0;
    if (state.window_start <= earliest) {
        lv_obj_add_state(state.previous_button, LV_STATE_DISABLED);
        lv_obj_add_state(state.back_hour_button, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(state.previous_button, LV_STATE_DISABLED);
        lv_obj_remove_state(state.back_hour_button, LV_STATE_DISABLED);
    }
    if (state.window_end >= state.latest_end) {
        lv_obj_add_state(state.next_button, LV_STATE_DISABLED);
        lv_obj_add_state(state.forward_hour_button, LV_STATE_DISABLED);
        lv_obj_add_flag(state.latest_button, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_state(state.next_button, LV_STATE_DISABLED);
        lv_obj_remove_state(state.forward_hour_button, LV_STATE_DISABLED);
        lv_obj_remove_flag(state.latest_button, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_power_label() {
    if (state.power_label == nullptr) return;
    char kwh[16];
    format_tenths(kwh, sizeof(kwh), (state.window_wh + 50) / 100, true);
    char amount[48];
    snprintf(amount, sizeof(amount), i18n_get(STR_HISTORY_POWER_WINDOW_FMT), kwh);
    char text[80];
    snprintf(text, sizeof(text), "%s \xc2\xb7 %s", i18n_get(STR_HISTORY_VIEW_POWER),
             amount);
    lv_label_set_text(state.power_label, text);
}

static void start_pending_query_if_needed() {
    if (!state.shown || !state.pending_query || state.query_running) return;
    uint32_t end = state.pending_end;
    state.pending_query = false;
    request_window(end);
}

static void query_complete(void* data) {
    auto* result = static_cast<QueryResult*>(data);
    state.query_running = false;
    if (!state.shown || result->generation != state.generation) {
        heap_caps_free(result->samples);
        heap_caps_free(result->defrosts);
        delete result;
        start_pending_query_if_needed();
        return;
    }

    heap_caps_free(state.samples);
    state.samples = result->samples;
    state.sample_count = result->count;
    state.cursor = -1;
    state.fault_count = result->fault_count;
    memcpy(state.faults, result->faults, sizeof(fault_interval_t) * result->fault_count);
    heap_caps_free(state.defrosts);
    state.defrosts = result->defrosts;
    state.defrost_count = result->defrosts ? result->defrost_count : 0;
    heap_caps_free(state.interp_inlet);
    heap_caps_free(state.interp_outlet);
    state.interp_inlet = nullptr;
    state.interp_outlet = nullptr;
    if (state.sample_count > 0) {
        state.interp_inlet = static_cast<int32_t*>(heap_caps_malloc(
            sizeof(int32_t) * state.sample_count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        state.interp_outlet = static_cast<int32_t*>(heap_caps_malloc(
            sizeof(int32_t) * state.sample_count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (state.interp_inlet == nullptr || state.interp_outlet == nullptr) {
            // Fall back to drawing the raw readings.
            heap_caps_free(state.interp_inlet);
            heap_caps_free(state.interp_outlet);
            state.interp_inlet = nullptr;
            state.interp_outlet = nullptr;
        }
        recompute_interpolation();
    }
    {
        uint64_t joules = 0;
        for (size_t i = 0; i < state.sample_count; i++) {
            const auto& s = state.samples[i];
            if (s.flags & HISTORY_TELEMETRY_POWER_VALID) {
                joules += (uint64_t)s.average_power_w *
                          HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC;
            }
        }
        state.window_wh = (uint32_t)((joules + 1800) / 3600);
        update_power_label();
    }
    if (result->error != ESP_OK) {
        lv_label_set_text(state.status_label,
                          i18n_get(STR_HISTORY_STORAGE_ERROR));
        lv_obj_remove_flag(state.status_label, LV_OBJ_FLAG_HIDDEN);
    } else if (result->count == 0) {
        lv_label_set_text(state.status_label,
                          time_mgr_is_synced()
                              ? i18n_get(STR_HISTORY_NO_DATA)
                              : i18n_get(STR_HISTORY_WAITING_FOR_TIME));
        lv_obj_remove_flag(state.status_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(state.status_label, LV_OBJ_FLAG_HIDDEN);
    }
    s_data_gen++;
    // Build the cached image now rather than on the first touch, so a quick
    // swipe isn't swallowed by a 150+ ms build at the start of the pan.
    lv_obj_update_layout(state.chart);
    static_cache_build();
    lv_obj_invalidate(state.chart);
    delete result;
    start_pending_query_if_needed();
}

static void query_task(void* data) {
    auto* result = static_cast<QueryResult*>(data);
    result->samples = static_cast<history_telemetry_sample_t*>(
        heap_caps_malloc(sizeof(history_telemetry_sample_t) *
                             HISTORY_TELEMETRY_PAGE_CAPACITY,
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (result->samples == nullptr) {
        result->error = ESP_ERR_NO_MEM;
        result->count = 0;
    } else {
        result->error = history_storage_query_telemetry(
            result->start, result->end, result->samples,
            HISTORY_TELEMETRY_PAGE_CAPACITY, &result->count);
    }
    result->defrosts = static_cast<fault_interval_t*>(heap_caps_malloc(
        sizeof(fault_interval_t) * MAX_DEFROSTS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    result->defrost_count = 0;
    result->fault_count = chart_spans_query(
        result->start, result->end, result->faults, MAX_FAULTS, nullptr,
        result->defrosts, result->defrosts ? MAX_DEFROSTS : 0,
        &result->defrost_count);
    // Worker task: lv_async_call() must hold the LVGL lock.
    bsp_display_lock(0);
    lv_async_call(query_complete, result);
    bsp_display_unlock();
    vTaskDelete(nullptr);
}

static void request_window(uint32_t end) {
    state.window_end = end;
    state.window_start = end - state.window_seconds;
    update_range_label();
    update_navigation();
    lv_label_set_text(state.status_label, i18n_get(STR_HISTORY_LOADING));
    lv_obj_remove_flag(state.status_label, LV_OBJ_FLAG_HIDDEN);

    state.generation++;
    if (state.query_running) {
        state.pending_query = true;
        state.pending_end = end;
        return;
    }

    auto* result = new QueryResult{
        state.generation, state.window_start, state.window_end,
        ESP_OK, 0, nullptr};
    state.query_running = true;
    if (xTaskCreate(query_task, "history_query", 4096, result, 3, nullptr) !=
        pdPASS) {
        state.query_running = false;
        delete result;
        lv_label_set_text(state.status_label,
                          i18n_get(STR_HISTORY_STORAGE_ERROR));
    }
}

static lv_obj_t* create_button(lv_obj_t* parent, const char* text,
                               const char* tag, lv_event_cb_t callback) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_height(button, 64);
    lv_obj_set_flex_grow(button, 1);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x3d4f6f), LV_PART_MAIN);
    lv_obj_set_style_radius(button, 12, LV_PART_MAIN);
    lv_obj_set_user_data(button, (void*)tag);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_center(label);
    return button;
}

static void previous_cb(lv_event_t*) {
    request_window(history_window_shift(state.window_end,
                                        -(int32_t)state.window_seconds,
                                        state.window_seconds, state.latest_end));
}

static void step_window(int32_t delta) {
    request_window(history_window_shift(state.window_end, delta,
                                        state.window_seconds, state.latest_end));
}

static void back_hour_cb(lv_event_t*) {
    step_window(-(int32_t)history_window_step(state.window_seconds));
}
static void forward_hour_cb(lv_event_t*) {
    step_window((int32_t)history_window_step(state.window_seconds));
}

// "1 h" or "15 min".
static void format_duration(char* buf, size_t len, uint32_t seconds) {
    if (seconds % 3600 == 0) {
        snprintf(buf, len, "%lu h", (unsigned long)(seconds / 3600));
    } else {
        snprintf(buf, len, "%lu min", (unsigned long)(seconds / 60));
    }
}

// Highlight the active zoom preset and label the fine steps to match it.
static void apply_zoom() {
    for (size_t i = 0; i < sizeof(HISTORY_WINDOW_PRESETS) /
                               sizeof(HISTORY_WINDOW_PRESETS[0]); i++) {
        lv_obj_t* button = state.zoom_buttons[i];
        if (button == nullptr) continue;
        const bool on = HISTORY_WINDOW_PRESETS[i] == state.window_seconds;
        lv_obj_set_style_bg_color(button,
                                  on ? UI_COLOR_ACCENT : lv_color_hex(0x3d4f6f),
                                  LV_PART_MAIN);
        lv_obj_set_style_text_color(lv_obj_get_child(button, 0),
                                    on ? lv_color_hex(0x062033) : UI_COLOR_TEXT,
                                    LV_PART_MAIN);
    }
    char step[16], text[32];
    format_duration(step, sizeof(step), state.window_seconds);
    if (state.previous_button) {
        snprintf(text, sizeof(text), LV_SYMBOL_LEFT " %s", step);
        lv_label_set_text(lv_obj_get_child(state.previous_button, 0), text);
    }
    if (state.next_button) {
        snprintf(text, sizeof(text), "%s " LV_SYMBOL_RIGHT, step);
        lv_label_set_text(lv_obj_get_child(state.next_button, 0), text);
    }
    format_duration(step, sizeof(step), history_window_step(state.window_seconds));
    if (state.back_hour_button) {
        snprintf(text, sizeof(text), LV_SYMBOL_LEFT " %s", step);
        lv_label_set_text(lv_obj_get_child(state.back_hour_button, 0), text);
    }
    if (state.forward_hour_button) {
        snprintf(text, sizeof(text), "%s " LV_SYMBOL_RIGHT, step);
        lv_label_set_text(lv_obj_get_child(state.forward_hour_button, 0), text);
    }
}

// Zoom keeps the window's end, so the newest data stays in view from the
// live window and an older window zooms around its right edge.
static void zoom_cb(lv_event_t* event) {
    const auto* target = static_cast<lv_obj_t*>(lv_event_get_target(event));
    uint32_t window = 0;
    for (size_t i = 0; i < sizeof(HISTORY_WINDOW_PRESETS) /
                               sizeof(HISTORY_WINDOW_PRESETS[0]); i++) {
        if (state.zoom_buttons[i] == target) window = HISTORY_WINDOW_PRESETS[i];
    }
    if (window == 0 || window == state.window_seconds) return;
    state.window_seconds = window;
    apply_zoom();
    request_window(history_window_shift(state.window_end, 0, window,
                                        state.latest_end));
}

// Double-tap: re-centre the window on the tapped moment, e.g. to see a run
// that straddled two windows in one piece.
static void chart_double_click_cb(lv_event_t*) {
    // Two quick pans in a row aren't a double-tap.
    if (s_drag.panning || lv_tick_elaps(s_drag.ended_ms) < 600) return;
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    const lv_area_t plot = plot_area(state.chart);
    if (point.x < plot.x1 || point.x > plot.x2) return;
    const uint32_t t = state.window_start +
        (uint32_t)((uint64_t)(point.x - plot.x1) * state.window_seconds /
                   (uint32_t)lv_area_get_width(&plot));
    request_window(history_window_center(t, state.window_seconds,
                                         history_window_snap(state.window_seconds),
                                         state.latest_end));
}

static void next_cb(lv_event_t*) {
    uint32_t end = state.window_end + state.window_seconds;
    if (end > state.latest_end) end = state.latest_end;
    request_window(end);
}

static void latest_cb(lv_event_t*) {
    request_window(state.latest_end);
}

static void apply_view() {
    if (state.view_temps_button == nullptr) return;
    lv_obj_t* buttons[2] = {state.view_temps_button, state.view_power_button};
    for (int i = 0; i < 2; i++) {
        const bool on = (i == 1) == state.power_view;
        lv_obj_set_style_bg_color(buttons[i],
                                  on ? UI_COLOR_ACCENT : lv_color_hex(0x3d4f6f),
                                  LV_PART_MAIN);
        lv_obj_set_style_text_color(lv_obj_get_child(buttons[i], 0),
                                    on ? lv_color_hex(0x062033) : UI_COLOR_TEXT,
                                    LV_PART_MAIN);
    }
    if (state.power_view) {
        lv_obj_add_flag(state.legend_lines, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(state.legend_power, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(state.legend_lines, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(state.legend_power, LV_OBJ_FLAG_HIDDEN);
    }
    state.cursor = -1;
    if (state.sample_count > 0) {
        lv_obj_update_layout(state.chart);
        static_cache_build();
    }
    lv_obj_invalidate(state.chart);
}

static void view_temps_cb(lv_event_t*) {
    state.power_view = false;
    apply_view();
}

static void view_power_cb(lv_event_t*) {
    state.power_view = true;
    apply_view();
}

// Lines get a thin bar swatch, mode bands a filled block, so the two legend
// rows read like a chart key.
static lv_obj_t* add_legend_item(lv_obj_t* parent, lv_color_t color,
                                 const char* text, bool band) {
    lv_obj_t* item = lv_obj_create(parent);
    lv_obj_remove_style_all(item);
    lv_obj_set_size(item, LV_SIZE_CONTENT, 34);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(item, 8, LV_PART_MAIN);

    lv_obj_t* swatch = lv_obj_create(item);
    lv_obj_set_size(swatch, band ? 20 : 26, band ? 20 : 6);
    lv_obj_set_style_bg_color(swatch, color, LV_PART_MAIN);
    lv_obj_set_style_border_width(swatch, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(swatch, band ? 4 : 3, LV_PART_MAIN);

    lv_obj_t* label = lv_label_create(item);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &montserrat_24_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT_DIM, LV_PART_MAIN);
    return label;
}

lv_obj_t* heatpump_history_create_in(lv_obj_t* parent) {
    if (state.shown || parent == nullptr) return nullptr;
    state.shown = true;
    state.generation++;

    state.overlay = lv_obj_create(parent);
    lv_obj_set_size(state.overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_align(state.overlay, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(state.overlay, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state.overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(state.overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.overlay, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_top(state.overlay, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(state.overlay, NAV_BAR_H + 18, LV_PART_MAIN);
    lv_obj_set_flex_flow(state.overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(state.overlay, 12, LV_PART_MAIN);
    lv_obj_clear_flag(state.overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(state.overlay, (void*)"temperature_history_screen");

    // Header: range text, plus a "Latest" jump that only shows when the
    // window is behind the newest data. Fixed height so the chart doesn't
    // move when it appears.
    lv_obj_t* header = lv_obj_create(state.overlay);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), LATEST_BUTTON_H);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, 12, LV_PART_MAIN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    state.range_label = lv_label_create(header);
    lv_obj_set_flex_grow(state.range_label, 1);
    lv_obj_set_style_text_font(state.range_label, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.range_label, UI_COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_align(state.range_label, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_set_user_data(state.range_label,
                         (void*)"temperature_history_range");

    char latest_text[48];
    snprintf(latest_text, sizeof(latest_text), "%s  " LV_SYMBOL_NEXT,
             i18n_get(STR_HISTORY_LATEST));
    state.latest_button = create_button(header, latest_text,
                                        "temperature_history_latest", latest_cb);
    lv_obj_set_flex_grow(state.latest_button, 0);
    lv_obj_set_size(state.latest_button, LV_SIZE_CONTENT, LATEST_BUTTON_H);
    lv_obj_set_style_pad_hor(state.latest_button, 22, LV_PART_MAIN);
    lv_obj_set_style_bg_color(state.latest_button, UI_COLOR_ACCENT,
                              LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_obj_get_child(state.latest_button, 0),
                                lv_color_hex(0x062033), LV_PART_MAIN);
    lv_obj_add_flag(state.latest_button, LV_OBJ_FLAG_HIDDEN);

    // Temperatures | Power switch.
    lv_obj_t* views = lv_obj_create(state.overlay);
    lv_obj_remove_style_all(views);
    lv_obj_set_size(views, LV_PCT(100), 52);
    lv_obj_set_flex_flow(views, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(views, 12, LV_PART_MAIN);
    lv_obj_clear_flag(views, LV_OBJ_FLAG_SCROLLABLE);
    state.view_temps_button = create_button(
        views, i18n_get(STR_HISTORY_VIEW_TEMPS), "history_view_temps",
        view_temps_cb);
    state.view_power_button = create_button(
        views, i18n_get(STR_HISTORY_VIEW_POWER), "history_view_power",
        view_power_cb);
    lv_obj_set_height(state.view_temps_button, 52);
    lv_obj_set_height(state.view_power_button, 52);

    // Zoom presets, widest first.
    static const char* const kZoomTags[] = {"history_zoom_8h", "history_zoom_4h",
                                            "history_zoom_2h", "history_zoom_1h"};
    static_assert(sizeof(kZoomTags) / sizeof(kZoomTags[0]) ==
                      sizeof(HISTORY_WINDOW_PRESETS) /
                          sizeof(HISTORY_WINDOW_PRESETS[0]),
                  "one tag per zoom preset");
    lv_obj_t* zoom = lv_obj_create(state.overlay);
    lv_obj_remove_style_all(zoom);
    lv_obj_set_size(zoom, LV_PCT(100), 52);
    lv_obj_set_flex_flow(zoom, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(zoom, 12, LV_PART_MAIN);
    lv_obj_clear_flag(zoom, LV_OBJ_FLAG_SCROLLABLE);
    for (size_t i = 0; i < sizeof(kZoomTags) / sizeof(kZoomTags[0]); i++) {
        char text[16];
        format_duration(text, sizeof(text), HISTORY_WINDOW_PRESETS[i]);
        state.zoom_buttons[i] = create_button(zoom, text, kZoomTags[i], zoom_cb);
        lv_obj_set_height(state.zoom_buttons[i], 52);
    }

    // Row 1: plotted lines. Row 2: mode bands.
    auto make_legend_row = [](lv_obj_t* parent) {
        lv_obj_t* row = lv_obj_create(parent);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 28, LV_PART_MAIN);
        lv_obj_set_style_pad_row(row, 4, LV_PART_MAIN);
        return row;
    };
    lv_obj_t* legend_lines = make_legend_row(state.overlay);
    state.legend_lines = legend_lines;
    add_legend_item(legend_lines, COLOR_INLET, i18n_get(STR_HISTORY_INLET), false);
    add_legend_item(legend_lines, COLOR_OUTLET, i18n_get(STR_HISTORY_OUTLET), false);
    add_legend_item(legend_lines, COLOR_SETPOINT,
                    i18n_get(STR_HISTORY_SETPOINT), false);
    state.legend_power = make_legend_row(state.overlay);
    state.power_label = add_legend_item(state.legend_power, COLOR_POWER,
                                        i18n_get(STR_HISTORY_VIEW_POWER), false);
    lv_obj_set_user_data(state.power_label, (void*)"history_power_total");
    lv_obj_t* legend_modes = make_legend_row(state.overlay);
    lv_obj_set_style_margin_top(legend_modes, -8, LV_PART_MAIN);
    add_legend_item(legend_modes, COLOR_HEATING,
                    i18n_get(STR_HISTORY_LEGEND_HEATING), true);
    add_legend_item(legend_modes, COLOR_COOLING,
                    i18n_get(STR_HISTORY_LEGEND_COOLING), true);
    add_legend_item(legend_modes, COLOR_HOT_WATER,
                    i18n_get(STR_HISTORY_LEGEND_HOT_WATER), true);
    add_legend_item(legend_modes, COLOR_DEFROST,
                    i18n_get(STR_HISTORY_LEGEND_DEFROST), true);
    add_legend_item(legend_modes, COLOR_FAULT,
                    i18n_get(STR_HISTORY_LEGEND_FAULT), false);

    state.chart = lv_obj_create(state.overlay);
    lv_obj_set_width(state.chart, LV_PCT(100));
    lv_obj_set_flex_grow(state.chart, 1);
    lv_obj_set_style_bg_color(state.chart, COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_border_color(state.chart, COLOR_GRID, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.chart, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(state.chart, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.chart, 0, LV_PART_MAIN);
    lv_obj_clear_flag(state.chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(state.chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(state.chart, (void*)"temperature_history_chart");
    lv_obj_add_event_cb(state.chart, chart_press_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(state.chart, chart_press_cb, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(state.chart, chart_press_cb, LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_add_event_cb(state.chart, chart_press_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(state.chart, chart_press_cb, LV_EVENT_PRESS_LOST, nullptr);
    lv_obj_add_event_cb(state.chart, chart_double_click_cb,
                        LV_EVENT_DOUBLE_CLICKED, nullptr);
    lv_obj_add_event_cb(state.chart, chart_draw_cb, LV_EVENT_DRAW_MAIN_END,
                        nullptr);

    state.status_label = lv_label_create(state.chart);
    lv_obj_set_width(state.status_label, LV_PCT(80));
    lv_label_set_long_mode(state.status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(state.status_label, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.status_label, UI_COLOR_TEXT_DIM,
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(state.status_label, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_center(state.status_label);

    lv_obj_t* actions = lv_obj_create(state.overlay);
    lv_obj_remove_style_all(actions);
    lv_obj_set_size(actions, LV_PCT(100), 68);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(actions, 12, LV_PART_MAIN);
    state.previous_button = create_button(
        actions, LV_SYMBOL_LEFT " 8 h",
        "temperature_history_previous", previous_cb);
    state.back_hour_button = create_button(
        actions, LV_SYMBOL_LEFT " 1 h", "temperature_history_back_hour",
        back_hour_cb);
    state.forward_hour_button = create_button(
        actions, "1 h " LV_SYMBOL_RIGHT, "temperature_history_forward_hour",
        forward_hour_cb);
    state.next_button = create_button(
        actions, "8 h " LV_SYMBOL_RIGHT,
        "temperature_history_next", next_cb);

    time_t now;
    time(&now);
    uint32_t latest = 0;
    if (time_mgr_is_synced() && now > 0) {
        latest = ((uint32_t)now +
                  HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC - 1) /
                 HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC *
                 HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC;
    } else {
        history_storage_latest_telemetry_timestamp(&latest);
        if (latest > 0) latest += HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC;
    }
    if (latest == 0) {
        latest = state.window_seconds;
    }
    state.latest_end = latest;
    apply_view();
    apply_zoom();
    if (state.pending_center != 0) {
        const uint32_t t = state.pending_center;
        state.pending_center = 0;
        request_window(history_window_center(
            t, state.window_seconds, history_window_snap(state.window_seconds),
            latest));
    } else {
        request_window(latest);
    }
    return state.overlay;
}

void heatpump_history_hide(void) {
    if (!state.shown) return;
    state.shown = false;
    state.generation++;
    state.pending_query = false;
    if (state.overlay) lv_obj_delete(state.overlay);
    state.overlay = nullptr;
    static_cache_free();
    s_cursor_area_valid = false;
    state.chart = nullptr;
    state.range_label = nullptr;
    state.status_label = nullptr;
    state.previous_button = nullptr;
    state.back_hour_button = nullptr;
    state.forward_hour_button = nullptr;
    state.next_button = nullptr;
    state.latest_button = nullptr;
    state.view_temps_button = nullptr;
    state.view_power_button = nullptr;
    for (auto& button : state.zoom_buttons) button = nullptr;
    state.legend_lines = nullptr;
    state.legend_power = nullptr;
    state.power_label = nullptr;
    state.fault_count = 0;
    state.defrost_count = 0;
    heap_caps_free(state.defrosts);
    state.defrosts = nullptr;
    heap_caps_free(state.samples);
    state.samples = nullptr;
    heap_caps_free(state.interp_inlet);
    heap_caps_free(state.interp_outlet);
    state.interp_inlet = nullptr;
    state.interp_outlet = nullptr;
    state.cursor = -1;
    state.sample_count = 0;
}

bool heatpump_history_is_shown(void) {
    return state.shown;
}

void heatpump_history_open_centered(uint32_t timestamp) {
    if (state.shown) {
        request_window(history_window_center(
            timestamp, state.window_seconds,
            history_window_snap(state.window_seconds), state.latest_end));
    } else {
        state.pending_center = timestamp;
    }
}
