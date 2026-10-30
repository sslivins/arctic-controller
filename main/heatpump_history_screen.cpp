#include "heatpump_history_screen.h"

#include "app_preferences.h"
#include "fault_history.h"
#include "fonts/fonts.h"
#include "history_storage.h"
#include "history_window.h"
#include "i18n/i18n.h"
#include "macon_faults.h"
#include "nav_bar.h"
#include "time_manager.h"
#include "tschart.h"
#include "ui_common.h"
#include <bsp/m5stack_tab5.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#include <new>
#include <stddef.h>
#include <stdio.h>
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
#define COLOR_DEFROST   lv_color_hex(0x4fb3c9)
#define COLOR_POWER     lv_color_hex(0x60a5fa)

static constexpr size_t MAX_FAULTS = 16;
static constexpr size_t MAX_DEFROSTS = 24;
static constexpr size_t ZOOM_COUNT =
    sizeof(HISTORY_WINDOW_PRESETS) / sizeof(HISTORY_WINDOW_PRESETS[0]);

// Span sets, drawn in this order.
enum : uint8_t { SPANS_RUNS = 0, SPANS_DEFROST = 1, SPANS_FAULTS = 2 };

// One loaded window. The chart borrows `samples` until it releases the
// dataset (window replaced or chart deleted).
struct WindowData {
    uint32_t session;
    uint32_t request_id;
    uint32_t start;
    uint32_t end;
    esp_err_t error;
    size_t count;
    history_telemetry_sample_t* samples;  // PSRAM
    size_t fault_count;
    fault_interval_t faults[MAX_FAULTS];
    size_t defrost_count;
    fault_interval_t* defrosts;  // PSRAM, MAX_DEFROSTS
    uint32_t wh;
};

static struct {
    bool shown = false;
    // Bumped on every open and close, so a query finishing after its chart
    // is gone (or replaced) is dropped.
    uint32_t session = 0;
    lv_obj_t* overlay = nullptr;
    lv_obj_t* chart = nullptr;
    lv_obj_t* range_label = nullptr;
    lv_obj_t* power_label = nullptr;
    lv_obj_t* previous_button = nullptr;
    lv_obj_t* back_hour_button = nullptr;
    lv_obj_t* forward_hour_button = nullptr;
    lv_obj_t* next_button = nullptr;
    lv_obj_t* latest_button = nullptr;
    lv_obj_t* zoom_buttons[ZOOM_COUNT] = {};
    tschart_source_t* source = nullptr;
    tschart_group_t* temps_group = nullptr;
    tschart_group_t* power_group = nullptr;
    // The dataset the chart is showing.
    WindowData* cur = nullptr;
    bool query_running = false;
    // A window asked for while a query ran; started when it completes.
    bool has_queued = false;
    uint32_t queued_start = 0, queued_end = 0, queued_id = 0;
    uint32_t latest_end = 0;
    // Moment to centre the next opened window on (0 = latest).
    uint32_t pending_center = 0;
    // Kept across hide/show so the chart reopens the way the user left it.
    uint32_t window_seconds = HISTORY_WINDOW_DEFAULT_SECONDS;
    bool temps_on = true;
    bool power_on = true;
    bool view_saved = false;  // axis/tags below hold the user's choice
    bool axis_open = false;
    bool tags_on = true;
} state;

static void request_window(uint32_t end);

static bool fahrenheit() {
    return app_prefs_get_temp_unit() == TEMP_UNIT_FAHRENHEIT;
}

static int32_t display_deci_c(int32_t deci_c, void*) {
    return fahrenheit() ? (deci_c * 9) / 5 + 320 : deci_c;
}

