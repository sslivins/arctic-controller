#include "heatpump_history_screen.h"

#include "app_preferences.h"
#include "fonts/fonts.h"
#include "history_storage.h"
#include "i18n/i18n.h"
#include "nav_bar.h"
#include "time_manager.h"
#include "ui_common.h"
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static constexpr uint32_t WINDOW_SECONDS = 8 * 60 * 60;
static constexpr uint32_t RETENTION_SECONDS =
    HISTORY_TELEMETRY_RETENTION_DAYS * 24 * 60 * 60;
static constexpr uint32_t CONTIGUOUS_SECONDS =
    HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC + 15;

#define COLOR_BG        lv_color_hex(0x1a1a2e)
#define COLOR_PANEL     lv_color_hex(0x16213e)
#define COLOR_GRID      lv_color_hex(0x3d4f6f)
#define COLOR_INLET     lv_color_hex(0x38bdf8)
#define COLOR_OUTLET    lv_color_hex(0xfb7185)
#define COLOR_SETPOINT  lv_color_hex(0x4ade80)
#define COLOR_HEATING   lv_color_hex(0xef4444)
#define COLOR_COOLING   lv_color_hex(0x3b82f6)
#define COLOR_HOT_WATER lv_color_hex(0xfbbf24)

struct QueryResult {
    uint32_t generation;
    uint32_t start;
    uint32_t end;
    esp_err_t error;
    size_t count;
    history_telemetry_sample_t* samples;
};

static struct {
    bool shown = false;
    lv_obj_t* overlay = nullptr;
    lv_obj_t* chart = nullptr;
    lv_obj_t* range_label = nullptr;
    lv_obj_t* status_label = nullptr;
    lv_obj_t* previous_button = nullptr;
    lv_obj_t* next_button = nullptr;
    lv_obj_t* latest_button = nullptr;
    heatpump_history_close_cb_t on_close = nullptr;
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
} state;

static void request_window(uint32_t end);

