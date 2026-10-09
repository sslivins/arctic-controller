/*
 * Arctic Heat Pump Controller
 * Home screen: what the heat pump is doing, the tank temperature, the key
 * readings and (while idle) the last 24 h of tank temperature.
 */

#include "heatpump_screen.h"
#include "app_preferences.h"
#include "chart_fault_zone.h"
#include "event_log_screen.h"
#include "fault_history.h"
#include "fonts/fonts.h"
#include "fonts/home_icons.h"
#include "heatpump_controller.h"
#include "heatpump_errors.h"
#include "heatpump_errors_screen.h"
#include "heatpump_history_screen.h"
#include "heatpump_types.h"
#include "history_storage.h"
#include "home_stats.h"
#include "i18n/i18n.h"
#include "macon_faults.h"
#include "nav_bar.h"
#include "tab_shell.h"
#include "time_manager.h"
#include "ui_common.h"
#include "ui_overlay.h"
#include <bsp/m5stack_tab5.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <initializer_list>
#include <limits.h>
#include <new>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char* TAG = "hp_screen";

// ============================================================================
// Colors
// ============================================================================
#define COLOR_CARD_BG       lv_color_hex(0x16213e)
#define COLOR_CARD_BORDER   lv_color_hex(0x24345a)
#define COLOR_PRESSED       lv_color_hex(0x1e2d4e)
#define COLOR_TEXT          lv_color_hex(0xe8eaf0)
#define COLOR_TEXT_DIM      lv_color_hex(0x9aa3b5)
#define COLOR_CAPTION       lv_color_hex(0x8d96a8)
#define COLOR_VALUE_DIM     lv_color_hex(0x6b7385)
#define COLOR_CHEVRON       lv_color_hex(0x4a5570)
#define COLOR_ACCENT        lv_color_hex(0x18c8f0)
#define COLOR_TRACK         lv_color_hex(0x2b3a5c)
#define COLOR_GRID          lv_color_hex(0x3d4f6f)
#define COLOR_WARNING       lv_color_hex(0xfbbf24)
#define COLOR_WARNING_EDGE  lv_color_hex(0xf59e0b)
#define COLOR_ERROR         lv_color_hex(0xef4444)
#define COLOR_ERROR_TEXT    lv_color_hex(0xff5a5a)
#define COLOR_FAULT_BG      lv_color_hex(0x2e1d2e)
#define COLOR_BANNER_BG     lv_color_hex(0x3a1d2b)
#define COLOR_STRIP_BG      lv_color_hex(0x141c33)
#define COLOR_STRIP_BORDER  lv_color_hex(0x22304f)
#define COLOR_SETPOINT      lv_color_hex(0x5d6478)
#define COLOR_FAULT_TIME    lv_color_hex(0xc79aa0)

// Hero tints: amber while heating, blue while cooling, icy teal in defrost.
#define COLOR_HEAT_BG       lv_color_hex(0x2a2117)
#define COLOR_HEAT_BORDER   lv_color_hex(0x8a5a1c)
#define COLOR_HEAT_TEXT     lv_color_hex(0xfbbf24)
#define COLOR_COOL_BG       lv_color_hex(0x0f2a55)
#define COLOR_COOL_BORDER   lv_color_hex(0x3b82f6)
#define COLOR_COOL_TEXT     lv_color_hex(0x7cb8ff)
#define COLOR_DEFROST_BG    lv_color_hex(0x152a36)
#define COLOR_DEFROST_BORDER lv_color_hex(0x4fb3c9)
#define COLOR_DEFROST_TEXT  lv_color_hex(0xa5f3fc)
#define COLOR_IND_OFF       lv_color_hex(0x4f5871)
#define COLOR_HEATER        lv_color_hex(0xfb923c)

static constexpr uint32_t CHART_WINDOW_S = 24 * 60 * 60;
static constexpr size_t CHART_BUCKETS = 150;      // 4 px each across the plot
static constexpr size_t CHART_MAX_RUNS = 160;
static constexpr size_t CHART_MAX_FAULTS = 16;
static constexpr size_t CHART_SAMPLE_CAP = 3600;  // 24 h at 30 s is 2880
static constexpr uint32_t CHART_REFRESH_MS = 2 * 60 * 1000;
static constexpr int32_t CHART_HEIGHT = 250;
static constexpr int32_t CHART_TOP_H = 40;        // fault markers above the plot
static constexpr int32_t CHART_AXIS_H = 30;       // time labels below the plot
static constexpr int16_t AT_SETPOINT_MARGIN_C = 2;
static constexpr uint16_t STARTS_WARN_PER_HOUR = 6;
static constexpr int16_t DT_WARN_C = 3;

#define DEGREE "\xC2\xB0"
#define WARNING_ICON "\xEF\x81\xB1"

// ============================================================================
// Error banner text
// ============================================================================

