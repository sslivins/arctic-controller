/*
 * Arctic Heat Pump Controller
 * Energy (History tab segment). See energy_screen.h.
 *
 * Days come from the energy ledger's NVS store (closed days) plus the open
 * day from the controller; runs come from the 30 s telemetry journal, which
 * only reaches back about two weeks. Both are read on a worker task, since
 * flash reads would stall the UI.
 */
#include "energy_screen.h"

#include "app_preferences.h"
#include "energy_store.h"
#include "fonts/fonts.h"
#include "heatpump_controller.h"
#include "heatpump_history_screen.h"
#include "history_storage.h"
#include "history_tab.h"
#include "i18n/i18n.h"
#include "nav_bar.h"
#include "run_store.h"
#include "time_manager.h"
#include "tsbars.h"
#include "ui_common.h"
#include <bsp/m5stack_tab5.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <initializer_list>
#include <new>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char* TAG = "energy_screen";

static constexpr int WINDOW_DAYS = 14;
// Days loaded either side of the window, so a drag reveals real bars.
static constexpr int PAD_DAYS = WINDOW_DAYS;
static constexpr int BUF_DAYS = WINDOW_DAYS + 2 * PAD_DAYS;
// About as far back as the store keeps (energy::kMonthsKept months).
static constexpr int MAX_DAYS_BACK = 31 * energy::kMonthsKept;
static constexpr size_t MAX_RUNS = 48;
static constexpr uint32_t RUN_GAP_SECONDS = 75;
// A day's estimated COP needs this much covered electricity to mean anything.
static constexpr uint32_t COP_MIN_COVERED_WH = 200;
static constexpr uint32_t REFRESH_MS = 60 * 1000;

#define COLOR_BG        lv_color_hex(0x1a1a2e)
#define COLOR_PANEL     lv_color_hex(0x16213e)
#define COLOR_GRID      lv_color_hex(0x3d4f6f)
#define COLOR_BUTTON    lv_color_hex(0x3d4f6f)
#define COLOR_HEATING   lv_color_hex(0xf97316)
#define COLOR_COOLING   lv_color_hex(0x3b82f6)
#define COLOR_HOT_WATER lv_color_hex(0xfbbf24)
#define COLOR_STANDBY   lv_color_hex(0x94a3b8)
#define COLOR_COP       lv_color_hex(0x4ade80)
#define COLOR_ON_ACCENT lv_color_hex(0x062033)

namespace {

struct MainData {
    uint32_t generation;
    uint32_t end_date;    // last day of the window (YYYYMMDD)
    uint32_t today_date;  // 0 = clock not set
    bool have_today;
    bool have_yesterday;
    energy::DayRecord today;
    energy::DayRecord yesterday;
    // One slot per day, oldest first: PAD_DAYS before the window, the window,
    // then PAD_DAYS after it. date 0 = no record.
    energy::DayRecord days[BUF_DAYS];
};

struct Run {
    uint32_t start;
    uint32_t end;
    uint32_t wh;
    uint8_t mode;
    bool has_outdoor;
    int16_t outdoor_deci;
    int32_t cop_x10;  // -1 = no estimate
};

struct RunsResult {
    uint32_t generation;
    uint32_t date;
    esp_err_t error;
    size_t samples;
    size_t count;
    size_t total;
    Run runs[MAX_RUNS];
};

}  // namespace

static struct {
    bool shown = false;
    lv_obj_t* root = nullptr;
    // Main view
    lv_obj_t* main_view = nullptr;
    lv_obj_t* range_label = nullptr;
    lv_obj_t* latest_button = nullptr;
    lv_obj_t* tile_value[3] = {};
    lv_obj_t* tile_cop[3] = {};
    lv_obj_t* tile_title[3] = {};
    lv_obj_t* chart = nullptr;  // tsbars
    lv_obj_t* prev_window_button = nullptr;
    lv_obj_t* prev_day_button = nullptr;
    lv_obj_t* next_day_button = nullptr;
    lv_obj_t* next_window_button = nullptr;
    lv_timer_t* refresh_timer = nullptr;
    MainData* data = nullptr;  // PSRAM
    uint32_t end_date = 0;
    uint32_t main_generation = 0;
    bool main_running = false;
    bool main_pending = false;
    uint32_t selected_date = 0;
    // Day view
    lv_obj_t* day_view = nullptr;
    lv_obj_t* day_title = nullptr;
    lv_obj_t* stat_value[4] = {};
    lv_obj_t* stat_sub[4] = {};
    lv_obj_t* outdoor_name = nullptr;
    lv_obj_t* facts_label = nullptr;
    lv_obj_t* runs_title = nullptr;
    lv_obj_t* runs_list = nullptr;
    lv_obj_t* runs_status = nullptr;
    lv_obj_t* settings_label = nullptr;
    RunsResult* runs = nullptr;  // PSRAM
    uint32_t day_date = 0;
    uint32_t day_generation = 0;
    bool day_running = false;
    bool day_pending = false;
} s;

// ---------------------------------------------------------------------------
// Dates and formatting
// ---------------------------------------------------------------------------

static uint32_t date_of(const struct tm& t) {
    return (uint32_t)(t.tm_year + 1900) * 10000 + (uint32_t)(t.tm_mon + 1) * 100 +
           (uint32_t)t.tm_mday;
}

static struct tm tm_of(uint32_t date, int add_days = 0) {
    struct tm t = {};
    t.tm_year = (int)(date / 10000) - 1900;
    t.tm_mon = (int)(date / 100 % 100) - 1;
    t.tm_mday = (int)(date % 100) + add_days;
    t.tm_isdst = -1;
    return t;
}

static uint32_t date_add(uint32_t date, int days) {
    struct tm t = tm_of(date, days);
    t.tm_hour = 12;
    mktime(&t);
    return date_of(t);
}

// Local midnight at the start of `date`.
static uint32_t day_start(uint32_t date) {
    struct tm t = tm_of(date);
    return (uint32_t)mktime(&t);
}

// Whole days from `a` to `b` (negative when b is earlier).
static int day_diff(uint32_t a, uint32_t b) {
    const int64_t secs = (int64_t)day_start(b) - (int64_t)day_start(a);
    return (int)((secs + (secs >= 0 ? 43200 : -43200)) / 86400);
}

static uint32_t today_date() {
    if (!time_mgr_is_synced()) return 0;
    time_t now = time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    return date_of(local);
}

// "Oct 13" / "13 oct.".
static void format_day(char* buf, size_t len, uint32_t date) {
    const unsigned day = date % 100;
    const unsigned month = date / 100 % 100;
    const char* name = (month >= 1 && month <= 12)
        ? i18n_get((string_id_t)(STR_EVENT_MONTH_JAN + month - 1))
        : "?";
    if (i18n_get_language() == LANG_ENGLISH) {
        snprintf(buf, len, "%s %u", name, day);
    } else {
        snprintf(buf, len, "%u %s", day, name);
    }
}