static size_t clamp_len(int n, size_t cap) {
    if (n < 0) return 0;
    return (size_t)n < cap ? (size_t)n : cap - 1;
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

static bool compressor_on(const history_telemetry_sample_t& s) {
    return (s.flags & HISTORY_TELEMETRY_COMPRESSOR_VALID) &&
           (s.flags & HISTORY_TELEMETRY_COMPRESSOR_RUNNING);
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

// Tenths of a unit as "12.5" (or "12" when whole).
static void format_tenths(char* buf, size_t len, uint32_t tenths, bool keep_decimal) {
    if (!keep_decimal && tenths % 10 == 0) {
        snprintf(buf, len, "%lu", (unsigned long)(tenths / 10));
    } else {
        snprintf(buf, len, "%lu.%lu", (unsigned long)(tenths / 10),
                 (unsigned long)(tenths % 10));
    }
}

static const char* fault_code(const fault_interval_t& f) {
    const arctic::MaconFaultBit* fb = arctic::macon_fault_bit_for_site(
        static_cast<arctic::MaconFaultSiteId>(f.site));
    return fb != nullptr ? fb->code : "?";
}

// A still-active fault runs to "now", which is the window's end at most.
static uint32_t fault_end(const fault_interval_t& f, const WindowData& w) {
    if (f.end != 0) return f.end;
    time_t now;
    time(&now);
    const uint32_t end = (uint32_t)now;
    return end > w.end || end < f.start ? w.end : end;
}

// ---- Formatters ---------------------------------------------------------------

// Whole degrees: "37°" on the axis and tags, "37°C" in the card.
static size_t format_temp(int32_t v, tschart_fmt_ctx_t ctx, char* buf, size_t cap,
                          void*) {
    if (v == TSCHART_NONE) return clamp_len(snprintf(buf, cap, "--"), cap);
    const long whole = v >= 0 ? (v + 5) / 10 : -((-v + 5) / 10);
    if (ctx == TSCHART_FMT_CURSOR) {
        return clamp_len(snprintf(buf, cap, "%ld°%s", whole, fahrenheit() ? "F" : "C"),
                         cap);
    }
    return clamp_len(snprintf(buf, cap, "%ld°", whole), cap);
}

// Watts in, kW out: "1.5" on the axis, "1.4 kW" on tags and in the card.
static size_t format_power(int32_t v, tschart_fmt_ctx_t ctx, char* buf, size_t cap,
                           void*) {
    if (v == TSCHART_NONE) return clamp_len(snprintf(buf, cap, "--"), cap);
    const uint32_t tenths = (uint32_t)(v < 0 ? 0 : v + 50) / 100;
    char kw[16];
    format_tenths(kw, sizeof(kw), tenths, ctx != TSCHART_FMT_AXIS);
    if (ctx == TSCHART_FMT_AXIS) return clamp_len(snprintf(buf, cap, "%s", kw), cap);
    return clamp_len(snprintf(buf, cap, "%s kW", kw), cap);
}

// Gridline labels sit on round local times, so the minutes only matter off
// the hour: "8 AM" rather than "8:00 AM".
static size_t format_tick(tschart_time_t t, uint32_t, char* buf, size_t cap, void*) {
    time_t tt = t;
    struct tm local = {};
    localtime_r(&tt, &local);
    if (time_mgr_get_24h_format()) return strftime(buf, cap, "%H:%M", &local);
    int hour12 = local.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    const char* suffix = local.tm_hour < 12 ? "AM" : "PM";
    if (local.tm_min == 0) {
        return clamp_len(snprintf(buf, cap, "%d %s", hour12, suffix), cap);
    }
    return clamp_len(snprintf(buf, cap, "%d:%02d %s", hour12, local.tm_min, suffix), cap);
}

// ---- Cursor card ----------------------------------------------------------------

static const fault_interval_t* defrost_at(const WindowData& w, uint32_t t) {
    for (size_t d = 0; d < w.defrost_count; d++) {
        const auto& df = w.defrosts[d];
        if (df.start <= t && t <= (df.end != 0 ? df.end : w.end)) return &df;
    }
    return nullptr;
}

// What the unit was doing at a sample, as the home screen names it. Defrost
// is checked first: it pauses the run, so its samples aren't "running".
static const char* sample_state_name(const WindowData& w,
                                     const history_telemetry_sample_t& s,
                                     lv_color_t* color) {
    if (defrost_at(w, s.timestamp) != nullptr) {
        *color = COLOR_DEFROST;
        return i18n_get(STR_HOME_DEFROSTING);
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

// Start/end of the compressor run holding sample `i`, as the web dashboard
// groups them (a defrost's mode-less samples included). In the compressor
// pause inside a defrost, the defrost itself. A run beginning at the window's
// left edge may have started earlier (clip_start); `open` means it is still
// going at the window's last sample.
struct RunSpan { uint32_t start, end; bool clip_start, open; };
static bool cursor_run(const WindowData& w, size_t i, RunSpan* run) {
    static constexpr uint32_t GAP = HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC * 5 / 2;
    const history_telemetry_sample_t* s = w.samples;
    const fault_interval_t* defrost = defrost_at(w, s[i].timestamp);
    // Matches the card's state row, which reads "Idle" otherwise.
    if (compressor_on(s[i]) && (running_sample(s[i]) || defrost != nullptr)) {
        size_t lo = i, hi = i;
        while (lo > 0 && compressor_on(s[lo - 1]) &&
               s[lo].timestamp - s[lo - 1].timestamp <= GAP) {
            lo--;
        }
        while (hi + 1 < w.count && compressor_on(s[hi + 1]) &&
               s[hi + 1].timestamp - s[hi].timestamp <= GAP) {
            hi++;
        }
        run->start = s[lo].timestamp;
        run->end = s[hi].timestamp + HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC;
        run->clip_start = run->start < w.start + GAP;
        run->open = hi + 1 == w.count;
        return true;
    }
    if (defrost == nullptr) return false;
    run->start = defrost->start;
    run->end = defrost->end != 0 ? defrost->end : w.end;
    run->clip_start = defrost->start < w.start;
    run->open = defrost->end == 0;
    return true;
}

// Card text for the run under sample `i`: "…2:00 AM – 2:25 AM" (… where the
// window cuts it off, "now" while it runs in the live window) and "25 min".
static bool format_cursor_run(const WindowData& w, size_t i, char* span,
                              size_t span_len, char* length, size_t length_len) {
    RunSpan run;
    if (!cursor_run(w, i, &run)) return false;
    const bool live = w.end >= state.latest_end;
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

static void card_cb(lv_obj_t*, const tschart_cursor_info_t* info, tschart_card_t* card,
                    void*) {
    const WindowData* w = state.cur;
    if (w == nullptr || info->index >= w->count) {
        char when[12];
        format_clock(when, sizeof(when), info->time);
        tschart_card_add(card, TSCHART_ROW_TITLE, when, nullptr, UI_COLOR_TEXT_DIM);
        return;
    }
    const auto& s = w->samples[info->index];
    char when[12];
    format_clock(when, sizeof(when), s.timestamp);
    tschart_card_add(card, TSCHART_ROW_TITLE, when, nullptr, UI_COLOR_TEXT_DIM);

    char span[48] = "", length[16] = "";
    const bool has_run =
        format_cursor_run(*w, info->index, span, sizeof(span), length, sizeof(length));
    lv_color_t color;
    const char* name = sample_state_name(*w, s, &color);
    tschart_card_add(card, TSCHART_ROW_BLOCK, name, length, color);
    if (has_run) tschart_card_add(card, TSCHART_ROW_TEXT, span, nullptr, UI_COLOR_TEXT_DIM);
    tschart_card_add_layers(card);

    // Faults active at the touched moment, by code (the notes name them).
    int rows = 0;
    for (size_t f = 0; f < w->fault_count && rows < 2; f++) {
        if (w->faults[f].start <= s.timestamp && s.timestamp <= fault_end(w->faults[f], *w)) {
            char text[24];
            snprintf(text, sizeof(text), LV_SYMBOL_WARNING " %s", fault_code(w->faults[f]));
            tschart_card_add(card, TSCHART_ROW_WARNING, text, nullptr, COLOR_FAULT);
            rows++;
        }
    }
}

// ---- Header, navigation ---------------------------------------------------------

static void update_range_label() {
    if (state.range_label == nullptr || state.chart == nullptr) return;
    tschart_time_t ws, we;
    tschart_get_target_window(state.chart, &ws, &we);
    time_t start = ws;
    time_t end = we;
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

static void set_enabled(lv_obj_t* button, bool on) {
    if (on) {
        lv_obj_remove_state(button, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(button, LV_STATE_DISABLED);
    }
}

static void update_navigation() {
    if (state.chart == nullptr) return;
    tschart_time_t ws, we;
    tschart_get_target_window(state.chart, &ws, &we);
    const uint32_t earliest =
        state.latest_end > RETENTION_SECONDS ? state.latest_end - RETENTION_SECONDS : 0;
    set_enabled(state.previous_button, ws > earliest);
    set_enabled(state.back_hour_button, ws > earliest);
    const bool live = we >= state.latest_end;
    set_enabled(state.next_button, !live);
    set_enabled(state.forward_hour_button, !live);
    if (live) {
        lv_obj_add_flag(state.latest_button, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(state.latest_button, LV_OBJ_FLAG_HIDDEN);
    }
}

// "Power · 12.3 kWh" for the shown window, while the power pane is on.
static void update_power_label() {
    if (state.power_label == nullptr) return;
    if (!state.power_on) {
        lv_obj_add_flag(state.power_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(state.power_label, LV_OBJ_FLAG_HIDDEN);
    char kwh[16];
    format_tenths(kwh, sizeof(kwh), ((state.cur ? state.cur->wh : 0) + 50) / 100, true);
    char amount[48];
    snprintf(amount, sizeof(amount), i18n_get(STR_HISTORY_POWER_WINDOW_FMT), kwh);
    char text[80];
    snprintf(text, sizeof(text), "%s \xc2\xb7 %s", i18n_get(STR_HISTORY_VIEW_POWER),
             amount);
    lv_label_set_text(state.power_label, text);
}

// ---- Data -----------------------------------------------------------------------

static void free_window(WindowData* w) {
    if (w == nullptr) return;
    heap_caps_free(w->samples);
    heap_caps_free(w->defrosts);
    delete w;
}

static void release_cb(void* ud) {
    auto* w = static_cast<WindowData*>(ud);
    if (state.cur == w) state.cur = nullptr;
    free_window(w);
}

// Mode bands, defrosts and faults for the window just shown.
static void set_overlays(lv_obj_t* c, const WindowData& w) {
    size_t nb = 0;
    auto* bands = w.count ? static_cast<tschart_span_t*>(heap_caps_malloc(
                                sizeof(tschart_span_t) * w.count,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))
                          : nullptr;
    if (bands != nullptr) {
        for (size_t i = 0; i < w.count; i++) {
            const auto& first = w.samples[i];
            if (!running_sample(first)) continue;
            uint32_t band_end = first.timestamp + HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC;
            size_t j = i + 1;
            while (j < w.count) {
                const auto& next = w.samples[j];
                if (!running_sample(next) || next.mode != first.mode ||
                    next.timestamp - w.samples[j - 1].timestamp > CONTIGUOUS_SECONDS) {
                    break;
                }
                band_end = next.timestamp + HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC;
                j++;
            }
            bands[nb++] = {first.timestamp, band_end, mode_color(first.mode), LV_OPA_20,
                           TSCHART_SPAN_FILL, 0};
            i = j - 1;
        }
    }
    tschart_set_spans(c, SPANS_RUNS, bands, nb);
    heap_caps_free(bands);

    // Defrost: an icy block over the run it interrupts, at least 3 px wide so a
    // short one still shows at wide zoom.
    tschart_span_t defrosts[MAX_DEFROSTS];
    for (size_t i = 0; i < w.defrost_count; i++) {
        defrosts[i] = {w.defrosts[i].start, w.defrosts[i].end, COLOR_DEFROST, LV_OPA_40,
                       TSCHART_SPAN_FILL, 3};
    }
    tschart_set_spans(c, SPANS_DEFROST, defrosts, w.defrost_count);

    tschart_span_t faults[MAX_FAULTS];
    tschart_note_t notes[MAX_FAULTS];
    for (size_t i = 0; i < w.fault_count; i++) {
        const auto& f = w.faults[i];
        faults[i] = {f.start, fault_end(f, w), COLOR_FAULT, LV_OPA_COVER,
                     TSCHART_SPAN_HATCH, 0};
        notes[i] = {f.start, fault_code(f), COLOR_FAULT};
    }
    tschart_set_spans(c, SPANS_FAULTS, faults, w.fault_count);
    tschart_set_notes(c, notes, w.fault_count, LV_SYMBOL_WARNING " ");
}

static void applied_cb(lv_obj_t* c, void* ud) {
    auto* w = static_cast<WindowData*>(ud);
    state.cur = w;
    set_overlays(c, *w);
    if (w->error != ESP_OK) {
        tschart_set_status(c, TSCHART_STATUS_ERROR, i18n_get(STR_HISTORY_STORAGE_ERROR));
    } else if (w->count == 0) {
        tschart_set_status(c, TSCHART_STATUS_EMPTY,
                           time_mgr_is_synced() ? i18n_get(STR_HISTORY_NO_DATA)
                                                : i18n_get(STR_HISTORY_WAITING_FOR_TIME));
    } else {
        tschart_set_status(c, TSCHART_STATUS_OK, nullptr);
    }
    update_power_label();
    update_range_label();
    update_navigation();
}

static void start_query(uint32_t start, uint32_t end, uint32_t id);

static void start_queued() {
    if (!state.shown || !state.has_queued || state.query_running) return;
    state.has_queued = false;
    start_query(state.queued_start, state.queued_end, state.queued_id);
}

static void query_complete(void* data) {
    auto* w = static_cast<WindowData*>(data);
    state.query_running = false;
    if (!state.shown || state.chart == nullptr || w->session != state.session) {
        free_window(w);
        start_queued();
        return;
    }
    uint64_t joules = 0;
    for (size_t i = 0; i < w->count; i++) {
        const auto& s = w->samples[i];
        if (s.flags & HISTORY_TELEMETRY_POWER_VALID) {
            joules += (uint64_t)s.average_power_w * HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC;
        }
    }
    w->wh = (uint32_t)((joules + 1800) / 3600);

    tschart_dataset_t ds = {};
    ds.request_id = w->request_id;
    ds.start = w->start;
    ds.end = w->end;
    ds.sources[0].src = state.source;
    ds.sources[0].data = {w->samples, w->count, sizeof(history_telemetry_sample_t),
                          offsetof(history_telemetry_sample_t, timestamp),
                          CONTIGUOUS_SECONDS};
    ds.source_count = 1;
    ds.release = release_cb;
    ds.release_ud = w;
    ds.applied = applied_cb;
    // Stale ids (a newer window was asked for) are released by the chart.
    tschart_set_dataset(state.chart, &ds);
    start_queued();
}

static void query_task(void* data) {
    auto* w = static_cast<WindowData*>(data);
    w->samples = static_cast<history_telemetry_sample_t*>(
        heap_caps_malloc(sizeof(history_telemetry_sample_t) * HISTORY_TELEMETRY_PAGE_CAPACITY,
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (w->samples == nullptr) {
        w->error = ESP_ERR_NO_MEM;
        w->count = 0;
    } else {
        w->error = history_storage_query_telemetry(w->start, w->end, w->samples,
                                                   HISTORY_TELEMETRY_PAGE_CAPACITY,
                                                   &w->count);
    }
    w->defrosts = static_cast<fault_interval_t*>(heap_caps_malloc(
        sizeof(fault_interval_t) * MAX_DEFROSTS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    w->defrost_count = 0;
    w->fault_count = chart_spans_query(w->start, w->end, w->faults, MAX_FAULTS, nullptr,
                                       w->defrosts, w->defrosts ? MAX_DEFROSTS : 0,
                                       &w->defrost_count);
    // Worker task: lv_async_call() must hold the LVGL lock.
    bsp_display_lock(0);
    lv_async_call(query_complete, w);
    bsp_display_unlock();
    vTaskDelete(nullptr);
}

static void start_query(uint32_t start, uint32_t end, uint32_t id) {
    if (state.query_running) {
        state.has_queued = true;
        state.queued_start = start;
        state.queued_end = end;
        state.queued_id = id;
        return;
    }
    auto* w = new (std::nothrow) WindowData{};
    if (w != nullptr) {
        w->session = state.session;
        w->request_id = id;
        w->start = start;
        w->end = end;
        state.query_running = true;
        if (xTaskCreate(query_task, "history_query", 4096, w, 3, nullptr) == pdPASS) return;
        state.query_running = false;
        delete w;
    }
    tschart_cancel_pending(state.chart);
    tschart_set_status(state.chart, TSCHART_STATUS_ERROR, i18n_get(STR_HISTORY_STORAGE_ERROR));
    update_range_label();
    update_navigation();
}

static void request_window(uint32_t end) {
    if (state.chart == nullptr) return;
    const uint32_t start = end - state.window_seconds;
    const uint32_t id = tschart_request_window(state.chart, start, end);
    update_range_label();
    update_navigation();
    start_query(start, end, id);
}

static uint32_t target_end() {
    tschart_time_t ws, we;
    tschart_get_target_window(state.chart, &ws, &we);
    return we;
}

// ---- Controls -------------------------------------------------------------------

static lv_obj_t* create_button(lv_obj_t* parent, const char* text,
                               const char* tag, lv_event_cb_t callback) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_height(button, 64);
    lv_obj_set_flex_grow(button, 1);
    lv_obj_set_style_bg_color(button, COLOR_GRID, LV_PART_MAIN);
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

static void step_window(int32_t delta) {
    request_window(history_window_shift(target_end(), delta, state.window_seconds,
                                        state.latest_end));
}

static void previous_cb(lv_event_t*) { step_window(-(int32_t)state.window_seconds); }

static void back_hour_cb(lv_event_t*) {
    step_window(-(int32_t)history_window_step(state.window_seconds));
}
static void forward_hour_cb(lv_event_t*) {
    step_window((int32_t)history_window_step(state.window_seconds));
}

static void next_cb(lv_event_t*) {
    uint32_t end = target_end() + state.window_seconds;
    if (end > state.latest_end) end = state.latest_end;
    request_window(end);
}

static void latest_cb(lv_event_t*) { request_window(state.latest_end); }

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
    for (size_t i = 0; i < ZOOM_COUNT; i++) {
        lv_obj_t* button = state.zoom_buttons[i];
        if (button == nullptr) continue;
        const bool on = HISTORY_WINDOW_PRESETS[i] == state.window_seconds;
        lv_obj_set_style_bg_color(button, on ? UI_COLOR_ACCENT : COLOR_GRID, LV_PART_MAIN);
        lv_obj_set_style_text_color(lv_obj_get_child(button, 0),
                                    on ? lv_color_hex(0x062033) : UI_COLOR_TEXT,
                                    LV_PART_MAIN);
    }
    char step[16], text[32];
    format_duration(step, sizeof(step), state.window_seconds);
    snprintf(text, sizeof(text), LV_SYMBOL_LEFT " %s", step);
    lv_label_set_text(lv_obj_get_child(state.previous_button, 0), text);
    snprintf(text, sizeof(text), "%s " LV_SYMBOL_RIGHT, step);
    lv_label_set_text(lv_obj_get_child(state.next_button, 0), text);
    format_duration(step, sizeof(step), history_window_step(state.window_seconds));
    snprintf(text, sizeof(text), LV_SYMBOL_LEFT " %s", step);
    lv_label_set_text(lv_obj_get_child(state.back_hour_button, 0), text);
    snprintf(text, sizeof(text), "%s " LV_SYMBOL_RIGHT, step);
    lv_label_set_text(lv_obj_get_child(state.forward_hour_button, 0), text);
    if (state.chart) tschart_set_pan_snap(state.chart, history_window_snap(state.window_seconds));
}

// Zoom keeps the window's end, so the newest data stays in view from the
// live window and an older window zooms around its right edge.
static void zoom_cb(lv_event_t* event) {
    const auto* target = static_cast<lv_obj_t*>(lv_event_get_target(event));
    uint32_t window = 0;
    for (size_t i = 0; i < ZOOM_COUNT; i++) {
        if (state.zoom_buttons[i] == target) window = HISTORY_WINDOW_PRESETS[i];
    }
    if (window == 0 || window == state.window_seconds) return;
    const uint32_t end = target_end();
    state.window_seconds = window;
    apply_zoom();
    request_window(history_window_shift(end, 0, window, state.latest_end));
}

static void chart_event_cb(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    void* param = lv_event_get_param(e);
    if (code == TSCHART_EVENT_WINDOW_REQUEST) {
        // A pan settled on a new window; the chart already shows it pending.
        const auto* p = static_cast<const tschart_window_request_t*>(param);
        update_range_label();
        update_navigation();
        start_query(p->start, p->end, p->request_id);
    } else if (code == TSCHART_EVENT_DOUBLE_TAP) {
        // Re-centre on the tapped moment, e.g. to see a run that straddled two
        // windows in one piece.
        const auto* p = static_cast<const tschart_double_tap_t*>(param);
        request_window(history_window_center(p->time, state.window_seconds,
                                             history_window_snap(state.window_seconds),
                                             state.latest_end));
    } else if (code == TSCHART_EVENT_GROUP_TOGGLED) {
        const auto* p = static_cast<const tschart_group_toggled_t*>(param);
        if (p->group == state.power_group) {
            state.power_on = p->visible;
            update_power_label();
        } else if (p->group == state.temps_group) {
            state.temps_on = p->visible;
        }
    } else if (code == TSCHART_EVENT_AXIS_TOGGLED) {
        state.axis_open = static_cast<const tschart_toggled_t*>(param)->on;
        state.view_saved = true;
    } else if (code == TSCHART_EVENT_TAGS_TOGGLED) {
        state.tags_on = static_cast<const tschart_toggled_t*>(param)->on;
        state.view_saved = true;
    }
}

// ---- Chart setup ----------------------------------------------------------------

static tschart_field_t temp_field(size_t offset, uint8_t valid) {
    tschart_field_t f = {};
    f.offset = offset;
    f.type = TSCHART_I16;
    f.flags_offset = offsetof(history_telemetry_sample_t, flags);
    f.flags_mask = valid;
    f.xform = display_deci_c;
    return f;
}

static lv_obj_t* create_chart(lv_obj_t* parent) {
    lv_obj_t* c = tschart_create(parent);
    lv_obj_set_width(c, LV_PCT(100));
    lv_obj_set_flex_grow(c, 1);
    lv_obj_set_style_bg_color(c, COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_border_color(c, COLOR_GRID, LV_PART_MAIN);
    lv_obj_set_style_border_width(c, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(c, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(c, 0, LV_PART_MAIN);
    lv_obj_set_style_text_color(c, UI_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(c, &montserrat_24_latin, LV_PART_MAIN);
    tschart_set_small_font(c, &montserrat_16_latin);
    tschart_set_time_formatter(c, format_tick, nullptr);
    tschart_set_edge_text(c, i18n_get(STR_HISTORY_START), i18n_get(STR_HISTORY_NOW));
    tschart_set_card_cb(c, card_cb, nullptr);

    state.source = tschart_add_source(c, "telemetry");

    const char* unit = fahrenheit() ? "\xc2\xb0" "F" : "\xc2\xb0" "C";
    tschart_pane_cfg_t temps = {};
    temps.id = "temps";
    temps.weight = 60;
    temps.unit_tag = unit;
    temps.color = COLOR_OUTLET;
    temps.auto_range = true;
    temps.min_span = 100;
    temps.pad_permille = 100;
    temps.min_pad = 20;
    temps.min_step = 10;
    temps.fmt = format_temp;
    tschart_pane_t* temps_pane = tschart_add_pane(c, &temps);

    tschart_pane_cfg_t power = {};
    power.id = "power";
    power.weight = 40;
    power.unit_tag = "kW";
    power.color = COLOR_POWER;
    power.auto_range = true;
    power.include_zero = true;
    power.has_max = true;
    power.max = 1000;  // at least 1 kW tall
    power.pad_permille = 50;
    power.min_step = 100;
    power.fmt = format_power;
    tschart_pane_t* power_pane = tschart_add_pane(c, &power);

    tschart_layer_cfg_t l = {};
    l.width = 3;
    l.in_cursor = true;
    l.drives_range = true;

    l.id = "outlet";
    l.name = i18n_get(STR_HISTORY_OUTLET);
    l.tag_name = "out";
    l.field = temp_field(offsetof(history_telemetry_sample_t, outlet_deci_c),
                         HISTORY_TELEMETRY_OUTLET_VALID);
    l.geom = TSCHART_GEOM_LINE;
    // Recorded in whole degrees: drawn through each plateau's midpoint rather
    // than as a staircase.
    l.interp = TSCHART_INTERP_PLATEAU;
    l.color = COLOR_OUTLET;
    tschart_add_layer(temps_pane, &l);

    l.id = "inlet";
    l.name = i18n_get(STR_HISTORY_INLET);
    l.tag_name = "in";
    l.field = temp_field(offsetof(history_telemetry_sample_t, inlet_deci_c),
                         HISTORY_TELEMETRY_INLET_VALID);
    l.color = COLOR_INLET;
    tschart_add_layer(temps_pane, &l);

    l.id = "setpoint";
    l.name = i18n_get(STR_HISTORY_SETPOINT);
    l.tag_name = "set";
    l.field = temp_field(offsetof(history_telemetry_sample_t, setpoint_deci_c),
                         HISTORY_TELEMETRY_SETPOINT_VALID);
    l.geom = TSCHART_GEOM_STEP;
    l.interp = TSCHART_INTERP_NONE;
    l.color = COLOR_SETPOINT;
    tschart_add_layer(temps_pane, &l);

    l.id = "power";
    l.name = i18n_get(STR_HISTORY_VIEW_POWER);
    l.tag_name = "";
    l.field = {};
    l.field.offset = offsetof(history_telemetry_sample_t, average_power_w);
    l.field.type = sizeof(history_telemetry_sample_t::average_power_w) == 2 ? TSCHART_U16
                                                                            : TSCHART_U32;
    l.field.flags_offset = offsetof(history_telemetry_sample_t, flags);
    l.field.flags_mask = HISTORY_TELEMETRY_POWER_VALID;
    l.geom = TSCHART_GEOM_STEP;
    l.fill = true;
    l.fill_opa = LV_OPA_30;
    l.color = COLOR_POWER;
    tschart_add_layer(power_pane, &l);

    state.temps_group = tschart_add_group(c, "temps", i18n_get(STR_HISTORY_VIEW_TEMPS),
                                          COLOR_OUTLET, TSCHART_SWATCH_LINE);
    tschart_group_add_pane(state.temps_group, temps_pane);
    state.power_group = tschart_add_group(c, "power", i18n_get(STR_HISTORY_VIEW_POWER),
                                          COLOR_POWER, TSCHART_SWATCH_FILL);
    tschart_group_add_pane(state.power_group, power_pane);
    tschart_group_set_visible(state.temps_group, state.temps_on);
    tschart_group_set_visible(state.power_group, state.power_on);

    tschart_add_key(c, "heating", i18n_get(STR_HISTORY_LEGEND_HEATING), COLOR_HEATING,
                    TSCHART_SPAN_FILL);
    tschart_add_key(c, "cooling", i18n_get(STR_HISTORY_LEGEND_COOLING), COLOR_COOLING,
                    TSCHART_SPAN_FILL);
    tschart_add_key(c, "hot_water", i18n_get(STR_HISTORY_LEGEND_HOT_WATER),
                    COLOR_HOT_WATER, TSCHART_SPAN_FILL);
    tschart_add_key(c, "defrost", i18n_get(STR_HISTORY_LEGEND_DEFROST), COLOR_DEFROST,
                    TSCHART_SPAN_FILL);
    tschart_add_key(c, "fault", i18n_get(STR_HISTORY_LEGEND_FAULT), COLOR_FAULT,
                    TSCHART_SPAN_HATCH);
    tschart_set_header(c, TSCHART_HEADER_CHIPS | TSCHART_HEADER_KEY |
                              TSCHART_HEADER_TAG_BTN);
    if (state.view_saved) {
        tschart_set_axis_open(c, state.axis_open);
        tschart_set_end_tags(c, state.tags_on);
    }
    // Set last: tags every child created above.
    tschart_set_id(c, "temperature_history_chart");

    const uint32_t earliest =
        state.latest_end > RETENTION_SECONDS ? state.latest_end - RETENTION_SECONDS : 0;
    tschart_set_bounds(c, earliest, state.latest_end);
    tschart_set_pan_snap(c, history_window_snap(state.window_seconds));
    lv_obj_add_event_cb(c, chart_event_cb, LV_EVENT_ALL, nullptr);
    return c;
}

#if CONFIG_TEST_ENDPOINTS
// Test hook: move the cursor as a finger at screen x would, then force a
// refresh and time both. Caller holds the display lock.
extern "C" bool heatpump_history_profile_cursor(int32_t x,
                                                history_cursor_profile_t* out) {
    if (state.chart == nullptr || state.cur == nullptr || state.cur->count == 0) {
        return false;
    }
    tschart_profile_reset(state.chart);
    int64_t t0 = esp_timer_get_time();
    out->cursor = tschart_set_cursor_x(state.chart, x);
    out->press_us = esp_timer_get_time() - t0;
    t0 = esp_timer_get_time();
    lv_refr_now(nullptr);
    out->refr_us = esp_timer_get_time() - t0;
    tschart_profile_t p;
    tschart_profile_get(state.chart, &p);
    out->samples = (uint32_t)state.cur->count;
    out->power_view = state.power_on;
    out->draw_calls = p.draw_calls;
    out->draw_us = p.draw_us;
    out->build_us = p.build_us;
    out->cached = p.cached;
    return true;
}
#endif

lv_obj_t* heatpump_history_create_in(lv_obj_t* parent) {
    if (state.shown || parent == nullptr) return nullptr;
    state.shown = true;
    state.session++;

    time_t now;
    time(&now);
    uint32_t latest = 0;
    if (time_mgr_is_synced() && now > 0) {
        latest = ((uint32_t)now + HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC - 1) /
                 HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC *
                 HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC;
    } else {
        history_storage_latest_telemetry_timestamp(&latest);
        if (latest > 0) latest += HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC;
    }
    if (latest == 0) latest = state.window_seconds;
    state.latest_end = latest;

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
    lv_obj_set_style_text_align(state.range_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_user_data(state.range_label, (void*)"temperature_history_range");

    char latest_text[48];
    snprintf(latest_text, sizeof(latest_text), "%s  " LV_SYMBOL_NEXT,
             i18n_get(STR_HISTORY_LATEST));
    state.latest_button = create_button(header, latest_text, "temperature_history_latest",
                                        latest_cb);
    lv_obj_set_flex_grow(state.latest_button, 0);
    lv_obj_set_size(state.latest_button, LV_SIZE_CONTENT, LATEST_BUTTON_H);
    lv_obj_set_style_pad_hor(state.latest_button, 22, LV_PART_MAIN);
    lv_obj_set_style_bg_color(state.latest_button, UI_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_obj_get_child(state.latest_button, 0),
                                lv_color_hex(0x062033), LV_PART_MAIN);
    lv_obj_add_flag(state.latest_button, LV_OBJ_FLAG_HIDDEN);

    // Zoom presets, widest first.
    static const char* const kZoomTags[] = {"history_zoom_8h", "history_zoom_4h",
                                            "history_zoom_2h", "history_zoom_1h"};
    static_assert(sizeof(kZoomTags) / sizeof(kZoomTags[0]) == ZOOM_COUNT,
                  "one tag per zoom preset");
    lv_obj_t* zoom = lv_obj_create(state.overlay);
    lv_obj_remove_style_all(zoom);
    lv_obj_set_size(zoom, LV_PCT(100), 52);
    lv_obj_set_flex_flow(zoom, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(zoom, 12, LV_PART_MAIN);
    lv_obj_clear_flag(zoom, LV_OBJ_FLAG_SCROLLABLE);
    for (size_t i = 0; i < ZOOM_COUNT; i++) {
        char text[16];
        format_duration(text, sizeof(text), HISTORY_WINDOW_PRESETS[i]);
        state.zoom_buttons[i] = create_button(zoom, text, kZoomTags[i], zoom_cb);
        lv_obj_set_height(state.zoom_buttons[i], 52);
    }

    // The window's energy, while the power pane is on. A fixed-height row so
    // toggling power doesn't resize the chart.
    lv_obj_t* totals = lv_obj_create(state.overlay);
    lv_obj_remove_style_all(totals);
    lv_obj_set_size(totals, LV_PCT(100), 30);
    lv_obj_clear_flag(totals, LV_OBJ_FLAG_SCROLLABLE);
    state.power_label = lv_label_create(totals);
    lv_obj_set_style_text_font(state.power_label, &montserrat_24_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.power_label, UI_COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(state.power_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_user_data(state.power_label, (void*)"history_power_total");

    state.chart = create_chart(state.overlay);

    lv_obj_t* actions = lv_obj_create(state.overlay);
    lv_obj_remove_style_all(actions);
    lv_obj_set_size(actions, LV_PCT(100), 68);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(actions, 12, LV_PART_MAIN);
    state.previous_button = create_button(actions, "", "temperature_history_previous",
                                          previous_cb);
    state.back_hour_button = create_button(actions, "", "temperature_history_back_hour",
                                           back_hour_cb);
    state.forward_hour_button = create_button(
        actions, "", "temperature_history_forward_hour", forward_hour_cb);
    state.next_button = create_button(actions, "", "temperature_history_next", next_cb);

    apply_zoom();
    update_power_label();
    if (state.pending_center != 0) {
        const uint32_t t = state.pending_center;
        state.pending_center = 0;
        request_window(history_window_center(t, state.window_seconds,
                                             history_window_snap(state.window_seconds),
                                             latest));
    } else {
        request_window(latest);
    }
    return state.overlay;
}

void heatpump_history_hide(void) {
    if (!state.shown) return;
    state.shown = false;
    state.session++;
    state.has_queued = false;
    // Deleting the chart releases its dataset (and clears state.cur).
    if (state.overlay) lv_obj_delete(state.overlay);
    state.overlay = nullptr;
    state.chart = nullptr;
    state.cur = nullptr;
    state.source = nullptr;
    state.temps_group = nullptr;
    state.power_group = nullptr;
    state.range_label = nullptr;
    state.power_label = nullptr;
    state.previous_button = nullptr;
    state.back_hour_button = nullptr;
    state.forward_hour_button = nullptr;
    state.next_button = nullptr;
    state.latest_button = nullptr;
    for (auto& button : state.zoom_buttons) button = nullptr;
}

bool heatpump_history_is_shown(void) { return state.shown; }

void heatpump_history_open_centered(uint32_t timestamp) {
    if (state.shown) {
        request_window(history_window_center(timestamp, state.window_seconds,
                                             history_window_snap(state.window_seconds),
                                             state.latest_end));
    } else {
        state.pending_center = timestamp;
    }
}

void heatpump_history_open_span(uint32_t start, uint32_t end) {
    if (end < start) end = start;
    // The whole span if it fits in the window, otherwise its start.
    heatpump_history_open_centered(end - start <= state.window_seconds
                                       ? start + (end - start) / 2
                                       : start);
}