// First active error as "<icon> <code> · <description>", truncated to fit,
// with " + N more" when there are others.
static void format_error_card_text(char* buf, size_t buf_size, const lv_font_t* font, lv_coord_t max_width_px) {
    arctic::ActiveError errors[16];
    int count = arctic::getActiveErrors(errors, 16);
    if (count <= 0) {
        buf[0] = '\0';
        return;
    }

    char first[128];
    snprintf(first, sizeof(first), WARNING_ICON " %s \xC2\xB7 %s", errors[0].code,
             i18n_get_key(errors[0].name_msg_id, errors[0].description));

    if (count == 1) {
        lv_point_t size;
        lv_txt_get_size(&size, first, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x <= max_width_px) {
            strncpy(buf, first, buf_size - 1);
            buf[buf_size - 1] = '\0';
        } else {
            for (int len = (int)strlen(first) - 1; len > 10; len--) {
                first[len] = '\0';
                char trial[140];
                snprintf(trial, sizeof(trial), "%s...", first);
                lv_txt_get_size(&size, trial, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                if (size.x <= max_width_px) {
                    strncpy(buf, trial, buf_size - 1);
                    buf[buf_size - 1] = '\0';
                    return;
                }
            }
            strncpy(buf, first, buf_size - 1);
            buf[buf_size - 1] = '\0';
        }
        return;
    }

    char suffix[24];
    snprintf(suffix, sizeof(suffix), " + %d more", count - 1);

    lv_point_t suffix_size;
    lv_txt_get_size(&suffix_size, suffix, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_coord_t available = max_width_px - suffix_size.x;

    lv_point_t first_size;
    lv_txt_get_size(&first_size, first, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);

    if (first_size.x <= available) {
        snprintf(buf, buf_size, "%s%s", first, suffix);
    } else {
        for (int len = (int)strlen(first) - 1; len > 10; len--) {
            first[len] = '\0';
            char trial[140];
            snprintf(trial, sizeof(trial), "%s...", first);
            lv_point_t trial_size;
            lv_txt_get_size(&trial_size, trial, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            if (trial_size.x <= available) {
                snprintf(buf, buf_size, "%s%s", trial, suffix);
                return;
            }
        }
        snprintf(buf, buf_size, WARNING_ICON " %s%s", errors[0].code, suffix);
    }
}

// ============================================================================
// 24 h history (runs, tank trend, faults), loaded off the UI thread
// ============================================================================

struct HomeHistory {
    uint32_t start;
    uint32_t end;
    bool loaded;
    int16_t tank[CHART_BUCKETS];
    bool tank_ok[CHART_BUCKETS];
    home_run_t runs[CHART_MAX_RUNS];
    size_t run_count;
    fault_interval_t faults[CHART_MAX_FAULTS];
    size_t fault_count;
    home_run_summary_t summary;
};

struct HomeQuery {
    uint32_t generation;
    HomeHistory data;
};

// Outlive the widgets: a query can still be in flight when the screen is
// rebuilt, and its result must be dropped rather than applied.
static HomeHistory s_history;
static uint32_t s_generation = 0;
static bool s_query_running = false;
static bool s_query_pending = false;

// ============================================================================
// UI Elements
// ============================================================================

enum class HeroState {
    DISCONNECTED,
    FAULT,
    STANDBY,
    DEFROST,
    HEATING,
    COOLING,
    IDLE
};

struct Tile {
    lv_obj_t* box;
    lv_obj_t* value;
};

// A component pill under the hero: icon + name, lit while the part runs.
struct Indicator {
    lv_obj_t* box = nullptr;
    lv_obj_t* icon = nullptr;
    lv_obj_t* label = nullptr;
};

static struct {
    bool created = false;
    bool active = false;
    lv_obj_t* panel = nullptr;
    lv_obj_t* container = nullptr;

    lv_obj_t* device_name_label = nullptr;
    lv_obj_t* demo_banner = nullptr;

    lv_obj_t* error_card = nullptr;
    lv_obj_t* error_label = nullptr;
    lv_obj_t* error_chevron = nullptr;

    lv_obj_t* hero_card = nullptr;
    bool pulse_on = false;
    lv_obj_t* hero_state_label = nullptr;
    lv_obj_t* hero_tank_label = nullptr;
    lv_obj_t* hero_unit_label = nullptr;
    lv_obj_t* hero_sub_label = nullptr;
    lv_obj_t* hero_sub_value = nullptr;
    lv_obj_t* chart = nullptr;
    lv_obj_t* fault_caption = nullptr;
    lv_obj_t* fault_caption_text = nullptr;
    lv_obj_t* fault_caption_time = nullptr;
    lv_obj_t* fault_caption_more = nullptr;

    lv_obj_t* ind_row = nullptr;
    Indicator ind_comp;
    Indicator ind_fan;
    Indicator ind_pump;
    Indicator ind_heater;
    lv_obj_t* fan_bar[3] = {};

    Tile supply;
    Tile ret;
    Tile dt;
    Tile power;
    Tile cop;
    Tile hz;

    lv_obj_t* strip = nullptr;
    lv_obj_t* strip_label[3] = {};
    lv_obj_t* strip_value[3] = {};
    lv_obj_t* strip_group[3] = {};

    // Edges seen by this screen, for the parts history can't tell yet.
    bool have_prev = false;
    bool prev_running = false;
    bool prev_defrost = false;
    bool prev_fault = false;
    uint32_t run_since = 0;
    uint32_t run_stopped = 0;
    uint32_t defrost_since = 0;

    int16_t chart_setpoint_c = 0;
    bool chart_setpoint_valid = false;
    uint32_t last_query_ms = 0;
    bool queried_once = false;

    bool history_open = false;
    lv_obj_t* saved_screen = nullptr;
    lv_timer_t* update_timer = nullptr;
} state;

static void request_history(void);

static uint32_t now_s(void) {
    return (uint32_t)time(nullptr);
}

static void update_device_name_label(void)
{
    if (!state.created || !state.device_name_label) return;

    const char* name = app_prefs_get_device_name();
    if (!name || name[0] == '\0') {
        lv_obj_add_flag(state.device_name_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    char buf[APP_PREFS_DEVICE_NAME_MAX_BYTES + 8];
    snprintf(buf, sizeof(buf), LV_SYMBOL_HOME " %s", name);
    lv_label_set_text(state.device_name_label, buf);
    lv_obj_clear_flag(state.device_name_label, LV_OBJ_FLAG_HIDDEN);
}

// ============================================================================
// Formatting
// ============================================================================

static void format_temp(char* buf, size_t len, int16_t celsius) {
    snprintf(buf, len, "%d" DEGREE, app_prefs_convert_temp(celsius));
}

static void format_power(char* buf, size_t len, uint32_t watts) {
    if (watts >= 1000) {
        snprintf(buf, len, "%lu.%lu kW", (unsigned long)(watts / 1000),
                 (unsigned long)((watts % 1000) / 100));
    } else {
        snprintf(buf, len, "%lu W", (unsigned long)watts);
    }
}

static void format_duration(char* buf, size_t len, uint32_t seconds) {
    uint32_t minutes = seconds / 60;
    if (minutes < 100) {
        snprintf(buf, len, i18n_get(STR_HOME_MINUTES), (unsigned)minutes);
    } else {
        snprintf(buf, len, "%u h", (unsigned)((minutes + 30) / 60));
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

static const arctic::MaconFaultBit* fault_bit(const fault_interval_t& f) {
    return arctic::macon_fault_bit_for_site(
        static_cast<arctic::MaconFaultSiteId>(f.site));
}

// ============================================================================
// Hero state
// ============================================================================

static HeroState getHeroState(const arctic::HeatPumpState& hp) {
    if (!hp.connected) return HeroState::DISCONNECTED;
    switch (hp.operation) {
        case arctic::HeatPumpOperation::FAULT:   return HeroState::FAULT;
        case arctic::HeatPumpOperation::OFF:     return HeroState::STANDBY;
        case arctic::HeatPumpOperation::DEFROST: return HeroState::DEFROST;
        case arctic::HeatPumpOperation::HEATING: return HeroState::HEATING;
        case arctic::HeatPumpOperation::COOLING: return HeroState::COOLING;
        case arctic::HeatPumpOperation::IDLE:    return HeroState::IDLE;
        default:                                 break;
    }
    if (hp.hasAnyError()) return HeroState::FAULT;
    return hp.unit_on ? HeroState::IDLE : HeroState::STANDBY;
}

static bool is_running(HeroState s) {
    return s == HeroState::HEATING || s == HeroState::COOLING ||
           s == HeroState::DEFROST;
}

static const char* hero_state_text(HeroState s) {
    switch (s) {
        case HeroState::DISCONNECTED: return i18n_get(STR_HOME_DISCONNECTED);
        case HeroState::FAULT:        return i18n_get(STR_HOME_FAULT);
        case HeroState::STANDBY:      return i18n_get(STR_HOME_STANDBY);
        case HeroState::DEFROST:      return i18n_get(STR_HOME_DEFROSTING);
        case HeroState::COOLING:      return i18n_get(STR_HOME_COOLING);
        case HeroState::HEATING:      return i18n_get(STR_HOME_HEATING);
        case HeroState::IDLE:         return i18n_get(STR_HOME_IDLE);
    }
    return "---";
}

static lv_color_t hero_state_color(HeroState s) {
    switch (s) {
        case HeroState::DISCONNECTED:
        case HeroState::FAULT:   return COLOR_ERROR_TEXT;
        case HeroState::HEATING: return COLOR_HEAT_TEXT;
        case HeroState::COOLING: return COLOR_COOL_TEXT;
        case HeroState::DEFROST: return COLOR_DEFROST_TEXT;
        default:                 return COLOR_TEXT_DIM;
    }
}

static void hero_card_colors(HeroState s, lv_color_t* bg, lv_color_t* border) {
    switch (s) {
        case HeroState::DISCONNECTED:
        case HeroState::FAULT:
            *bg = COLOR_FAULT_BG;   *border = COLOR_ERROR;          return;
        case HeroState::HEATING:
            *bg = COLOR_HEAT_BG;    *border = COLOR_HEAT_BORDER;    return;
        case HeroState::COOLING:
            *bg = COLOR_COOL_BG;    *border = COLOR_COOL_BORDER;    return;
        case HeroState::DEFROST:
            *bg = COLOR_DEFROST_BG; *border = COLOR_DEFROST_BORDER; return;
        default:
            *bg = COLOR_CARD_BG;    *border = COLOR_CARD_BORDER;    return;
    }
}

// ============================================================================
// Navigation
// ============================================================================

static void on_errors_close(void) {
    if (state.saved_screen) {
        lv_screen_load_anim(state.saved_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
        state.saved_screen = nullptr;
    }
}

static void error_card_cb(lv_event_t*) {
    state.saved_screen = lv_scr_act();
    heatpump_errors_show(on_errors_close);
}

static void tile_cb(lv_event_t*) {
    tab_shell_select(NAV_TAB_STATUS);
}

static void history_closed(void) {
    state.history_open = false;
    if (state.active && state.update_timer) {
        lv_timer_resume(state.update_timer);
        heatpump_screen_update();
    }
}

static void strip_cb(lv_event_t*) {
    lv_obj_t* overlay = heatpump_history_show(state.panel, history_closed);
    if (overlay == nullptr) return;
    state.history_open = true;
    if (state.update_timer) lv_timer_pause(state.update_timer);
    // Bound to the overlay's lifetime, so the home content comes back however
    // the overlay is torn down.
    ui_overlay_cover(overlay, state.container);
}

static void fault_caption_cb(lv_event_t*) {
    event_log_screen_show_problems();
    tab_shell_select(NAV_TAB_EVENTS);
}

static void update_timer_cb(lv_timer_t*) {
    heatpump_screen_update();
}

// ============================================================================
// Chart
// ============================================================================

static void fill_rect(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2,
                      int32_t y2, lv_color_t color, lv_opa_t opa,
                      int32_t radius = 0) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = opa;
    dsc.radius = radius;
    dsc.border_width = 0;
    lv_area_t area = {x1, y1, x2, y2};
    lv_draw_rect(layer, &dsc, &area);
}

static lv_color_t run_color(uint8_t mode) {
    // Matches the cycle-history legend.
    switch (mode) {
        case HISTORY_TELEMETRY_MODE_COOLING: return lv_color_hex(0x3b82f6);
        case HISTORY_TELEMETRY_MODE_HOT_WATER: return lv_color_hex(0xfbbf24);
        default: return lv_color_hex(0xf97316);
    }
}

static void draw_line(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2,
                      int32_t y2, lv_color_t color, int32_t width,
                      int32_t dash = 0) {
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width;
    dsc.round_start = dash == 0;
    dsc.round_end = dash == 0;
    dsc.dash_width = dash;
    dsc.dash_gap = dash;
    dsc.p1.x = x1;
    dsc.p1.y = y1;
    dsc.p2.x = x2;
    dsc.p2.y = y2;
    lv_draw_line(layer, &dsc);
}

static void draw_text(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2,
                      int32_t y2, const char* text, lv_color_t color,
                      lv_text_align_t align, const lv_font_t* font) {
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.color = color;
    dsc.font = font;
    dsc.text = text;
    dsc.text_local = true;
    dsc.align = align;
    lv_area_t area = {x1, y1, x2, y2};
    lv_draw_label(layer, &dsc, &area);
}

static int32_t chart_x(const HomeHistory& h, uint32_t t, const lv_area_t& plot) {
    if (t <= h.start) return plot.x1;
    if (t >= h.end) return plot.x2;
    return plot.x1 + (int32_t)(((uint64_t)(t - h.start) *
                                (uint32_t)lv_area_get_width(&plot)) /
                               (h.end - h.start));
}

static int32_t chart_y(int32_t deci_c, int32_t lo, int32_t hi, const lv_area_t& plot) {
    if (deci_c < lo) deci_c = lo;
    if (deci_c > hi) deci_c = hi;
    return plot.y2 - (int32_t)(((int64_t)(deci_c - lo) *
                                (lv_area_get_height(&plot) - 1)) /
                               (hi - lo));
}

static void chart_draw_cb(lv_event_t* e) {
    lv_layer_t* layer = lv_event_get_layer(e);
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    const HomeHistory& h = s_history;
    if (layer == nullptr || obj == nullptr || !h.loaded || h.end <= h.start) return;

    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    lv_area_t plot = {area.x1, area.y1 + CHART_TOP_H, area.x2,
                      area.y2 - CHART_AXIS_H};

    // Same look as the cycle-history chart: opaque panel, grid, and flat
    // mode-coloured run bands at 20%.
    {
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = COLOR_CARD_BG;
        dsc.bg_opa = LV_OPA_COVER;
        dsc.radius = 12;
        dsc.border_color = COLOR_GRID;
        dsc.border_width = 1;
        lv_draw_rect(layer, &dsc, &plot);
    }
    lv_area_t inner = {plot.x1 + 1, plot.y1 + 1, plot.x2 - 1, plot.y2 - 1};

    for (size_t i = 0; i < h.run_count; i++) {
        const home_run_t& run = h.runs[i];
        int32_t x1 = chart_x(h, run.start, plot);
        int32_t x2 = chart_x(h, run.end != 0 ? run.end : h.end, plot);
        if (x2 - x1 < 3) x2 = x1 + 3;
        if (x1 < inner.x1) x1 = inner.x1;
        if (x2 > inner.x2) x2 = inner.x2;
        fill_rect(layer, x1, inner.y1, x2, inner.y2, run_color(run.mode),
                  LV_OPA_20);
    }

    for (size_t i = 0; i < h.fault_count; i++) {
        const fault_interval_t& f = h.faults[i];
        int32_t x1 = LV_MAX(chart_x(h, f.start, plot), inner.x1);
        int32_t x2 = LV_MIN(chart_x(h, f.end != 0 ? f.end : h.end, plot), inner.x2);
        chart_draw_fault_zone(layer, x1, x2, inner.y1, inner.y2, COLOR_ERROR);
    }

    for (int g = 1; g < 4; g++) {
        int32_t y = plot.y1 + (lv_area_get_height(&plot) * g) / 4;
        draw_line(layer, inner.x1, y, inner.x2, y, COLOR_GRID, 1);
        int32_t x = plot.x1 + (lv_area_get_width(&plot) * g) / 4;
        draw_line(layer, x, inner.y1, x, inner.y2, COLOR_GRID, 1);
    }

    // Value range: the tank readings and the setpoint, with a little room.
    int32_t lo = INT32_MAX;
    int32_t hi = INT32_MIN;
    for (size_t i = 0; i < CHART_BUCKETS; i++) {
        if (!h.tank_ok[i]) continue;
        if (h.tank[i] < lo) lo = h.tank[i];
        if (h.tank[i] > hi) hi = h.tank[i];
    }
    bool sp_valid = state.chart_setpoint_valid;
    int32_t sp_deci = (int32_t)state.chart_setpoint_c * 10;
    if (!sp_valid && h.summary.setpoint_valid) {
        sp_valid = true;
        sp_deci = h.summary.setpoint_deci_c;
    }
    if (sp_valid) {
        if (sp_deci < lo) lo = sp_deci;
        if (sp_deci > hi) hi = sp_deci;
    }
    if (lo > hi) {
        lo = 400;
        hi = 500;
    }
    lo -= 20;
    hi += 30;
    if (hi - lo < 100) {
        int32_t mid = (lo + hi) / 2;
        lo = mid - 50;
        hi = mid + 50;
    }

    if (sp_valid) {
        int32_t y = chart_y(sp_deci, lo, hi, plot);
        draw_line(layer, plot.x1, y, plot.x2, y, COLOR_SETPOINT, 2, 8);
        char sp_text[16];
        format_temp(sp_text, sizeof(sp_text), (int16_t)(sp_deci / 10));
        draw_text(layer, plot.x2 - 120, y - 26, plot.x2, y - 4, sp_text,
                  COLOR_VALUE_DIM, LV_TEXT_ALIGN_RIGHT, &montserrat_20_latin);
    }

    // Tank trend, broken where there is no reading.
    int32_t width = lv_area_get_width(&plot);
    bool have_prev = false;
    int32_t px = 0;
    int32_t py = 0;
    for (size_t i = 0; i < CHART_BUCKETS; i++) {
        if (!h.tank_ok[i]) {
            have_prev = false;
            continue;
        }
        int32_t x = plot.x1 + (int32_t)(((2 * i + 1) * (size_t)width) / (2 * CHART_BUCKETS));
        int32_t y = chart_y(h.tank[i], lo, hi, plot);
        if (have_prev) draw_line(layer, px, py, x, y, COLOR_TEXT, 3);
        px = x;
        py = y;
        have_prev = true;
    }

    // Faults: hatched zones in the plot, labelled above it where there's room.
    int32_t label_right = INT32_MIN;
    for (size_t i = 0; i < h.fault_count; i++) {
        const fault_interval_t& f = h.faults[i];
        int32_t x1 = chart_x(h, f.start, plot);
        if (x1 < label_right + 8) continue;
        const arctic::MaconFaultBit* bit = fault_bit(f);
        char text[24];
        snprintf(text, sizeof(text), WARNING_ICON " %s", bit ? bit->code : "?");
        lv_point_t size;
        lv_txt_get_size(&size, text, &montserrat_20_latin, 0, 0, LV_COORD_MAX,
                        LV_TEXT_FLAG_NONE);
        int32_t lx = x1;
        if (lx + size.x > plot.x2) lx = plot.x2 - size.x;
        // Pulling a label in from the right edge can land it on the previous one.
        if (lx < label_right + 8) continue;
        draw_text(layer, lx, area.y1, lx + size.x, plot.y1 - 6, text,
                  COLOR_ERROR_TEXT, LV_TEXT_ALIGN_LEFT, &montserrat_20_latin);
        label_right = lx + size.x;
    }

    int32_t ay1 = plot.y2 + 6;
    int32_t ay2 = area.y2;
    draw_text(layer, plot.x1, ay1, plot.x1 + 120, ay2, "24 h", COLOR_VALUE_DIM,
              LV_TEXT_ALIGN_LEFT, &montserrat_20_latin);
    int32_t mid = (plot.x1 + plot.x2) / 2;
    draw_text(layer, mid - 60, ay1, mid + 60, ay2, "12 h", COLOR_VALUE_DIM,
              LV_TEXT_ALIGN_CENTER, &montserrat_20_latin);
    draw_text(layer, plot.x2 - 120, ay1, plot.x2, ay2, i18n_get(STR_HOME_NOW),
              COLOR_VALUE_DIM, LV_TEXT_ALIGN_RIGHT, &montserrat_20_latin);
}

static bool history_has_data(const HomeHistory& h) {
    if (!h.loaded) return false;
    if (h.run_count > 0 || h.fault_count > 0) return true;
    for (size_t i = 0; i < CHART_BUCKETS; i++) {
        if (h.tank_ok[i]) return true;
    }
    return false;
}

// Most recent fault in the window, with how many others there were.
static void update_fault_caption(void) {
    const HomeHistory& h = s_history;
    if (h.fault_count == 0) {
        lv_obj_add_flag(state.fault_caption, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const fault_interval_t& f = h.faults[h.fault_count - 1];
    const arctic::MaconFaultBit* bit = fault_bit(f);

    char text[160];
    if (bit != nullptr) {
        snprintf(text, sizeof(text), WARNING_ICON " %s %s", bit->code,
                 i18n_get_key(bit->label_msg_id, bit->label));
    } else {
        snprintf(text, sizeof(text), WARNING_ICON " ?");
    }
    lv_label_set_text(state.fault_caption_text, text);

    uint32_t end = f.end != 0 ? f.end : now_s();
    char clock[16];
    char dur[24];
    format_clock(clock, sizeof(clock), f.start);
    format_duration(dur, sizeof(dur), end > f.start ? end - f.start : 0);
    char right[64];
    snprintf(right, sizeof(right), "%s \xC2\xB7 %s", clock, dur);
    lv_label_set_text(state.fault_caption_time, right);
    if (h.fault_count > 1) {
        char more[32];
        snprintf(more, sizeof(more), i18n_get(STR_HOME_MORE_FAULTS),
                 (unsigned)(h.fault_count - 1));
        lv_label_set_text(state.fault_caption_more, more);
        lv_obj_clear_flag(state.fault_caption_more, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(state.fault_caption_more, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(state.fault_caption, LV_OBJ_FLAG_HIDDEN);
}

static void home_query_done(void* arg) {
    auto* q = static_cast<HomeQuery*>(arg);
    s_query_running = false;
    if (q->generation == s_generation && state.created) {
        s_history = q->data;
        if (state.chart) lv_obj_invalidate(state.chart);
        heatpump_screen_update();
    }
    delete q;
    if (s_query_pending && state.created) {
        s_query_pending = false;
        request_history();
    }
}

static void home_query_task(void* arg) {
    auto* q = static_cast<HomeQuery*>(arg);
    HomeHistory& h = q->data;
    auto* samples = static_cast<history_telemetry_sample_t*>(heap_caps_malloc(
        sizeof(history_telemetry_sample_t) * CHART_SAMPLE_CAP,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    size_t count = 0;
    if (samples != nullptr &&
        history_storage_query_telemetry(h.start, h.end, samples,
                                        CHART_SAMPLE_CAP, &count) == ESP_OK) {
        h.run_count = home_stats_runs(samples, count, h.end, h.runs,
                                      CHART_MAX_RUNS, &h.summary);
        home_stats_tank_series(samples, count, h.start, h.end, h.tank,
                               h.tank_ok, CHART_BUCKETS);
        h.loaded = true;
    }
    heap_caps_free(samples);
    h.fault_count = fault_history_query(h.start, h.end, h.faults, CHART_MAX_FAULTS);

    // Worker task: lv_async_call() must hold the LVGL lock.
    bsp_display_lock(0);
    lv_async_call(home_query_done, q);
    bsp_display_unlock();
    vTaskDelete(nullptr);
}

static void request_history(void) {
    if (!state.created || !time_mgr_is_synced()) return;
    if (s_query_running) {
        s_query_pending = true;
        return;
    }
    auto* q = new (std::nothrow) HomeQuery();
    if (q == nullptr) return;
    q->generation = s_generation;
    q->data.end = now_s() + 1;
    q->data.start = q->data.end - CHART_WINDOW_S;
    state.last_query_ms = lv_tick_get();
    state.queried_once = true;
    s_query_running = true;
    if (xTaskCreate(home_query_task, "home_query", 6144, q, 3, nullptr) != pdPASS) {
        ESP_LOGW(TAG, "home history query task failed to start");
        s_query_running = false;
        delete q;
    }
}

// ============================================================================
// Widgets
// ============================================================================

static lv_obj_t* create_card(lv_obj_t* parent, lv_color_t bg, lv_color_t border,
                             int32_t radius) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_style_bg_color(card, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, border, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(card, radius, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static void make_pressable(lv_obj_t* obj, lv_event_cb_t cb) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(obj, COLOR_PRESSED, LV_STATE_PRESSED);
    lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, nullptr);
}

static lv_obj_t* create_label(lv_obj_t* parent, const lv_font_t* font,
                              lv_color_t color, const char* text = "") {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    return label;
}

static lv_obj_t* create_row(lv_obj_t* parent) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    return row;
}

static Tile create_tile(lv_obj_t* parent, const char* caption, const char* tag) {
    Tile tile;
    tile.box = create_card(parent, COLOR_CARD_BG, COLOR_CARD_BORDER, 12);
    lv_obj_set_size(tile.box, 225, 136);
    lv_obj_set_style_pad_all(tile.box, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(tile.box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile.box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(tile.box, 4, LV_PART_MAIN);
    make_pressable(tile.box, tile_cb);

    tile.value = create_label(tile.box, UI_FONT_TITLE, COLOR_TEXT, "--");
    lv_obj_set_user_data(tile.value, (void*)tag);
    create_label(tile.box, &montserrat_20_latin, COLOR_CAPTION, caption);

    lv_obj_t* chevron = create_label(tile.box, &montserrat_16_latin, COLOR_CHEVRON,
                                     LV_SYMBOL_RIGHT);
    lv_obj_add_flag(chevron, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(chevron, LV_ALIGN_TOP_RIGHT, -4, 4);
    return tile;
}

static void set_tile(const Tile& tile, const char* text, lv_color_t value_color,
                     bool warn) {
    lv_label_set_text(tile.value, text);
    lv_obj_set_style_text_color(tile.value, warn ? COLOR_WARNING : value_color,
                                LV_PART_MAIN);
    lv_obj_set_style_border_color(tile.box, warn ? COLOR_WARNING_EDGE : COLOR_CARD_BORDER,
                                  LV_PART_MAIN);
}

static void pulse_opa_cb(void* obj, int32_t v) {
    lv_obj_set_style_text_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(v),
                              LV_PART_MAIN);
}

// Slow "breathing" fade on the state label while the compressor runs; a
// spinner turned out to be too distracting over hours-long runs.
static void set_state_pulse(lv_obj_t* label, bool on) {
    lv_anim_delete(label, pulse_opa_cb);
    lv_obj_set_style_text_opa(label, LV_OPA_COVER, LV_PART_MAIN);
    if (!on) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, label);
    lv_anim_set_exec_cb(&a, pulse_opa_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_50);
    lv_anim_set_duration(&a, 1500);
    lv_anim_set_reverse_duration(&a, 1500);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

static Indicator create_indicator(lv_obj_t* parent, const lv_image_dsc_t* icon,
                                  const char* text, const char* tag) {
    Indicator ind;
    ind.box = lv_obj_create(parent);
    lv_obj_remove_style_all(ind.box);
    lv_obj_set_size(ind.box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(ind.box, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(ind.box, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(ind.box, COLOR_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ind.box, COLOR_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ind.box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(ind.box, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(ind.box, 9, LV_PART_MAIN);
    lv_obj_set_style_pad_column(ind.box, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(ind.box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ind.box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(ind.box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(ind.box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(ind.box, (void*)tag);

    ind.icon = lv_image_create(ind.box);
    lv_image_set_src(ind.icon, icon);
    lv_obj_set_style_image_recolor(ind.icon, COLOR_IND_OFF, LV_PART_MAIN);
    lv_obj_set_style_image_recolor_opa(ind.icon, LV_OPA_COVER, LV_PART_MAIN);

    ind.label = create_label(ind.box, &montserrat_24_latin, COLOR_IND_OFF, text);
    return ind;
}

// Lit: accent border/icon, white text and a faint accent fill (bg_color is the
// accent, which device tests read). Off: dim, bg_color = COLOR_TRACK.
static void set_indicator(const Indicator& ind, bool on, lv_color_t accent) {
    lv_obj_set_style_border_color(ind.box, on ? accent : COLOR_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ind.box, on ? accent : COLOR_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ind.box, on ? LV_OPA_10 : LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_image_recolor(ind.icon, on ? accent : COLOR_IND_OFF, LV_PART_MAIN);
    lv_obj_set_style_text_color(ind.label, on ? COLOR_TEXT : COLOR_IND_OFF, LV_PART_MAIN);
}

static void set_visible(lv_obj_t* obj, bool visible) {
    if (visible) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_strip(int i, const char* label, const char* value, lv_color_t color) {
    if (label == nullptr) {
        lv_obj_add_flag(state.strip_group[i], LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(state.strip_label[i], label);
    lv_label_set_text(state.strip_value[i], value);
    lv_obj_set_style_text_color(state.strip_value[i], color, LV_PART_MAIN);
    lv_obj_clear_flag(state.strip_group[i], LV_OBJ_FLAG_HIDDEN);
}

// ============================================================================
// Public Functions
// ============================================================================

void heatpump_screen_create(lv_obj_t* parent, int y_offset) {
    if (state.created) {
        ESP_LOGW(TAG, "Screen already created");
        return;
    }

    ESP_LOGI(TAG, "Creating home screen");
    state.panel = parent;

    // Fills the tab panel; the persistent nav bar (drawn by the tab shell)
    // overlays the bottom NAV_BAR_H.
    state.container = lv_obj_create(parent);
    lv_obj_set_size(state.container, 700, LV_PCT(100));
    lv_obj_align(state.container, LV_ALIGN_TOP_MID, 0, y_offset);
    lv_obj_set_style_bg_opa(state.container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(state.container, NAV_BAR_H + 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(state.container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(state.container, 12, LV_PART_MAIN);
    lv_obj_set_flex_align(state.container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(state.container, LV_SCROLLBAR_MODE_AUTO);

    // Friendly controller name (hidden when unset)
    state.device_name_label = create_label(state.container, &montserrat_32_latin, COLOR_TEXT);
    lv_obj_set_size(state.device_name_label, LV_PCT(100), 40);
    lv_obj_set_style_text_align(state.device_name_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(state.device_name_label, LV_LABEL_LONG_DOT);
    lv_obj_set_user_data(state.device_name_label, (void*)"home_device_name");

    // Demo mode banner
    state.demo_banner = lv_obj_create(state.container);
    lv_obj_set_size(state.demo_banner, LV_PCT(100), 40);
    lv_obj_set_style_bg_color(state.demo_banner, COLOR_WARNING, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state.demo_banner, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.demo_banner, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(state.demo_banner, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.demo_banner, 0, LV_PART_MAIN);
    lv_obj_clear_flag(state.demo_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(state.demo_banner, (void*)"demo_banner");
    lv_obj_t* demo_label = create_label(state.demo_banner, UI_FONT_BODY,
                                        lv_color_hex(0x000000),
                                        i18n_get(STR_HP_DEMO_MODE_ENABLED));
    lv_obj_center(demo_label);
    set_visible(state.demo_banner, app_prefs_is_demo_mode());

    // Fault banner (only while a fault is active or the unit is unreachable)
    state.error_card = create_card(state.container, COLOR_BANNER_BG, COLOR_ERROR, 12);
    lv_obj_set_size(state.error_card, LV_PCT(100), 76);
    lv_obj_set_style_pad_hor(state.error_card, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(state.error_card, 0, LV_PART_MAIN);
    lv_obj_set_user_data(state.error_card, (void*)"home_fault_banner");
    make_pressable(state.error_card, error_card_cb);
    lv_obj_set_style_bg_color(state.error_card, lv_color_hex(0x4a2436), LV_STATE_PRESSED);
    state.error_label = create_label(state.error_card, UI_FONT_BODY, COLOR_ERROR_TEXT);
    lv_obj_set_width(state.error_label, 620);
    lv_label_set_long_mode(state.error_label, LV_LABEL_LONG_CLIP);
    lv_obj_align(state.error_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_user_data(state.error_label, (void*)"error_label");
    state.error_chevron = create_label(state.error_card, UI_FONT_BODY, COLOR_ERROR_TEXT,
                                       LV_SYMBOL_RIGHT);
    lv_obj_align(state.error_chevron, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_flag(state.error_card, LV_OBJ_FLAG_HIDDEN);

    // Hero: state, tank temperature, why, and (idle) the 24 h trend
    state.hero_card = create_card(state.container, COLOR_CARD_BG, COLOR_CARD_BORDER, 14);
    lv_obj_set_width(state.hero_card, LV_PCT(100));
    lv_obj_set_flex_grow(state.hero_card, 1);
    lv_obj_set_style_min_height(state.hero_card, 520, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(state.hero_card, 28, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(state.hero_card, 20, LV_PART_MAIN);
    lv_obj_set_flex_flow(state.hero_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(state.hero_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(state.hero_card, 10, LV_PART_MAIN);
    lv_obj_set_user_data(state.hero_card, (void*)"hero_card");

    lv_obj_t* state_row = create_row(state.hero_card);
    lv_obj_set_flex_align(state_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    state.hero_state_label = create_label(state_row, UI_FONT_TITLE, COLOR_TEXT_DIM, "---");
    lv_obj_set_user_data(state.hero_state_label, (void*)"hero_state");

    lv_obj_t* number_row = create_row(state.hero_card);
    lv_obj_set_flex_align(number_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(number_row, 8, LV_PART_MAIN);
    // The 220 px digit glyphs overhang their line box; keep clear of the state label.
    lv_obj_set_style_margin_top(number_row, 26, LV_PART_MAIN);
    state.hero_tank_label = create_label(number_row, &montserrat_220_digits, COLOR_TEXT, "--");
    lv_obj_set_user_data(state.hero_tank_label, (void*)"hero_tank_temp");
    state.hero_unit_label = create_label(number_row, &montserrat_80_unit, COLOR_TEXT_DIM,
                                         app_prefs_temp_unit_str());
    lv_obj_set_style_pad_top(state.hero_unit_label, 10, LV_PART_MAIN);
    lv_obj_set_user_data(state.hero_unit_label, (void*)"hero_tank_unit");

    lv_obj_t* sub_row = create_row(state.hero_card);
    lv_obj_set_flex_align(sub_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(sub_row, 10, LV_PART_MAIN);
    lv_obj_set_style_margin_top(sub_row, 6, LV_PART_MAIN);
    state.hero_sub_label = create_label(sub_row, UI_FONT_BODY, COLOR_TEXT_DIM,
                                        i18n_get(STR_HOME_TANK));
    lv_obj_set_user_data(state.hero_sub_label, (void*)"hero_sub");
    state.hero_sub_value = create_label(sub_row, UI_FONT_BODY, COLOR_TEXT);
    lv_obj_set_user_data(state.hero_sub_value, (void*)"hero_sub_value");

    state.chart = lv_obj_create(state.hero_card);
    lv_obj_remove_style_all(state.chart);
    lv_obj_set_size(state.chart, LV_PCT(100), CHART_HEIGHT);
    lv_obj_set_style_margin_top(state.chart, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(state.chart, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(state.chart, COLOR_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(state.chart, LV_OPA_60, LV_STATE_PRESSED);
    // Tapping the chart opens the full cycle history.
    lv_obj_add_flag(state.chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(state.chart, strip_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_clear_flag(state.chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(state.chart, chart_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_set_user_data(state.chart, (void*)"home_chart");
    lv_obj_add_flag(state.chart, LV_OBJ_FLAG_HIDDEN);

    state.fault_caption = create_card(state.hero_card, COLOR_STRIP_BG, COLOR_TRACK, 10);
    lv_obj_set_style_border_width(state.fault_caption, 1, LV_PART_MAIN);
    lv_obj_set_size(state.fault_caption, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(state.fault_caption, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(state.fault_caption, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(state.fault_caption, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(state.fault_caption, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(state.fault_caption, 12, LV_PART_MAIN);
    lv_obj_set_user_data(state.fault_caption, (void*)"home_fault_caption");
    make_pressable(state.fault_caption, fault_caption_cb);
    state.fault_caption_text = create_label(state.fault_caption, &montserrat_24_latin, COLOR_TEXT);
    lv_obj_set_flex_grow(state.fault_caption_text, 1);
    lv_obj_set_height(state.fault_caption_text, lv_font_get_line_height(&montserrat_24_latin));
    lv_label_set_long_mode(state.fault_caption_text, LV_LABEL_LONG_DOT);
    state.fault_caption_time = create_label(state.fault_caption, &montserrat_24_latin,
                                            COLOR_FAULT_TIME);
    // "+N more" badge: red so the extra faults aren't missed.
    state.fault_caption_more = create_label(state.fault_caption, &montserrat_20_latin,
                                            lv_color_white());
    lv_obj_set_style_bg_color(state.fault_caption_more, COLOR_ERROR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state.fault_caption_more, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(state.fault_caption_more, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(state.fault_caption_more, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(state.fault_caption_more, 3, LV_PART_MAIN);
    lv_obj_set_user_data(state.fault_caption_more, (void*)"home_fault_more");
    lv_obj_add_flag(state.fault_caption_more, LV_OBJ_FLAG_HIDDEN);
    create_label(state.fault_caption, &montserrat_24_latin, COLOR_FAULT_TIME,
                 LV_SYMBOL_RIGHT);
    lv_obj_add_flag(state.fault_caption, LV_OBJ_FLAG_HIDDEN);

    // Component pills: compressor, fan (with speed bars), pump, backup heater.
    state.ind_row = create_row(state.hero_card);
    lv_obj_set_flex_align(state.ind_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(state.ind_row, 14, LV_PART_MAIN);
    lv_obj_set_style_margin_top(state.ind_row, 16, LV_PART_MAIN);
    lv_obj_set_user_data(state.ind_row, (void*)"home_indicators");
    state.ind_comp = create_indicator(state.ind_row, &home_icon_compressor,
                                      i18n_get(STR_HP_COMPRESSOR), "home_ind_compressor");
    state.ind_fan = create_indicator(state.ind_row, &home_icon_fan, i18n_get(STR_HP_FAN),
                                     "home_ind_fan");
    lv_obj_t* bars = create_row(state.ind_fan.box);
    lv_obj_set_flex_align(bars, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(bars, 3, LV_PART_MAIN);
    static const char* bar_tags[] = {"home_ind_fan_bar_1", "home_ind_fan_bar_2",
                                     "home_ind_fan_bar_3"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t* bar = lv_obj_create(bars);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 6, 8 + i * 7);
        lv_obj_set_style_radius(bar, 2, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, COLOR_TRACK, LV_PART_MAIN);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(bar, (void*)bar_tags[i]);
        state.fan_bar[i] = bar;
    }
    state.ind_pump = create_indicator(state.ind_row, &home_icon_pump, i18n_get(STR_HP_PUMP),
                                      "home_ind_pump");
    state.ind_heater = create_indicator(state.ind_row, &home_icon_heater,
                                        i18n_get(STR_HP_AUX_HEAT), "home_ind_heater");
    lv_obj_add_flag(state.ind_heater.box, LV_OBJ_FLAG_HIDDEN);

    // Tiles: two rows of three while running, one row of three otherwise.
    lv_obj_t* tiles = lv_obj_create(state.container);
    lv_obj_remove_style_all(tiles);
    lv_obj_set_size(tiles, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tiles, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(tiles, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(tiles, 12, LV_PART_MAIN);
    lv_obj_clear_flag(tiles, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tiles, LV_OBJ_FLAG_CLICKABLE);
    state.supply = create_tile(tiles, i18n_get(STR_HOME_SUPPLY), "home_supply");
    state.ret = create_tile(tiles, i18n_get(STR_HOME_RETURN), "home_return");
    state.dt = create_tile(tiles, "\xCE\x94T", "home_dt");
    state.power = create_tile(tiles, i18n_get(STR_HP_LABEL_POWER), "perf_power");
    state.cop = create_tile(tiles, i18n_get(STR_HP_LABEL_COP), "perf_cop");
    state.hz = create_tile(tiles, i18n_get(STR_HOME_COMPRESSOR), "home_hz");

    // Strip: run stats while running, last run and today's energy otherwise.
    state.strip = create_card(state.container, COLOR_STRIP_BG, COLOR_STRIP_BORDER, 12);
    lv_obj_set_size(state.strip, LV_PCT(100), 72);
    lv_obj_set_style_pad_hor(state.strip, 22, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(state.strip, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(state.strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(state.strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(state.strip, 16, LV_PART_MAIN);
    lv_obj_set_user_data(state.strip, (void*)"home_strip");
    make_pressable(state.strip, strip_cb);
    lv_obj_t* groups = create_row(state.strip);
    lv_obj_set_flex_grow(groups, 1);
    lv_obj_set_flex_align(groups, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    static const char* const kStripTags[3] = {"home_strip_0", "home_strip_1", "home_strip_2"};
    for (int i = 0; i < 3; i++) {
        state.strip_group[i] = create_row(groups);
        lv_obj_set_flex_align(state.strip_group[i], LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(state.strip_group[i], 8, LV_PART_MAIN);
        state.strip_label[i] = create_label(state.strip_group[i], &montserrat_24_latin,
                                            COLOR_TEXT_DIM);
        state.strip_value[i] = create_label(state.strip_group[i], &montserrat_24_latin,
                                            COLOR_TEXT, "--");
        lv_obj_set_user_data(state.strip_value[i], (void*)kStripTags[i]);
    }
    create_label(state.strip, &montserrat_24_latin, COLOR_TEXT_DIM, LV_SYMBOL_RIGHT);

    state.created = true;
    update_device_name_label();

    state.update_timer = lv_timer_create(update_timer_cb, 1000, nullptr);
    heatpump_screen_update();
}

void heatpump_screen_set_active(bool active) {
    state.active = active;
    if (!active && state.history_open && heatpump_history_is_shown()) {
        heatpump_history_hide();
    }
    if (state.update_timer) {
        if (active && !state.history_open) {
            lv_timer_resume(state.update_timer);
        } else {
            lv_timer_pause(state.update_timer);
        }
    }
    if (active) {
        if (!state.queried_once || lv_tick_elaps(state.last_query_ms) > 30 * 1000) {
            request_history();
        }
        heatpump_screen_update();
    }
}

void heatpump_screen_update(void) {
    if (!state.created) {
        return;
    }

    arctic::HeatPumpState hp = arctic::getState();
    arctic::TelemetrySnapshot snap = arctic::getTelemetrySnapshot();
    HeroState hero = getHeroState(hp);
    bool running = is_running(hero);
    bool defrost = hero == HeroState::DEFROST;
    bool fault = hp.connected && hp.hasAnyError();
    uint32_t now = now_s();
    bool synced = time_mgr_is_synced();

    // Edges: refresh the history right away when a run starts or stops or a
    // fault comes or goes, so the strip and chart don't lag by minutes.
    bool refresh = false;
    if (state.have_prev) {
        if (running != state.prev_running) {
            if (running) {
                state.run_since = synced ? now : 0;
            } else {
                state.run_stopped = synced ? now : 0;
            }
            refresh = true;
        }
        if (defrost && !state.prev_defrost) state.defrost_since = now;
        if (fault != state.prev_fault) refresh = true;
    }
    if (defrost && state.defrost_since == 0) state.defrost_since = now;
    if (!defrost) state.defrost_since = 0;
    state.have_prev = true;
    state.prev_running = running;
    state.prev_defrost = defrost;
    state.prev_fault = fault;
    if (state.active &&
        (refresh || !state.queried_once ||
         lv_tick_elaps(state.last_query_ms) >= CHART_REFRESH_MS)) {
        request_history();
    }

    state.chart_setpoint_valid = snap.setpoint_valid;
    state.chart_setpoint_c = snap.active_setpoint_c;

    // Fault banner
    if (!hp.connected) {
        char buf[96];
        snprintf(buf, sizeof(buf), WARNING_ICON " %s", i18n_get(STR_HP_NOT_CONNECTED));
        lv_label_set_text(state.error_label, buf);
        lv_obj_clear_flag(state.error_card, LV_OBJ_FLAG_HIDDEN);
    } else if (fault) {
        char buf[256];
        format_error_card_text(buf, sizeof(buf), UI_FONT_BODY, 620);
        lv_label_set_text(state.error_label, buf);
        lv_obj_clear_flag(state.error_card, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(state.error_card, LV_OBJ_FLAG_HIDDEN);
    }

    // Hero
    lv_color_t hero_bg, hero_border;
    hero_card_colors(hero, &hero_bg, &hero_border);
    lv_obj_set_style_bg_color(state.hero_card, hero_bg, LV_PART_MAIN);
    lv_obj_set_style_border_color(state.hero_card, hero_border, LV_PART_MAIN);
    // State line carries the "why": "Fault · P02", "Idle · at setpoint".
    const char* state_detail = nullptr;
    char fault_code_buf[16];
    if (hero == HeroState::FAULT) {
        arctic::ActiveError errors[1];
        if (arctic::getActiveErrors(errors, 1) > 0) {
            snprintf(fault_code_buf, sizeof(fault_code_buf), "%s", errors[0].code);
            state_detail = fault_code_buf;
        }
    } else if (hero == HeroState::IDLE && snap.setpoint_valid) {
        int16_t tank = hp.water_tank_temp;
        int16_t sp = snap.active_setpoint_c;
        bool cooling = hp.working_mode == arctic::WorkingMode::COOLING;
        bool satisfied = cooling ? tank <= sp + AT_SETPOINT_MARGIN_C
                                 : tank >= sp - AT_SETPOINT_MARGIN_C;
        state_detail = i18n_get(satisfied ? STR_HOME_AT_SETPOINT : STR_HOME_NO_DEMAND);
    }
    if (state_detail) {
        char state_buf[96];
        snprintf(state_buf, sizeof(state_buf), "%s \xC2\xB7 %s", hero_state_text(hero),
                 state_detail);
        lv_label_set_text(state.hero_state_label, state_buf);
    } else {
        lv_label_set_text(state.hero_state_label, hero_state_text(hero));
    }
    lv_obj_set_style_text_color(state.hero_state_label, hero_state_color(hero), LV_PART_MAIN);
    if (running != state.pulse_on) {
        set_state_pulse(state.hero_state_label, running);
        state.pulse_on = running;
    }

    // Component pills; unknown while disconnected, so hide them then.
    set_visible(state.ind_row, hp.connected);
    if (hp.connected) {
        lv_color_t accent = is_running(hero) ? hero_state_color(hero) : COLOR_TEXT;
        int fan_level = hp.getFanSpeedLevel();
        bool fan_on = hp.isFanRunning();
        bool heater_on = hp.isBackupHeaterOn();
        set_indicator(state.ind_comp, hp.isCompressorRunning(), accent);
        set_indicator(state.ind_fan, fan_on, accent);
        set_indicator(state.ind_pump, hp.isWaterPumpRunning(), accent);
        set_indicator(state.ind_heater, heater_on, COLOR_HEATER);
        for (int i = 0; i < 3; i++) {
            lv_obj_set_style_bg_color(state.fan_bar[i],
                                      fan_on && i < fan_level ? accent : COLOR_TRACK,
                                      LV_PART_MAIN);
        }
        // Four pills don't fit with names; drop to icons while the heater shows.
        set_visible(state.ind_heater.box, heater_on);
        for (const Indicator* ind : {&state.ind_comp, &state.ind_fan, &state.ind_pump}) {
            set_visible(ind->label, !heater_on);
        }
    }

    char buf[48];
    if (hp.connected) {
        snprintf(buf, sizeof(buf), "%d", app_prefs_convert_temp(hp.water_tank_temp));
        lv_label_set_text(state.hero_tank_label, buf);
        lv_obj_set_style_text_color(state.hero_tank_label, COLOR_TEXT, LV_PART_MAIN);
        lv_label_set_text(state.hero_unit_label, app_prefs_temp_unit_str());
        lv_obj_clear_flag(state.hero_unit_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(state.hero_tank_label, "--");
        lv_obj_set_style_text_color(state.hero_tank_label, COLOR_VALUE_DIM, LV_PART_MAIN);
        lv_obj_add_flag(state.hero_unit_label, LV_OBJ_FLAG_HIDDEN);
    }

    const char* sub = i18n_get(STR_HOME_TANK);
    buf[0] = '\0';
    if ((hero == HeroState::HEATING || hero == HeroState::COOLING) && snap.setpoint_valid) {
        sub = i18n_get(STR_HOME_TANK_TARGET);
        format_temp(buf, sizeof(buf), snap.active_setpoint_c);
    } else if (defrost) {
        sub = i18n_get(STR_HOME_TANK_DEFROST);
        format_duration(buf, sizeof(buf), now > state.defrost_since ? now - state.defrost_since : 0);
    }
    lv_label_set_text(state.hero_sub_label, sub);
    lv_label_set_text(state.hero_sub_value, buf);
    set_visible(state.hero_sub_value, buf[0] != '\0');

    bool show_chart = (hero == HeroState::IDLE || hero == HeroState::STANDBY) &&
                      history_has_data(s_history);
    set_visible(state.chart, show_chart);
    if (show_chart) {
        update_fault_caption();
    } else {
        lv_obj_add_flag(state.fault_caption, LV_OBJ_FLAG_HIDDEN);
    }

    // Tiles
    lv_color_t value_color = running ? COLOR_TEXT : COLOR_VALUE_DIM;
    if (hp.connected) {
        format_temp(buf, sizeof(buf), hp.outlet_water_temp);
        set_tile(state.supply, buf, value_color, false);
        format_temp(buf, sizeof(buf), hp.inlet_water_temp);
        set_tile(state.ret, buf, value_color, false);
        int16_t dt = hp.outlet_water_temp - hp.inlet_water_temp;
        snprintf(buf, sizeof(buf), "%d" DEGREE, app_prefs_convert_temp_diff(dt));
        set_tile(state.dt, buf, value_color, hero == HeroState::HEATING && dt < DT_WARN_C);
        format_power(buf, sizeof(buf), hp.realtime_power_w);
        set_tile(state.power, buf, value_color, false);
        if (hp.cop_valid && !defrost) {
            snprintf(buf, sizeof(buf), "%u.%02u", hp.cop_x100 / 100, hp.cop_x100 % 100);
            set_tile(state.cop, buf, value_color, hp.perf_fallback);
        } else {
            set_tile(state.cop, "\xE2\x80\x94", COLOR_VALUE_DIM, false);
        }
        snprintf(buf, sizeof(buf), "%u Hz", hp.compressor_freq);
        set_tile(state.hz, buf, value_color, false);
    } else {
        for (const Tile* t : {&state.supply, &state.ret, &state.dt, &state.power,
                              &state.cop, &state.hz}) {
            set_tile(*t, "--", COLOR_VALUE_DIM, false);
        }
    }
    set_visible(state.dt.box, running);
    set_visible(state.cop.box, running);
    set_visible(state.hz.box, running);

    // Strip
    const home_run_summary_t& summary = s_history.summary;
    if (running) {
        uint32_t since = summary.current.start;
        if (state.run_since != 0 && (since == 0 || state.run_since > since + 120)) {
            since = state.run_since;
        }
        if (since != 0 && synced && now >= since) {
            format_duration(buf, sizeof(buf), now - since);
        } else {
            snprintf(buf, sizeof(buf), "--");
        }
        set_strip(0, i18n_get(STR_HOME_RUNNING), buf, COLOR_TEXT);

        char starts[16] = "--";
        bool many = false;
        if (s_history.loaded) {
            snprintf(starts, sizeof(starts), i18n_get(STR_HOME_PER_HOUR),
                     (unsigned)summary.starts_last_hour);
            many = summary.starts_last_hour >= STARTS_WARN_PER_HOUR;
        }
        set_strip(1, i18n_get(STR_HOME_STARTS), starts, many ? COLOR_WARNING : COLOR_TEXT);

        char discharge[16] = "--";
        if (hp.connected) format_temp(discharge, sizeof(discharge), hp.discharge_temp);
        set_strip(2, i18n_get(STR_HP_DISCHARGE), discharge, COLOR_TEXT);
    } else {
        // A run that just ended can still look current in history until the
        // next sample records it stopped.
        home_run_t last = summary.last;
        if (summary.current.start != 0) {
            last = summary.current;
            last.end = state.run_stopped > last.start ? state.run_stopped : s_history.end;
        }
        if (last.start != 0 && last.end > last.start) {
            char clock[16];
            char dur[24];
            format_clock(clock, sizeof(clock), last.start);
            format_duration(dur, sizeof(dur), last.end - last.start);
            snprintf(buf, sizeof(buf), "%s \xC2\xB7 %s", clock, dur);
        } else {
            snprintf(buf, sizeof(buf), "--");
        }
        set_strip(0, i18n_get(STR_HOME_LAST_RUN), buf, COLOR_TEXT);

        char energy[24] = "--";
        if (hp.energy_today_valid) {
            snprintf(energy, sizeof(energy), "%lu.%lu kWh",
                     (unsigned long)(hp.energy_today_wh / 1000),
                     (unsigned long)((hp.energy_today_wh % 1000) / 100));
        }
        set_strip(1, i18n_get(STR_EVENT_TODAY), energy, COLOR_TEXT);
        set_strip(2, nullptr, nullptr, COLOR_TEXT);
    }
}

void heatpump_screen_delete(void) {
    if (!state.created) {
        return;
    }

    if (state.history_open && heatpump_history_is_shown()) {
        heatpump_history_hide();
    }

    if (state.update_timer) {
        lv_timer_del(state.update_timer);
        state.update_timer = nullptr;
    }

    if (state.container) {
        lv_obj_del(state.container);
        state.container = nullptr;
    }

    // Drop any query still in flight; its result belongs to the old widgets.
    s_generation++;
    s_query_pending = false;
    state = {};
}

bool heatpump_screen_is_created(void) {
    return state.created;
}

void heatpump_screen_set_demo_banner(bool visible) {
    if (!state.created || !state.demo_banner) {
        return;
    }
    set_visible(state.demo_banner, visible);
}

void heatpump_screen_update_device_name(void) {
    update_device_name_label();
}