static int32_t display_deci_c(int32_t deci_c) {
    if (app_prefs_get_temp_unit() == TEMP_UNIT_FAHRENHEIT) {
        return (deci_c * 9) / 5 + 320;
    }
    return deci_c;
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
                               WINDOW_SECONDS);
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
                       lv_text_align_t align) {
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.color = color;
    dsc.font = &montserrat_16_latin;
    dsc.text = text;
    dsc.text_local = true;
    dsc.align = align;
    lv_draw_label(layer, &dsc, &area);
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
    return {coords.x1 + 72, coords.y1 + 20, coords.x2 - 18, coords.y2 - 48};
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

static void chart_press_cb(lv_event_t* event) {
    if (state.sample_count == 0) return;
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    const lv_area_t plot = plot_area(state.chart);
    int32_t best = -1;
    int32_t best_dx = INT32_MAX;
    if (point.x >= plot.x1 && point.x <= plot.x2) {
        for (size_t i = 0; i < state.sample_count; i++) {
            const auto& s = state.samples[i];
            if (!(s.flags & (HISTORY_TELEMETRY_INLET_VALID |
                             HISTORY_TELEMETRY_OUTLET_VALID))) {
                continue;
            }
            const int32_t dx = abs(map_x(s.timestamp, plot) - point.x);
            if (dx < best_dx) { best_dx = dx; best = (int32_t)i; }
        }
    }
    // Only snap to a sample near the finger, so tapping an empty stretch of
    // the chart clears the crosshair.
    if (best_dx > 30) best = -1;
    if (best != state.cursor) {
        state.cursor = best;
        lv_obj_invalidate(state.chart);
    }
    (void)event;
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

static void draw_cursor(lv_layer_t* layer, const lv_area_t& plot,
                        int32_t min_value, int32_t max_value) {
    if (state.cursor < 0 || (size_t)state.cursor >= state.sample_count) return;
    const size_t i = (size_t)state.cursor;
    const auto& s = state.samples[i];
    const int32_t x = map_x(s.timestamp, plot);

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

    char when[12];
    time_t t = s.timestamp;
    struct tm local = {};
    localtime_r(&t, &local);
    if (time_mgr_get_24h_format()) {
        strftime(when, sizeof(when), "%H:%M", &local);
    } else {
        int hour12 = local.tm_hour % 12;
        if (hour12 == 0) hour12 = 12;
        snprintf(when, sizeof(when), "%d:%02d %s", hour12, local.tm_min,
                 local.tm_hour < 12 ? "AM" : "PM");
    }
    char outlet[12], inlet[12], setpoint[12];
    format_reading(outlet, sizeof(outlet), s, HISTORY_TELEMETRY_OUTLET_VALID,
                   s.outlet_deci_c);
    format_reading(inlet, sizeof(inlet), s, HISTORY_TELEMETRY_INLET_VALID,
                   s.inlet_deci_c);
    format_reading(setpoint, sizeof(setpoint), s,
                   HISTORY_TELEMETRY_SETPOINT_VALID, s.setpoint_deci_c);

    static constexpr int32_t BOX_W = 210, BOX_H = 112, ROW_H = 22, PAD = 10;
    int32_t bx = x + 16;
    if (bx + BOX_W > plot.x2) bx = x - 16 - BOX_W;
    if (bx < plot.x1) bx = plot.x1;
    const lv_area_t box = {bx, plot.y1 + 4, bx + BOX_W, plot.y1 + 4 + BOX_H};
    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = COLOR_BG;
    bg.bg_opa = LV_OPA_90;
    bg.radius = 10;
    bg.border_color = COLOR_GRID;
    bg.border_width = 1;
    lv_draw_rect(layer, &bg, &box);

    lv_area_t row = {box.x1 + PAD, box.y1 + 6, box.x2 - PAD, box.y1 + 6 + ROW_H};
    draw_label(layer, row, when, UI_COLOR_TEXT_DIM, LV_TEXT_ALIGN_LEFT);
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
        const lv_area_t swatch = {row.x1, cy - 2, row.x1 + 14, cy + 2};
        lv_draw_rect(layer, &sw, &swatch);
        lv_area_t name_area = {row.x1 + 22, row.y1, row.x2, row.y2};
        draw_label(layer, name_area, r.name, UI_COLOR_TEXT, LV_TEXT_ALIGN_LEFT);
        draw_label(layer, row, r.value, UI_COLOR_TEXT, LV_TEXT_ALIGN_RIGHT);
    }
}

static void chart_draw_cb(lv_event_t* event) {
    lv_obj_t* chart = (lv_obj_t*)lv_event_get_target(event);
    lv_layer_t* layer = lv_event_get_layer(event);
    lv_area_t coords;
    lv_obj_get_coords(chart, &coords);
    const lv_area_t plot = plot_area(chart);

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
        min_value = ((min_value - padding) / 50) * 50;
        if (min_value > 0 && min_value % 50) min_value -= 50;
        max_value = ((max_value + padding + 49) / 50) * 50;
    }

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

    for (int grid = 0; grid <= 5; grid++) {
        int32_t y = plot.y2 -
            (int32_t)((int64_t)grid * lv_area_get_height(&plot) / 5);
        draw_line(layer, {plot.x1, y}, {plot.x2, y}, COLOR_GRID, 1);
        int32_t value = min_value +
            (int32_t)((int64_t)grid * (max_value - min_value) / 5);
        value = display_deci_c(value);
        char label[16];
        snprintf(label, sizeof(label), "%ld.%ld°",
                 (long)(value / 10), (long)labs(value % 10));
        lv_area_t label_area = {coords.x1, y - 12, plot.x1 - 8, y + 12};
        draw_label(layer, label_area, label, UI_COLOR_TEXT_DIM,
                   LV_TEXT_ALIGN_RIGHT);
    }
    // X-axis gridlines/labels are anchored to whole-hour boundaries in local
    // time (rounded up to the next even-hour multiple) rather than to the
    // ragged window edges, so the axis always reads in clean hours
    // (e.g. 08:00, 10:00, 12:00) regardless of the current minute. The data
    // window itself still ends at "now" so the last 8 hours stay visible.
    static constexpr int TICK_STEP_HOURS = 2;
    time_t tick_time;
    {
        time_t start = state.window_start;
        struct tm t0 = {};
        localtime_r(&start, &t0);
        if (t0.tm_min != 0 || t0.tm_sec != 0) {
            t0.tm_hour += 1;
        }
        t0.tm_min = 0;
        t0.tm_sec = 0;
        // Snap up to the next multiple of TICK_STEP_HOURS for even spacing.
        t0.tm_hour +=
            (TICK_STEP_HOURS - (t0.tm_hour % TICK_STEP_HOURS)) % TICK_STEP_HOURS;
        t0.tm_isdst = -1;
        tick_time = mktime(&t0);
    }
    for (; tick_time > 0 && (uint32_t)tick_time <= state.window_end;
         tick_time += TICK_STEP_HOURS * 3600) {
        if ((uint32_t)tick_time < state.window_start) continue;
        int32_t x = map_x((uint32_t)tick_time, plot);
        draw_line(layer, {x, plot.y1}, {x, plot.y2}, COLOR_GRID, 1);
        struct tm local = {};
        localtime_r(&tick_time, &local);
        char label[12];
        if (time_mgr_get_24h_format()) {
            strftime(label, sizeof(label), "%H:%M", &local);
        } else {
            // Ticks are snapped to whole hours above, so the minutes carry no
            // information in 12-hour form -- "8 AM" rather than "08:00 AM".
            // That also keeps the label narrower than the 24-hour "08:00", so
            // adding the suffix cannot make axis labels collide.
            int hour12 = local.tm_hour % 12;
            if (hour12 == 0) hour12 = 12;
            snprintf(label, sizeof(label), "%d %s", hour12,
                     local.tm_hour < 12 ? "AM" : "PM");
        }
        lv_area_t label_area = {x - 40, plot.y2 + 10, x + 40, coords.y2};
        draw_label(layer, label_area, label, UI_COLOR_TEXT_DIM,
                   LV_TEXT_ALIGN_CENTER);
    }

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

    draw_cursor(layer, plot, min_value, max_value);
}

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
    const char* fmt =
        time_mgr_get_24h_format() ? "%b %d, %H:%M" : "%b %d, %I:%M %p";
    strftime(start_text, sizeof(start_text), fmt, &start_tm);
    strftime(end_text, sizeof(end_text), fmt, &end_tm);
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
    } else {
        lv_obj_remove_state(state.previous_button, LV_STATE_DISABLED);
    }
    if (state.window_end >= state.latest_end) {
        lv_obj_add_state(state.next_button, LV_STATE_DISABLED);
        lv_obj_add_state(state.latest_button, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(state.next_button, LV_STATE_DISABLED);
        lv_obj_remove_state(state.latest_button, LV_STATE_DISABLED);
    }
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
        delete result;
        start_pending_query_if_needed();
        return;
    }

    heap_caps_free(state.samples);
    state.samples = result->samples;
    state.sample_count = result->count;
    state.cursor = -1;
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
    lv_async_call(query_complete, result);
    vTaskDelete(nullptr);
}