// Whole kWh from 100 kWh up, otherwise tenths.
static void format_kwh_number(char* buf, size_t len, uint32_t wh) {
    if (wh >= 100000) {
        snprintf(buf, len, "%lu", (unsigned long)((wh + 500) / 1000));
    } else {
        const uint32_t tenths = (wh + 50) / 100;
        snprintf(buf, len, "%lu.%lu", (unsigned long)(tenths / 10),
                 (unsigned long)(tenths % 10));
    }
}

static void format_kwh(char* buf, size_t len, uint32_t wh) {
    char num[16];
    format_kwh_number(num, sizeof(num), wh);
    snprintf(buf, len, "%s kWh", num);
}

static uint32_t thermal_wh(const energy::DayRecord& r) {
    uint32_t t = 0;
    for (int m = 0; m < energy::kModes; m++) t += r.thermal_wh[m];
    return t;
}

static uint32_t covered_wh(const energy::DayRecord& r) {
    uint32_t t = 0;
    for (int m = 0; m < energy::kModes; m++) t += r.covered_elec_wh[m];
    return t;
}

// COP x10, or -1 when too little was covered to say.
static int32_t cop_x10(uint32_t thermal, uint32_t covered) {
    if (covered < COP_MIN_COVERED_WH) return -1;
    return (int32_t)(((uint64_t)thermal * 10 + covered / 2) / covered);
}

static void format_cop(char* buf, size_t len, int32_t x10) {
    if (x10 < 0) {
        snprintf(buf, len, "--");
    } else {
        snprintf(buf, len, "%ld.%ld", (long)(x10 / 10), (long)(x10 % 10));
    }
}

static void format_temp(char* buf, size_t len, int32_t deci_c, bool unit) {
    int32_t v = deci_c;
    const bool f = app_prefs_get_temp_unit() == TEMP_UNIT_FAHRENHEIT;
    if (f) v = (v * 9) / 5 + 320;
    const long whole = (v >= 0) ? (v + 5) / 10 : -((-v + 5) / 10);
    if (unit) {
        snprintf(buf, len, "%ld°%s", whole, f ? "F" : "C");
    } else {
        snprintf(buf, len, "%ld°", whole);
    }
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

static void format_duration(char* buf, size_t len, uint32_t seconds) {
    const uint32_t minutes = (seconds + 30) / 60;
    if (minutes >= 60) {
        snprintf(buf, len, "%lu h %02lu min", (unsigned long)(minutes / 60),
                 (unsigned long)(minutes % 60));
    } else {
        snprintf(buf, len, "%lu min", (unsigned long)minutes);
    }
}

static lv_color_t mode_color(uint8_t mode) {
    switch (mode) {
        case HISTORY_TELEMETRY_MODE_HEATING: return COLOR_HEATING;
        case HISTORY_TELEMETRY_MODE_COOLING: return COLOR_COOLING;
        case HISTORY_TELEMETRY_MODE_HOT_WATER: return COLOR_HOT_WATER;
        default: return COLOR_STANDBY;
    }
}

// ---------------------------------------------------------------------------
// Shared widgets
// ---------------------------------------------------------------------------

static lv_obj_t* make_row(lv_obj_t* parent, int32_t height) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), height);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font,
                            lv_color_t color, const char* text = "") {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    return label;
}

static lv_obj_t* make_button(lv_obj_t* parent, const char* text,
                             const char* tag, lv_event_cb_t callback) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_height(button, 64);
    lv_obj_set_flex_grow(button, 1);
    lv_obj_set_style_bg_color(button, COLOR_BUTTON, LV_PART_MAIN);
    lv_obj_set_style_radius(button, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_set_user_data(button, (void*)tag);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* label = make_label(button, UI_FONT_SMALL, UI_COLOR_TEXT, text);
    lv_obj_center(label);
    return button;
}

static lv_obj_t* make_panel(lv_obj_t* parent) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_style_bg_color(panel, COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, COLOR_GRID, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 12, LV_PART_MAIN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

static void set_enabled(lv_obj_t* obj, bool enabled) {
    if (obj == nullptr) return;
    if (enabled) {
        lv_obj_remove_state(obj, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
    }
}

static void set_hidden(lv_obj_t* obj, bool hidden) {
    if (obj == nullptr) return;
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void* psram_alloc(size_t size) {
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) memset(p, 0, size);
    return p;
}

// ---------------------------------------------------------------------------
// Main view: data
// ---------------------------------------------------------------------------

static void open_day(uint32_t date);
static void request_main();
static void sync_chart();

static void main_complete(void* arg) {
    auto* data = static_cast<MainData*>(arg);
    s.main_running = false;
    if (!s.shown || data->generation != s.main_generation) {
        heap_caps_free(data);
        if (s.shown && s.main_pending) {
            s.main_pending = false;
            request_main();
        }
        return;
    }
    MainData* old = s.data;
    s.data = data;
    sync_chart();
    heap_caps_free(old);

    // Tiles: today, yesterday, the window.
    struct Tile { bool have; uint32_t elec; uint32_t thermal; uint32_t covered; };
    Tile tiles[3] = {};
    if (data->have_today) {
        tiles[0] = {true, energy::total_elec_wh(data->today), thermal_wh(data->today),
                    covered_wh(data->today)};
    }
    if (data->have_yesterday) {
        tiles[1] = {true, energy::total_elec_wh(data->yesterday),
                    thermal_wh(data->yesterday), covered_wh(data->yesterday)};
    }
    for (int i = 0; i < WINDOW_DAYS; i++) {
        const auto& d = data->days[PAD_DAYS + i];
        if (d.date == 0) continue;
        tiles[2].have = true;
        tiles[2].elec += energy::total_elec_wh(d);
        tiles[2].thermal += thermal_wh(d);
        tiles[2].covered += covered_wh(d);
    }
    for (int i = 0; i < 3; i++) {
        char text[32];
        if (tiles[i].have) {
            format_kwh(text, sizeof(text), tiles[i].elec);
        } else {
            snprintf(text, sizeof(text), "--");
        }
        lv_label_set_text(s.tile_value[i], text);
        const int32_t cop = tiles[i].have ? cop_x10(tiles[i].thermal, tiles[i].covered) : -1;
        if (cop >= 0) {
            char num[12];
            format_cop(num, sizeof(num), cop);
            snprintf(text, sizeof(text), i18n_get(STR_ENERGY_COP_FMT), num);
            lv_label_set_text(s.tile_cop[i], text);
        } else {
            lv_label_set_text(s.tile_cop[i], " ");
        }
    }

    if (data->today_date == 0) {
        tsbars_set_status(s.chart, TSCHART_STATUS_EMPTY, i18n_get(STR_HISTORY_WAITING_FOR_TIME));
    } else if (!tiles[2].have) {
        tsbars_set_status(s.chart, TSCHART_STATUS_EMPTY, i18n_get(STR_ENERGY_NO_DATA));
    } else {
        tsbars_set_status(s.chart, TSCHART_STATUS_OK, nullptr);
    }
    if (s.main_pending) {
        s.main_pending = false;
        request_main();
    }
}

static void main_task(void* arg) {
    auto* data = static_cast<MainData*>(arg);
    const uint32_t from = date_add(data->end_date, -(WINDOW_DAYS - 1) - PAD_DAYS);
    uint32_t to = date_add(data->end_date, PAD_DAYS);
    if (data->today_date != 0 && to > data->today_date) to = data->today_date;
    energy::DayRecord* read = static_cast<energy::DayRecord*>(
        psram_alloc(sizeof(energy::DayRecord) * BUF_DAYS));
    if (read != nullptr) {
        const size_t n = energy::store_read_days(from, to, read, BUF_DAYS);
        for (size_t k = 0; k < n; k++) {
            const int i = day_diff(from, read[k].date);
            if (i >= 0 && i < BUF_DAYS) data->days[i] = read[k];
        }
        heap_caps_free(read);
    }
    if (data->today_date != 0) {
        data->have_today = arctic::getEnergyToday(&data->today) &&
                           data->today.date == data->today_date;
        const int i = data->have_today ? day_diff(from, data->today_date) : -1;
        if (i >= 0 && i < BUF_DAYS) data->days[i] = data->today;
        const uint32_t yday = date_add(data->today_date, -1);
        data->have_yesterday =
            energy::store_read_days(yday, yday, &data->yesterday, 1) == 1;
    }
    bsp_display_lock(0);
    lv_async_call(main_complete, data);
    bsp_display_unlock();
    vTaskDelete(nullptr);
}

static void update_main_header() {
    const uint32_t from = date_add(s.end_date, -(WINDOW_DAYS - 1));
    char a[24], b[24], range[56];
    format_day(a, sizeof(a), from);
    format_day(b, sizeof(b), s.end_date);
    snprintf(range, sizeof(range), "%s \xe2\x80\x93 %s", a, b);
    lv_label_set_text(s.range_label, range);

    const uint32_t today = today_date();
    const bool at_latest = today == 0 || s.end_date >= today;
    const uint32_t earliest = today ? date_add(today, -MAX_DAYS_BACK) : s.end_date;
    set_hidden(s.latest_button, at_latest);
    set_enabled(s.next_day_button, !at_latest);
    set_enabled(s.next_window_button, !at_latest);
    set_enabled(s.prev_day_button, from > earliest);
    set_enabled(s.prev_window_button, from > earliest);
}

static void request_main() {
    if (!s.shown) return;
    s.main_generation++;
    if (s.main_running) {
        s.main_pending = true;
        return;
    }
    auto* data = static_cast<MainData*>(psram_alloc(sizeof(MainData)));
    if (data == nullptr) return;
    data->generation = s.main_generation;
    data->end_date = s.end_date;
    data->today_date = today_date();
    s.main_running = true;
    if (xTaskCreate(main_task, "energy_days", 4096, data, 3, nullptr) != pdPASS) {
        s.main_running = false;
        heap_caps_free(data);
        tsbars_set_status(s.chart, TSCHART_STATUS_ERROR, i18n_get(STR_ENERGY_LOAD_ERROR));
    }
}

// The oldest window end that can be shown.
static uint32_t earliest_end(uint32_t today) {
    return date_add(today, -MAX_DAYS_BACK + WINDOW_DAYS - 1);
}

static void show_window(uint32_t end_date) {
    const uint32_t today = today_date();
    if (today != 0) {
        if (end_date > today) end_date = today;
        const uint32_t oldest = earliest_end(today);
        if (end_date < oldest) end_date = oldest;
    }
    s.end_date = end_date;
    update_main_header();
    sync_chart();
    request_main();
}

static void prev_window_cb(lv_event_t*) { show_window(date_add(s.end_date, -WINDOW_DAYS)); }
static void prev_day_cb(lv_event_t*) { show_window(date_add(s.end_date, -1)); }
static void next_day_cb(lv_event_t*) { show_window(date_add(s.end_date, 1)); }
static void next_window_cb(lv_event_t*) { show_window(date_add(s.end_date, WINDOW_DAYS)); }
static void latest_cb(lv_event_t*) { show_window(today_date()); }

static void refresh_timer_cb(lv_timer_t*) {
    if (!s.shown || lv_obj_has_flag(s.main_view, LV_OBJ_FLAG_HIDDEN)) return;
    const uint32_t today = today_date();
    // Follow midnight while parked on the latest window.
    if (today != 0 && s.data != nullptr && s.data->today_date != 0 &&
        s.end_date == s.data->today_date && s.end_date != today) {
        s.end_date = today;
        update_main_header();
    }
    if (today == 0 || s.end_date >= today) request_main();
}

// ---------------------------------------------------------------------------
// Main view: chart
// ---------------------------------------------------------------------------

static bool mode_wh(const tsbars_bucket_t* b, int32_t* out, void* ud) {
    const auto* r = static_cast<const energy::DayRecord*>(b->data);
    if (r == nullptr) return false;
    *out = (int32_t)r->elec_wh[(int)(intptr_t)ud];
    return true;
}

static bool day_cop(const tsbars_bucket_t* b, int32_t* out, void*) {
    const auto* r = static_cast<const energy::DayRecord*>(b->data);
    if (r == nullptr) return false;
    *out = cop_x10(thermal_wh(*r), covered_wh(*r));
    return *out >= 0;
}

static size_t kwh_axis_fmt(int32_t wh, tschart_fmt_ctx_t, char* buf, size_t cap, void*) {
    const int n = wh % 1000 == 0
        ? snprintf(buf, cap, "%ld", (long)(wh / 1000))
        : snprintf(buf, cap, "%ld.%ld", (long)(wh / 1000), (long)(wh % 1000 / 100));
    return n > 0 ? (size_t)n : 0;
}

static size_t cop_axis_fmt(int32_t x10, tschart_fmt_ctx_t, char* buf, size_t cap, void*) {
    const int n = snprintf(buf, cap, "%ld", (long)(x10 / 10));
    return n > 0 ? (size_t)n : 0;
}

static size_t day_label(const tsbars_bucket_t* b, char* buf, size_t cap, void*) {
    const int n = snprintf(buf, cap, "%u", (unsigned)(b->key % 100));
    return n > 0 ? (size_t)n : 0;
}

// A drag can go forward to today and back to the oldest day kept.
static void update_pan_limits() {
    const uint32_t today = today_date();
    if (today == 0) {
        tsbars_set_pan_limits(s.chart, 0, 0);
        return;
    }
    tsbars_set_pan_limits(s.chart, LV_MAX(0, day_diff(earliest_end(today), s.end_date)),
                          LV_MAX(0, day_diff(s.end_date, today)));
}

// Hands the loaded days, window and padding, to the chart with the window
// ending on s.end_date. Until a new window loads, the chart keeps its view
// when s.end_date isn't among them.
static void sync_chart() {
    if (s.chart == nullptr || s.data == nullptr) return;
    const MainData& d = *s.data;
    const uint32_t from = date_add(d.end_date, -(WINDOW_DAYS - 1) - PAD_DAYS);
    tsbars_bucket_t buckets[BUF_DAYS];
    size_t n = 0;
    bool have_end = false;
    for (int i = 0; i < BUF_DAYS; i++) {
        const uint32_t date = date_add(from, i);
        if (d.today_date != 0 && date > d.today_date) break;
        const energy::DayRecord* r = d.days[i].date != 0 ? &d.days[i] : nullptr;
        uint8_t flags = r ? TSBARS_SELECTABLE : 0;
        // Today is still filling up.
        if (date == d.today_date) flags |= TSBARS_ACCENT | TSBARS_PROVISIONAL;
        buckets[n++] = {date, flags, r};
        have_end = have_end || date == s.end_date;
    }
    tsbars_set_buckets(s.chart, buckets, n,
                       have_end ? TSBARS_VIEW_LAST_KEY : TSBARS_VIEW_KEEP, s.end_date);
    tsbars_set_selected(s.chart, s.selected_date);
    update_pan_limits();
}

static void chart_select_cb(lv_event_t* e) {
    const auto* sel = static_cast<const tsbars_select_t*>(lv_event_get_param(e));
    if (sel != nullptr) open_day(sel->key);
}

// A drag moved the window; the chart already shows it from the loaded days.
static void chart_view_cb(lv_event_t* e) {
    const auto* v = static_cast<const tsbars_view_changed_t*>(lv_event_get_param(e));
    if (v != nullptr && v->view.last_key != 0) show_window(v->view.last_key);
}

static void build_chart(lv_obj_t* parent) {
    lv_obj_t* c = tsbars_create(parent);
    s.chart = c;
    lv_obj_set_width(c, LV_PCT(100));
    lv_obj_set_flex_grow(c, 1);
    lv_obj_set_style_bg_color(c, COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(c, COLOR_GRID, LV_PART_MAIN);
    lv_obj_set_style_border_width(c, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(c, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(c, 0, LV_PART_MAIN);
    lv_obj_set_style_text_color(c, UI_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_user_data(c, (void*)"energy_chart");
    tsbars_set_small_font(c, &montserrat_16_latin);
    tsbars_set_accent(c, UI_COLOR_ACCENT);
    tsbars_set_count(c, WINDOW_DAYS);
    tsbars_set_labels(c, day_label, nullptr, 0);

    // Wh, stepping at least 0.1 kWh and spanning at least 1 kWh.
    tsbars_pane_cfg_t kwh = {};
    kwh.id = "kwh";
    kwh.weight = 8;
    kwh.unit = "kWh";
    kwh.unit_at = TSBARS_UNIT_TOP;
    kwh.auto_range = true;
    kwh.has_min = true;
    kwh.min = 0;
    kwh.include_zero = true;
    kwh.min_span = 1000;
    kwh.min_step = 100;
    kwh.max_ticks = 5;
    kwh.fmt = kwh_axis_fmt;
    tsbars_pane_t* bars = tsbars_add_pane(c, &kwh);
    // Modes stacked from the bottom, as on the web. Idle draw is below the
    // unit's 100 W metering step, so there is no standby segment.
    struct Mode { const char* id; lv_color_t color; int bucket; };
    const Mode modes[] = {
        {"heating", COLOR_HEATING, energy::BUCKET_HEATING},
        {"cooling", COLOR_COOLING, energy::BUCKET_COOLING},
        {"hot_water", COLOR_HOT_WATER, energy::BUCKET_HOT_WATER},
    };
    for (const Mode& m : modes) {
        tsbars_series_cfg_t sc = {};
        sc.id = m.id;
        sc.color = m.color;
        sc.geom = TSBARS_GEOM_BAR;
        sc.stack = 1;
        sc.value = mode_wh;
        sc.ud = (void*)(intptr_t)m.bucket;
        tsbars_add_series(bars, &sc);
    }

    // The estimated COP in whole numbers, labelled at the ends.
    tsbars_pane_cfg_t cop = {};
    cop.id = "cop";
    cop.weight = 1;
    cop.min_height = 64;
    cop.unit = "COP";
    cop.unit_at = TSBARS_UNIT_MIDDLE;
    cop.has_unit_color = true;
    cop.unit_color = COLOR_COP;
    cop.auto_range = true;
    cop.min_span = 10;
    cop.min_step = 10;
    cop.max_ticks = 2;
    cop.ends_only = true;
    cop.fmt = cop_axis_fmt;
    cop.empty_text = i18n_get(STR_ENERGY_NO_COP);
    tsbars_pane_t* cop_pane = tsbars_add_pane(c, &cop);
    tsbars_series_cfg_t line = {};
    line.id = "cop";
    line.color = COLOR_COP;
    line.geom = TSBARS_GEOM_LINE;
    line.width = 2;
    line.point_radius = 5;
    line.value = day_cop;
    tsbars_add_series(cop_pane, &line);

    tsbars_set_status(c, TSCHART_STATUS_LOADING, i18n_get(STR_ENERGY_LOADING));
    lv_obj_add_event_cb(c, chart_select_cb, TSBARS_EVENT_SELECT, nullptr);
    lv_obj_add_event_cb(c, chart_view_cb, TSBARS_EVENT_VIEW_CHANGED, nullptr);
}

static void tile_cb(lv_event_t* e) {
    if (s.data == nullptr) return;
    const int which = (int)(intptr_t)lv_event_get_user_data(e);
    if (which == 0 && s.data->have_today) open_day(s.data->today.date);
    if (which == 1 && s.data->have_yesterday) open_day(s.data->yesterday.date);
}

// ---------------------------------------------------------------------------
// Day view
// ---------------------------------------------------------------------------

static void request_runs();

static const energy::DayRecord* day_record(uint32_t date) {
    if (s.data == nullptr) return nullptr;
    if (s.data->have_today && s.data->today.date == date) return &s.data->today;
    if (s.data->have_yesterday && s.data->yesterday.date == date) return &s.data->yesterday;
    for (int i = 0; i < BUF_DAYS; i++) {
        if (s.data->days[i].date == date) return &s.data->days[i];
    }
    return nullptr;
}

static void fill_day_stats(const energy::DayRecord& r) {
    char title[48], day[24];
    format_day(day, sizeof(day), r.date);
    if (s.data && r.date == s.data->today_date) {
        snprintf(title, sizeof(title), "%s \xc2\xb7 %s", day, i18n_get(STR_ENERGY_SO_FAR));
    } else {
        snprintf(title, sizeof(title), "%s", day);
    }
    lv_label_set_text(s.day_title, title);

    char value[32], sub[96], num[16];
    format_kwh(value, sizeof(value), energy::total_elec_wh(r));
    lv_label_set_text(s.stat_value[0], value);
    lv_label_set_text(s.stat_sub[0], " ");

    const uint32_t thermal = thermal_wh(r);
    format_kwh(value, sizeof(value), thermal);
    lv_label_set_text(s.stat_value[1], value);
    if (r.defrost_thermal_wh < 0) {
        format_kwh_number(num, sizeof(num), (uint32_t)(-r.defrost_thermal_wh));
        snprintf(sub, sizeof(sub), "%s -%s kWh", i18n_get(STR_HISTORY_LEGEND_DEFROST), num);
    } else {
        snprintf(sub, sizeof(sub), " ");
    }
    lv_label_set_text(s.stat_sub[1], sub);

    format_cop(value, sizeof(value), cop_x10(thermal, covered_wh(r)));
    lv_label_set_text(s.stat_value[2], value);
    lv_label_set_text(s.stat_sub[2], " ");

    // The running average is what COP depends on; days booked before it
    // existed (or without a run) show the whole-day average instead.
    const bool run_avg = (r.flags & energy::DAY_RUN_OUTDOOR_SET) != 0;
    lv_label_set_text(s.outdoor_name,
                      i18n_get(run_avg ? STR_ENERGY_OUTDOOR_RUNNING : STR_ENERGY_OUTDOOR));
    if (r.flags & energy::DAY_OUTDOOR_SET) {
        char lo[16], hi[16];
        format_temp(lo, sizeof(lo), r.outdoor_min_deci, false);
        format_temp(hi, sizeof(hi), r.outdoor_max_deci, false);
        if (run_avg) {
            char day_avg[16], day_txt[32];
            format_temp(value, sizeof(value), r.outdoor_run_avg_deci, true);
            format_temp(day_avg, sizeof(day_avg), r.outdoor_avg_deci, false);
            snprintf(day_txt, sizeof(day_txt), i18n_get(STR_ENERGY_DAY_AVG_FMT), day_avg);
            snprintf(sub, sizeof(sub), "%s \xc2\xb7 %s \xe2\x80\x93 %s", day_txt, lo, hi);
        } else {
            format_temp(value, sizeof(value), r.outdoor_avg_deci, true);
            snprintf(sub, sizeof(sub), "%s \xe2\x80\x93 %s", lo, hi);
        }
    } else {
        snprintf(value, sizeof(value), "--");
        snprintf(sub, sizeof(sub), " ");
    }
    lv_label_set_text(s.stat_value[3], value);
    lv_label_set_text(s.stat_sub[3], sub);

    char starts[32], defrosts[32], facts[72];
    snprintf(starts, sizeof(starts), i18n_get(STR_ENERGY_STARTS_FMT), (unsigned)r.starts);
    snprintf(defrosts, sizeof(defrosts), i18n_get(STR_ENERGY_DEFROSTS_FMT),
             (unsigned)r.defrosts);
    snprintf(facts, sizeof(facts), "%s \xc2\xb7 %s", starts, defrosts);
    lv_label_set_text(s.facts_label, facts);

    if (r.flags & energy::DAY_SETTINGS_SET) {
        char flow[16], text[96];
        snprintf(flow, sizeof(flow), "%u.%u", (unsigned)(r.flow_lpm_x10 / 10),
                 (unsigned)(r.flow_lpm_x10 % 10));
        if (r.flow_lpm_x10 % 10 == 0) {
            snprintf(flow, sizeof(flow), "%u", (unsigned)(r.flow_lpm_x10 / 10));
        }
        snprintf(text, sizeof(text), i18n_get(STR_ENERGY_SETTINGS_FMT), flow,
                 (unsigned)r.glycol_pct);
        lv_label_set_text(s.settings_label, text);
        set_hidden(s.settings_label, false);
    } else {
        set_hidden(s.settings_label, true);
    }
}

static void open_run_async(void* arg) {
    const uint32_t mid = (uint32_t)(uintptr_t)arg;
    heatpump_history_open_centered(mid);
    history_tab_open(HISTORY_SEG_CYCLES);
}

static void run_cb(lv_event_t* e) {
    const size_t i = (size_t)(uintptr_t)lv_event_get_user_data(e);
    if (s.runs == nullptr || i >= s.runs->count) return;
    const Run& r = s.runs->runs[i];
    // Switching segments deletes this row, so not from inside its own event.
    lv_async_call(open_run_async, (void*)(uintptr_t)(r.start + (r.end - r.start) / 2));
}

static void add_run_row(size_t index, const Run& r) {
    lv_obj_t* row = lv_obj_create(s.runs_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, UI_COLOR_BTN_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(row, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(row, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_user_data(row, (void*)"energy_run");
    lv_obj_add_event_cb(row, run_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)index);

    lv_obj_t* top = lv_obj_create(row);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, 14, LV_PART_MAIN);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(top, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* swatch = lv_obj_create(top);
    lv_obj_remove_style_all(swatch);
    lv_obj_set_size(swatch, 16, 16);
    lv_obj_set_style_bg_color(swatch, mode_color(r.mode), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(swatch, 4, LV_PART_MAIN);

    char when[16], duration[24], kwh[24];
    format_clock(when, sizeof(when), r.start);
    format_duration(duration, sizeof(duration), r.end - r.start);
    format_kwh(kwh, sizeof(kwh), r.wh);
    lv_obj_t* t = make_label(top, UI_FONT_SMALL, UI_COLOR_TEXT, when);
    lv_obj_set_width(t, 150);
    lv_obj_t* dur = make_label(top, UI_FONT_SMALL, UI_COLOR_TEXT_DIM, duration);
    lv_obj_set_flex_grow(dur, 1);
    make_label(top, UI_FONT_SMALL, UI_COLOR_TEXT, kwh);
    make_label(top, UI_FONT_SMALL, UI_COLOR_TEXT_DIM, LV_SYMBOL_RIGHT);

    // "COP (est.) 3.4 · Outdoor -2°C", lined up under the start time.
    char cop[16], cop_text[48], outdoor[16], perf[112];
    format_cop(cop, sizeof(cop), r.cop_x10);
    snprintf(cop_text, sizeof(cop_text), i18n_get(STR_ENERGY_COP_FMT), cop);
    if (r.has_outdoor) {
        format_temp(outdoor, sizeof(outdoor), r.outdoor_deci, true);
    } else {
        snprintf(outdoor, sizeof(outdoor), "--");
    }
    snprintf(perf, sizeof(perf), "%s  \xc2\xb7  %s %s", cop_text, i18n_get(STR_ENERGY_OUTDOOR),
             outdoor);
    lv_obj_t* p = make_label(row, UI_FONT_SMALL, UI_COLOR_TEXT_DIM, perf);
    lv_obj_set_style_pad_left(p, 30, LV_PART_MAIN);
    lv_obj_set_user_data(p, (void*)"energy_run_perf");
}

static void runs_complete(void* arg) {
    auto* result = static_cast<RunsResult*>(arg);
    s.day_running = false;
    if (!s.shown || result->generation != s.day_generation) {
        heap_caps_free(result);
        if (s.shown && s.day_pending) {
            s.day_pending = false;
            request_runs();
        }
        return;
    }
    heap_caps_free(s.runs);
    s.runs = result;

    lv_obj_clean(s.runs_list);
    char title[32];
    snprintf(title, sizeof(title), i18n_get(STR_ENERGY_RUNS_FMT), (unsigned)result->total);
    lv_label_set_text(s.runs_title, title);
    for (size_t i = 0; i < result->count; i++) add_run_row(i, result->runs[i]);
    if (result->error != ESP_OK) {
        lv_label_set_text(s.runs_status, i18n_get(STR_HISTORY_STORAGE_ERROR));
    } else if (result->samples == 0) {
        lv_label_set_text(s.runs_status, i18n_get(STR_ENERGY_NO_RUN_DETAIL));
    } else if (result->total == 0) {
        lv_label_set_text(s.runs_status, i18n_get(STR_ENERGY_NO_RUNS));
    }
    set_hidden(s.runs_status, result->error == ESP_OK && result->total > 0);
    if (s.day_pending) {
        s.day_pending = false;
        request_runs();
    }
}

// Run summaries (estimated COP, outdoor temperature) recorded as runs end:
// each goes to the listed run it overlaps most. A run stored under the
// previous day can reach into this one; one not yet stored replaces its
// stored copy.
static void attach_run_summaries(RunsResult* result) {
    constexpr size_t kCap = 2 * energy::kMaxRunsPerDay;
    constexpr size_t kLive = 8;
    uint32_t starts[MAX_RUNS], ends[MAX_RUNS];
    for (size_t i = 0; i < result->count; i++) {
        starts[i] = result->runs[i].start;
        ends[i] = result->runs[i].end;
        result->runs[i].cop_x10 = -1;
        result->runs[i].has_outdoor = false;
    }
    if (result->count == 0) return;
    // In PSRAM: this runs on a small internal stack.
    struct Work {
        energy::RunRecord recs[kCap + kLive];
        energy::RunTotals totals[MAX_RUNS];
    };
    auto* w = static_cast<Work*>(heap_caps_malloc(sizeof(Work), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (w == nullptr) return;
    new (w) Work();
    energy::RunRecord* recs = w->recs;
    energy::RunTotals* totals = w->totals;
    size_t n = energy::store_read_runs(date_add(result->date, -1), recs, kCap);
    n += energy::store_read_runs(result->date, recs + n, kCap - n);
    const size_t live = arctic::getEnergyUnstoredRuns(recs + n, kLive);
    for (size_t i = 0; i < n + live; i++) {
        bool replaced = false;
        for (size_t j = n; i < n && j < n + live && !replaced; j++) {
            replaced = recs[j].start_s == recs[i].start_s;
        }
        if (replaced) continue;
        const int k = energy::best_run(recs[i], starts, ends, result->count,
                                       HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC);
        if (k >= 0) totals[k].add(recs[i]);
    }
    for (size_t i = 0; i < result->count; i++) {
        if (!totals[i].any) continue;
        result->runs[i].cop_x10 = totals[i].cop_x10();
        result->runs[i].has_outdoor = totals[i].outdoor(&result->runs[i].outdoor_deci);
    }
    heap_caps_free(w);
}

// Runs are stretches of compressor-on samples; a gap longer than a couple of
// sample intervals ends one.
static void runs_task(void* arg) {
    auto* result = static_cast<RunsResult*>(arg);
    auto* samples = static_cast<history_telemetry_sample_t*>(
        heap_caps_malloc(sizeof(history_telemetry_sample_t) * HISTORY_TELEMETRY_PAGE_CAPACITY,
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (samples == nullptr) {
        result->error = ESP_ERR_NO_MEM;
    } else {
        const uint32_t start = day_start(result->date);
        const uint32_t end = day_start(date_add(result->date, 1));
        bool open = false;
        Run cur = {};
        uint32_t mode_s[4] = {};
        uint32_t last_t = 0;
        uint64_t joules = 0;
        auto close_run = [&]() {
            if (!open) return;
            open = false;
            cur.wh = (uint32_t)((joules + 1800) / 3600);
            cur.mode = HISTORY_TELEMETRY_MODE_HEATING;
            for (uint8_t m = HISTORY_TELEMETRY_MODE_HEATING;
                 m <= HISTORY_TELEMETRY_MODE_HOT_WATER; m++) {
                if (mode_s[m] > mode_s[cur.mode]) cur.mode = m;
            }
            if (result->count < MAX_RUNS) result->runs[result->count++] = cur;
            result->total++;
        };
        // Three 8 h pages: a page holds 1024 samples, a day is 2880.
        for (uint32_t page = start; page < end && result->error == ESP_OK;
             page += 8 * 3600) {
            const uint32_t page_end = LV_MIN(page + 8 * 3600, end);
            size_t n = 0;
            result->error = history_storage_query_telemetry(
                page, page_end, samples, HISTORY_TELEMETRY_PAGE_CAPACITY, &n);
            if (result->error != ESP_OK) break;
            result->samples += n;
            for (size_t i = 0; i < n; i++) {
                const auto& smp = samples[i];
                const bool running = (smp.flags & HISTORY_TELEMETRY_COMPRESSOR_VALID) &&
                                     (smp.flags & HISTORY_TELEMETRY_COMPRESSOR_RUNNING);
                if (open && smp.timestamp - last_t > RUN_GAP_SECONDS) close_run();
                if (!running) {
                    close_run();
                    continue;
                }
                if (!open) {
                    open = true;
                    cur = {};
                    cur.start = smp.timestamp - HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC;
                    joules = 0;
                    memset(mode_s, 0, sizeof(mode_s));
                }
                cur.end = smp.timestamp;
                last_t = smp.timestamp;
                if (smp.mode < 4) mode_s[smp.mode]++;
                if (smp.flags & HISTORY_TELEMETRY_POWER_VALID) {
                    joules += (uint64_t)smp.average_power_w *
                              HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC;
                }
            }
        }
        close_run();
        heap_caps_free(samples);
        attach_run_summaries(result);
    }
    bsp_display_lock(0);
    lv_async_call(runs_complete, result);
    bsp_display_unlock();
    vTaskDelete(nullptr);
}

static void request_runs() {
    if (!s.shown) return;
    s.day_generation++;
    if (s.day_running) {
        s.day_pending = true;
        return;
    }
    auto* result = static_cast<RunsResult*>(psram_alloc(sizeof(RunsResult)));
    if (result == nullptr) return;
    result->generation = s.day_generation;
    result->date = s.day_date;
    result->error = ESP_OK;
    s.day_running = true;
    if (xTaskCreate(runs_task, "energy_runs", 4096, result, 3, nullptr) != pdPASS) {
        s.day_running = false;
        heap_caps_free(result);
        lv_label_set_text(s.runs_status, i18n_get(STR_HISTORY_STORAGE_ERROR));
        set_hidden(s.runs_status, false);
    }
}

static void open_day(uint32_t date) {
    const energy::DayRecord* r = day_record(date);
    if (r == nullptr) return;
    s.day_date = date;
    s.selected_date = date;
    if (s.chart) tsbars_set_selected(s.chart, date);
    fill_day_stats(*r);
    lv_obj_clean(s.runs_list);
    char title[32];
    snprintf(title, sizeof(title), i18n_get(STR_ENERGY_RUNS_FMT), 0u);
    lv_label_set_text(s.runs_title, title);
    lv_label_set_text(s.runs_status, i18n_get(STR_ENERGY_LOADING));
    set_hidden(s.runs_status, false);
    set_hidden(s.main_view, true);
    set_hidden(s.day_view, false);
    request_runs();
}

static void day_back_cb(lv_event_t*) {
    s.day_generation++;
    set_hidden(s.day_view, true);
    set_hidden(s.main_view, false);
    lv_obj_invalidate(s.chart);
}

// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

static lv_obj_t* make_view(lv_obj_t* parent, const char* tag) {
    lv_obj_t* view = lv_obj_create(parent);
    lv_obj_remove_style_all(view);
    lv_obj_set_size(view, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(view, 12, LV_PART_MAIN);
    lv_obj_clear_flag(view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(view, (void*)tag);
    return view;
}

static void add_legend_item(lv_obj_t* parent, lv_color_t color, const char* text) {
    lv_obj_t* item = lv_obj_create(parent);
    lv_obj_remove_style_all(item);
    lv_obj_set_size(item, LV_SIZE_CONTENT, 34);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(item, 8, LV_PART_MAIN);
    lv_obj_t* swatch = lv_obj_create(item);
    lv_obj_remove_style_all(swatch);
    lv_obj_set_size(swatch, 20, 20);
    lv_obj_set_style_bg_color(swatch, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(swatch, 4, LV_PART_MAIN);
    make_label(item, &montserrat_24_latin, UI_COLOR_TEXT_DIM, text);
}

static void build_main_view() {
    s.main_view = make_view(s.root, "energy_main_view");

    lv_obj_t* header = make_row(s.main_view, 52);
    s.range_label = make_label(header, UI_FONT_SMALL, UI_COLOR_TEXT_DIM);
    lv_obj_set_flex_grow(s.range_label, 1);
    lv_obj_set_style_text_align(s.range_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_user_data(s.range_label, (void*)"energy_range");
    char latest[48];
    snprintf(latest, sizeof(latest), "%s  " LV_SYMBOL_NEXT, i18n_get(STR_HISTORY_LATEST));
    s.latest_button = make_button(header, latest, "energy_latest", latest_cb);
    lv_obj_set_flex_grow(s.latest_button, 0);
    lv_obj_set_size(s.latest_button, LV_SIZE_CONTENT, 52);
    lv_obj_set_style_pad_hor(s.latest_button, 22, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s.latest_button, UI_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_obj_get_child(s.latest_button, 0), COLOR_ON_ACCENT,
                                LV_PART_MAIN);
    lv_obj_add_flag(s.latest_button, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* tiles = make_row(s.main_view, 150);
    static const char* const tile_tags[3] = {
        "energy_tile_today", "energy_tile_yesterday", "energy_tile_window"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t* tile = make_panel(tiles);
        lv_obj_set_height(tile, LV_PCT(100));
        lv_obj_set_flex_grow(tile, 1);
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_user_data(tile, (void*)tile_tags[i]);
        if (i < 2) {
            lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(tile, UI_COLOR_BTN_PRESSED, LV_STATE_PRESSED);
            lv_obj_add_event_cb(tile, tile_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        }
        char title[32];
        if (i == 0) {
            snprintf(title, sizeof(title), "%s", i18n_get(STR_EVENT_TODAY));
        } else if (i == 1) {
            snprintf(title, sizeof(title), "%s", i18n_get(STR_EVENT_YESTERDAY));
        } else {
            snprintf(title, sizeof(title), i18n_get(STR_ENERGY_DAYS_FMT), WINDOW_DAYS);
        }
        s.tile_title[i] = make_label(tile, UI_FONT_SMALL, UI_COLOR_TEXT_DIM, title);
        s.tile_value[i] = make_label(tile, &montserrat_32_latin, UI_COLOR_TEXT, "--");
        s.tile_cop[i] = make_label(tile, UI_FONT_SMALL, COLOR_COP, " ");
        for (lv_obj_t* l : {s.tile_title[i], s.tile_value[i], s.tile_cop[i]}) {
            lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    lv_obj_t* legend = lv_obj_create(s.main_view);
    lv_obj_remove_style_all(legend);
    lv_obj_set_size(legend, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(legend, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(legend, 24, LV_PART_MAIN);
    add_legend_item(legend, COLOR_HEATING, i18n_get(STR_HISTORY_LEGEND_HEATING));
    add_legend_item(legend, COLOR_COOLING, i18n_get(STR_HISTORY_LEGEND_COOLING));
    add_legend_item(legend, COLOR_HOT_WATER, i18n_get(STR_HISTORY_LEGEND_HOT_WATER));

    build_chart(s.main_view);

    lv_obj_t* actions = make_row(s.main_view, 68);
    char text[24];
    snprintf(text, sizeof(text), LV_SYMBOL_LEFT " %d %s", WINDOW_DAYS,
             i18n_get(STR_ENERGY_DAY_UNIT));
    s.prev_window_button = make_button(actions, text, "energy_prev_window", prev_window_cb);
    snprintf(text, sizeof(text), LV_SYMBOL_LEFT " 1 %s", i18n_get(STR_ENERGY_DAY_UNIT));
    s.prev_day_button = make_button(actions, text, "energy_prev_day", prev_day_cb);
    snprintf(text, sizeof(text), "1 %s " LV_SYMBOL_RIGHT, i18n_get(STR_ENERGY_DAY_UNIT));
    s.next_day_button = make_button(actions, text, "energy_next_day", next_day_cb);
    snprintf(text, sizeof(text), "%d %s " LV_SYMBOL_RIGHT, WINDOW_DAYS,
             i18n_get(STR_ENERGY_DAY_UNIT));
    s.next_window_button = make_button(actions, text, "energy_next_window", next_window_cb);
}

static void build_day_view() {
    s.day_view = make_view(s.root, "energy_day_view");
    lv_obj_add_flag(s.day_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* header = make_row(s.day_view, 64);
    char back[32];
    snprintf(back, sizeof(back), LV_SYMBOL_LEFT "  %s", i18n_get(STR_BACK));
    lv_obj_t* back_button = make_button(header, back, "energy_day_back", day_back_cb);
    lv_obj_set_flex_grow(back_button, 0);
    lv_obj_set_size(back_button, LV_SIZE_CONTENT, 60);
    lv_obj_set_style_pad_hor(back_button, 20, LV_PART_MAIN);
    s.day_title = make_label(header, UI_FONT_HEADER, UI_COLOR_TEXT);
    lv_obj_set_flex_grow(s.day_title, 1);
    lv_obj_set_user_data(s.day_title, (void*)"energy_day_title");

    const string_id_t names[4] = {STR_ENERGY_ELECTRICITY, STR_ENERGY_HEAT, STR_ENERGY_COP,
                                  STR_ENERGY_OUTDOOR};
    static const char* const tags[4] = {"energy_day_elec", "energy_day_heat",
                                        "energy_day_cop", "energy_day_outdoor"};
    for (int row_i = 0; row_i < 2; row_i++) {
        lv_obj_t* row = make_row(s.day_view, 150);
        for (int c = 0; c < 2; c++) {
            const int i = row_i * 2 + c;
            lv_obj_t* cell = make_panel(row);
            lv_obj_set_height(cell, LV_PCT(100));
            lv_obj_set_flex_grow(cell, 1);
            lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_START);
            lv_obj_t* name = make_label(cell, UI_FONT_SMALL, UI_COLOR_TEXT_DIM, i18n_get(names[i]));
            if (i == 3) s.outdoor_name = name;
            s.stat_value[i] = make_label(cell, &montserrat_32_latin,
                                         i == 2 ? COLOR_COP : UI_COLOR_TEXT, "--");
            lv_obj_set_user_data(s.stat_value[i], (void*)tags[i]);
            s.stat_sub[i] = make_label(cell, UI_FONT_SMALL, UI_COLOR_TEXT_DIM, " ");
        }
    }

    s.facts_label = make_label(s.day_view, UI_FONT_SMALL, UI_COLOR_TEXT_DIM);
    lv_obj_set_width(s.facts_label, LV_PCT(100));
    lv_obj_set_style_text_align(s.facts_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_user_data(s.facts_label, (void*)"energy_day_facts");

    s.runs_title = make_label(s.day_view, UI_FONT_SMALL, UI_COLOR_TEXT);
    lv_obj_set_user_data(s.runs_title, (void*)"energy_runs_title");

    s.runs_list = lv_obj_create(s.day_view);
    lv_obj_remove_style_all(s.runs_list);
    lv_obj_set_width(s.runs_list, LV_PCT(100));
    lv_obj_set_flex_grow(s.runs_list, 1);
    lv_obj_set_flex_flow(s.runs_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s.runs_list, 8, LV_PART_MAIN);
    lv_obj_set_scroll_dir(s.runs_list, LV_DIR_VER);
    lv_obj_set_user_data(s.runs_list, (void*)"energy_runs");

    s.runs_status = make_label(s.day_view, UI_FONT_SMALL, UI_COLOR_TEXT_DIM);
    lv_obj_set_width(s.runs_status, LV_PCT(100));
    lv_label_set_long_mode(s.runs_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s.runs_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_user_data(s.runs_status, (void*)"energy_runs_status");

    s.settings_label = make_label(s.day_view, &montserrat_16_latin, UI_COLOR_TEXT_DIM);
    lv_obj_set_width(s.settings_label, LV_PCT(100));
    lv_obj_set_style_text_align(s.settings_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_user_data(s.settings_label, (void*)"energy_day_settings");
}

lv_obj_t* energy_screen_create_in(lv_obj_t* parent) {
    if (s.shown || parent == nullptr) return nullptr;
    s.shown = true;

    s.root = lv_obj_create(parent);
    lv_obj_set_size(s.root, LV_PCT(100), LV_PCT(100));
    lv_obj_align(s.root, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s.root, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s.root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s.root, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s.root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s.root, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_top(s.root, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(s.root, NAV_BAR_H + 18, LV_PART_MAIN);
    lv_obj_clear_flag(s.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(s.root, (void*)"energy_screen");

    build_main_view();
    build_day_view();

    s.selected_date = 0;
    const uint32_t today = today_date();
    s.end_date = today ? today : 20000101;
    update_main_header();
    request_main();
    s.refresh_timer = lv_timer_create(refresh_timer_cb, REFRESH_MS, nullptr);
    ESP_LOGI(TAG, "energy screen shown");
    return s.root;
}

void energy_screen_hide(void) {
    if (!s.shown) return;
    s.shown = false;
    s.main_generation++;
    s.day_generation++;
    s.main_pending = false;
    s.day_pending = false;
    if (s.refresh_timer) lv_timer_delete(s.refresh_timer);
    s.refresh_timer = nullptr;
    if (s.root) lv_obj_delete(s.root);
    s.root = nullptr;
    s.main_view = s.day_view = nullptr;
    s.range_label = s.latest_button = s.chart = nullptr;
    s.prev_window_button = s.prev_day_button = nullptr;
    s.next_day_button = s.next_window_button = nullptr;
    for (int i = 0; i < 3; i++) s.tile_value[i] = s.tile_cop[i] = s.tile_title[i] = nullptr;
    for (int i = 0; i < 4; i++) s.stat_value[i] = s.stat_sub[i] = nullptr;
    s.outdoor_name = nullptr;
    s.day_title = s.facts_label = s.runs_title = s.runs_list = nullptr;
    s.runs_status = s.settings_label = nullptr;
    heap_caps_free(s.data);
    s.data = nullptr;
    heap_caps_free(s.runs);
    s.runs = nullptr;
}

bool energy_screen_is_shown(void) {
    return s.shown;
}