static void request_window(uint32_t end) {
    state.window_end = end;
    state.window_start = end - WINDOW_SECONDS;
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

static void close_cb(lv_event_t*) {
    heatpump_history_close_cb_t callback = state.on_close;
    heatpump_history_hide();
    if (callback) callback();
}

static void previous_cb(lv_event_t*) {
    request_window(state.window_end - WINDOW_SECONDS);
}

static void next_cb(lv_event_t*) {
    uint32_t end = state.window_end + WINDOW_SECONDS;
    if (end > state.latest_end) end = state.latest_end;
    request_window(end);
}

static void latest_cb(lv_event_t*) {
    request_window(state.latest_end);
}

static void add_legend_item(lv_obj_t* parent, lv_color_t color,
                            const char* text) {
    lv_obj_t* item = lv_obj_create(parent);
    lv_obj_remove_style_all(item);
    lv_obj_set_size(item, LV_SIZE_CONTENT, 32);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(item, 6, LV_PART_MAIN);

    lv_obj_t* swatch = lv_obj_create(item);
    lv_obj_set_size(swatch, 22, 8);
    lv_obj_set_style_bg_color(swatch, color, LV_PART_MAIN);
    lv_obj_set_style_border_width(swatch, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(swatch, 4, LV_PART_MAIN);

    lv_obj_t* label = lv_label_create(item);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &montserrat_16_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT_DIM, LV_PART_MAIN);
}

void heatpump_history_show(lv_obj_t* parent,
                           heatpump_history_close_cb_t on_close) {
    if (state.shown || parent == nullptr) return;
    state.shown = true;
    state.on_close = on_close;
    state.generation++;

    state.overlay = lv_obj_create(parent);
    lv_obj_set_size(state.overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_align(state.overlay, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(state.overlay, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state.overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(state.overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.overlay, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(state.overlay, NAV_BAR_H + 18, LV_PART_MAIN);
    lv_obj_set_flex_flow(state.overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(state.overlay, 12, LV_PART_MAIN);
    lv_obj_clear_flag(state.overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(state.overlay, (void*)"temperature_history_screen");

    lv_obj_t* header = lv_obj_create(state.overlay);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), 68);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, i18n_get(STR_HISTORY_TITLE));
    lv_obj_set_style_text_font(title, UI_FONT_HEADER, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, LV_PART_MAIN);

    lv_obj_t* back = lv_button_create(header);
    lv_obj_set_size(back, 130, 60);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x3d4f6f), LV_PART_MAIN);
    lv_obj_set_style_radius(back, 12, LV_PART_MAIN);
    lv_obj_set_user_data(back, (void*)"temperature_history_back");
    lv_obj_add_event_cb(back, close_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* back_label = lv_label_create(back);
    lv_label_set_text(back_label, i18n_get(STR_HISTORY_BACK));
    lv_obj_set_style_text_font(back_label, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(back_label, UI_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_center(back_label);

    state.range_label = lv_label_create(state.overlay);
    lv_obj_set_width(state.range_label, LV_PCT(100));
    lv_obj_set_style_text_font(state.range_label, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.range_label, UI_COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_align(state.range_label, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_set_user_data(state.range_label,
                         (void*)"temperature_history_range");

    lv_obj_t* legend = lv_obj_create(state.overlay);
    lv_obj_remove_style_all(legend);
    lv_obj_set_size(legend, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(legend, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(legend, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_row(legend, 4, LV_PART_MAIN);
    add_legend_item(legend, COLOR_INLET, i18n_get(STR_HISTORY_INLET));
    add_legend_item(legend, COLOR_OUTLET, i18n_get(STR_HISTORY_OUTLET));
    add_legend_item(legend, COLOR_SETPOINT, i18n_get(STR_HISTORY_SETPOINT));
    add_legend_item(legend, COLOR_HEATING, i18n_get(STR_HP_MODE_HEATING));
    add_legend_item(legend, COLOR_COOLING, i18n_get(STR_HP_MODE_COOLING));
    add_legend_item(legend, COLOR_HOT_WATER, i18n_get(STR_HP_MODE_HOT_WATER));

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
        actions, i18n_get(STR_HISTORY_PREVIOUS),
        "temperature_history_previous", previous_cb);
    state.latest_button = create_button(
        actions, i18n_get(STR_HISTORY_LATEST),
        "temperature_history_latest", latest_cb);
    state.next_button = create_button(
        actions, i18n_get(STR_HISTORY_NEXT),
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
        latest = WINDOW_SECONDS;
    }
    state.latest_end = latest;
    request_window(latest);
}

void heatpump_history_hide(void) {
    if (!state.shown) return;
    state.shown = false;
    state.generation++;
    state.on_close = nullptr;
    state.pending_query = false;
    if (state.overlay) lv_obj_delete(state.overlay);
    state.overlay = nullptr;
    state.chart = nullptr;
    state.range_label = nullptr;
    state.status_label = nullptr;
    state.previous_button = nullptr;
    state.next_button = nullptr;
    state.latest_button = nullptr;
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
