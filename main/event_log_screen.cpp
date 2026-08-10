/*
 * Arctic Heat Pump Controller
 * Event Log Screen Implementation
 * 
 * Full-screen scrollable list of recent operational events, newest first.
 */

#include "event_log_screen.h"
#include "nav_bar.h"
#include "event_log.h"
#include "heatpump_errors.h"
#include "time_manager.h"
#include "ui_common.h"
#include "fonts/fonts.h"
#include "i18n/i18n.h"
#include "settings/keyboard_maps.h"
#include <ctype.h>
#include <esp_log.h>
#include <esp_system.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char* TAG = "evt_screen";

// ============================================================================
// Colors
// ============================================================================
#define COLOR_BG            lv_color_hex(0x1a1a2e)
#define COLOR_CARD_BG       lv_color_hex(0x16213e)
#define COLOR_HEADER_BG     lv_color_hex(0x0f1a2e)
#define COLOR_TEXT          lv_color_hex(0xeaeaea)
#define COLOR_TEXT_DIM      lv_color_hex(0x888888)
#define COLOR_ACCENT        lv_color_hex(0x00d4ff)
#define COLOR_SUCCESS       lv_color_hex(0x4ade80)
#define COLOR_WARNING       lv_color_hex(0xfbbf24)
#define COLOR_ERROR         lv_color_hex(0xef4444)

// ============================================================================
// State
// ============================================================================

enum event_category_mask_t : uint8_t {
    EVENT_CATEGORY_MASK_PROBLEMS  = 1 << EVENT_CATEGORY_PROBLEMS,
    EVENT_CATEGORY_MASK_EQUIPMENT = 1 << EVENT_CATEGORY_EQUIPMENT,
    EVENT_CATEGORY_MASK_CHANGES   = 1 << EVENT_CATEGORY_CHANGES,
    EVENT_CATEGORY_MASK_SYSTEM    = 1 << EVENT_CATEGORY_SYSTEM,
};

enum event_time_filter_t : uint8_t {
    EVENT_TIME_ALL = 0,
    EVENT_TIME_TODAY,
    EVENT_TIME_LAST_24_HOURS,
    EVENT_TIME_LAST_7_DAYS,
    EVENT_TIME_CURRENT_BOOT,
    EVENT_TIME_COUNT,
};

static const int FILTER_BAR_H = 166;

static struct {
    bool shown = false;
    lv_obj_t* screen = nullptr;
    event_log_screen_close_cb_t on_close = nullptr;
    lv_timer_t* update_timer = nullptr;
    lv_obj_t* content = nullptr;
    lv_obj_t* count_label = nullptr;
    int last_count = -1;    // Force rebuild on first update
    uint32_t last_revision = 0;
    int displayed = 0;      // Rows currently rendered (for lazy "load more")
    lv_obj_t* footer = nullptr;    // Dim "loading older events" footer / scroll anchor
    bool loading = false;   // Re-entrancy guard while appending a batch
    int last_date_key = INT_MIN;
    int today_key = INT_MIN;
    lv_obj_t* controls = nullptr;
    lv_obj_t* search_label = nullptr;
    lv_obj_t* search_clear_btn = nullptr;
    lv_obj_t* filters_label = nullptr;
    lv_obj_t* results_label = nullptr;
    lv_obj_t* new_events_btn = nullptr;
    lv_obj_t* category_btn[4] = {};
    lv_obj_t* search_overlay = nullptr;
    lv_obj_t* search_textarea = nullptr;
    lv_obj_t* filter_overlay = nullptr;
    lv_obj_t* time_btn[EVENT_TIME_COUNT] = {};
    event_entry_t filtered_events[EVENT_LOG_MAX_ENTRIES] = {};
    int filtered_count = 0;
    uint8_t category_mask = 0;
    uint8_t pending_category_mask = 0;
    event_time_filter_t time_filter = EVENT_TIME_ALL;
    event_time_filter_t pending_time_filter = EVENT_TIME_ALL;
    char search_query[64] = {};
    bool pending_refresh = false;
} state;

// ============================================================================
// Helpers
// ============================================================================

static string_id_t event_type_to_str_id(event_type_t type) {
    switch (type) {
        case EVENT_SYSTEM_START:    return STR_EVENT_SYSTEM_START;
        case EVENT_POWER_ON:        return STR_EVENT_POWER_ON;
        case EVENT_POWER_OFF:       return STR_EVENT_POWER_OFF;
        case EVENT_MODE_CHANGED:    return STR_EVENT_MODE_CHANGED;
        case EVENT_SETPOINT_CHANGED:return STR_EVENT_SETPOINT_CHANGED;
        case EVENT_COMPRESSOR_ON:   return STR_EVENT_COMPRESSOR_ON;
        case EVENT_COMPRESSOR_OFF:  return STR_EVENT_COMPRESSOR_OFF;
        case EVENT_FAN_ON:          return STR_EVENT_FAN_ON;
        case EVENT_FAN_OFF:         return STR_EVENT_FAN_OFF;
        case EVENT_PUMP_ON:         return STR_EVENT_PUMP_ON;
        case EVENT_PUMP_OFF:        return STR_EVENT_PUMP_OFF;
        case EVENT_AUX_HEATER_ON:   return STR_EVENT_AUX_HEATER_ON;
        case EVENT_AUX_HEATER_OFF:  return STR_EVENT_AUX_HEATER_OFF;
        case EVENT_DEFROST_START:   return STR_EVENT_DEFROST_START;
        case EVENT_DEFROST_END:     return STR_EVENT_DEFROST_END;
        case EVENT_ERROR_APPEARED:  return STR_EVENT_ERROR_APPEARED;
        case EVENT_ERROR_CLEARED:   return STR_EVENT_ERROR_CLEARED;
        case EVENT_CONNECTED:       return STR_EVENT_CONNECTED;
        case EVENT_DISCONNECTED:    return STR_EVENT_DISCONNECTED;
        case EVENT_BROWNOUT_RESET:  return STR_EVENT_BROWNOUT_RESET;
        case EVENT_APPLICATION_CRASH:return STR_EVENT_APPLICATION_CRASH;
        case EVENT_WATCHDOG_RESET:   return STR_EVENT_WATCHDOG_RESET;
        default:                    return STR_EVENT_SYSTEM_START;
    }
}

static const char* event_type_icon(event_type_t type) {
    switch (type) {
        case EVENT_SYSTEM_START:    return LV_SYMBOL_CHARGE;
        case EVENT_POWER_ON:        return LV_SYMBOL_POWER;
        case EVENT_POWER_OFF:       return LV_SYMBOL_POWER;
        case EVENT_MODE_CHANGED:    return LV_SYMBOL_LOOP;
        case EVENT_SETPOINT_CHANGED:return LV_SYMBOL_EDIT;
        case EVENT_COMPRESSOR_ON:   return LV_SYMBOL_SETTINGS;
        case EVENT_COMPRESSOR_OFF:  return LV_SYMBOL_SETTINGS;
        case EVENT_FAN_ON:          return LV_SYMBOL_SHUFFLE;
        case EVENT_FAN_OFF:         return LV_SYMBOL_SHUFFLE;
        case EVENT_PUMP_ON:         return LV_SYMBOL_TINT;
        case EVENT_PUMP_OFF:        return LV_SYMBOL_TINT;
        case EVENT_AUX_HEATER_ON:   return LV_SYMBOL_PLUS;
        case EVENT_AUX_HEATER_OFF:  return LV_SYMBOL_PLUS;
        case EVENT_DEFROST_START:   return LV_SYMBOL_REFRESH;
        case EVENT_DEFROST_END:     return LV_SYMBOL_REFRESH;
        case EVENT_ERROR_APPEARED:  return LV_SYMBOL_WARNING;
        case EVENT_ERROR_CLEARED:   return LV_SYMBOL_OK;
        case EVENT_CONNECTED:       return LV_SYMBOL_WIFI;
        case EVENT_DISCONNECTED:    return LV_SYMBOL_WIFI;
        case EVENT_BROWNOUT_RESET:  return LV_SYMBOL_WARNING;
        case EVENT_APPLICATION_CRASH:return LV_SYMBOL_WARNING;
        case EVENT_WATCHDOG_RESET:   return LV_SYMBOL_WARNING;
        default:                    return LV_SYMBOL_FILE;
    }
}

static lv_color_t event_type_color(event_type_t type) {
    switch (type) {
        case EVENT_POWER_ON:
        case EVENT_COMPRESSOR_ON:
        case EVENT_FAN_ON:
        case EVENT_PUMP_ON:
        case EVENT_AUX_HEATER_ON:
        case EVENT_CONNECTED:
        case EVENT_DEFROST_END:
        case EVENT_ERROR_CLEARED:
            return COLOR_SUCCESS;
        
        case EVENT_ERROR_APPEARED:
            return COLOR_ERROR;
        
        case EVENT_BROWNOUT_RESET:
        case EVENT_APPLICATION_CRASH:
        case EVENT_WATCHDOG_RESET:
            return COLOR_ERROR;
        
        case EVENT_DEFROST_START:
        case EVENT_AUX_HEATER_OFF:
            return COLOR_WARNING;
        
        case EVENT_MODE_CHANGED:
        case EVENT_SETPOINT_CHANGED:
        case EVENT_SYSTEM_START:
            return COLOR_ACCENT;
        
        default:
            return COLOR_TEXT_DIM;
    }
}

static const char* mode_name_i18n(int mode) {
    switch (mode) {
        case 0: return i18n_get(STR_HP_MODE_COOLING);
        case 1: return i18n_get(STR_HP_MODE_FLOOR_HEAT);
        case 2: return i18n_get(STR_HP_MODE_FAN_HEAT);
        case 5: return i18n_get(STR_HP_MODE_HOT_WATER);
        case 6: return i18n_get(STR_HP_MODE_AUTO);
        default: return "?";
    }
}

static void format_event_detail(char* buf, size_t buf_size, const event_entry_t* evt) {
    uint32_t p = evt->payload;
    
    switch (evt->type) {
        case EVENT_MODE_CHANGED: {
            int old_mode = (p >> 8) & 0xFF;
            int new_mode = p & 0xFF;
            snprintf(buf, buf_size, "%s → %s", mode_name_i18n(old_mode), mode_name_i18n(new_mode));
            break;
        }
        case EVENT_SETPOINT_CHANGED: {
            int sp_type = (p >> 16) & 0xFF;
            int old_val = (p >> 8) & 0xFF;
            int new_val = p & 0xFF;
            const char* sp_names[] = {
                "Cooling", "Heating", "Hot Water"
            };
            const char* name = (sp_type < 3) ? sp_names[sp_type] : "?";
            snprintf(buf, buf_size, "%s: %d° → %d°", name, old_val, new_val);
            break;
        }
        case EVENT_ERROR_APPEARED:
        case EVENT_ERROR_CLEARED: {
            int reg = (p >> 16) & 0xFFFF;
            uint16_t bit = p & 0xFFFF;
            // Look up error code and description from error definitions
            const arctic::ErrorDef* defs = nullptr;
            int count = 0;
            if (reg == 1) {
                defs = arctic::getError1Definitions(&count);
            } else if (reg == 2) {
                defs = arctic::getError2Definitions(&count);
            }
            bool found = false;
            if (defs) {
                for (int i = 0; i < count; i++) {
                    if (defs[i].mask == bit) {
                        snprintf(buf, buf_size, "(%s) %s", defs[i].code, defs[i].description);
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                snprintf(buf, buf_size, "Reg %d bit 0x%04X", reg, bit);
            }
            break;
        }
        case EVENT_WATCHDOG_RESET:
            switch ((esp_reset_reason_t)p) {
                case ESP_RST_INT_WDT:
                    snprintf(buf, buf_size, "%s", i18n_get(STR_EVENT_WATCHDOG_INTERRUPT));
                    break;
                case ESP_RST_TASK_WDT:
                    snprintf(buf, buf_size, "%s", i18n_get(STR_EVENT_WATCHDOG_TASK));
                    break;
                default:
                    snprintf(buf, buf_size, "%s", i18n_get(STR_EVENT_WATCHDOG_OTHER));
                    break;
            }
            break;
        default:
            buf[0] = '\0';
            break;
    }
}

static int local_date_key(time_t timestamp);

static bool contains_case_insensitive(const char* haystack, const char* needle) {
    if (!needle || needle[0] == '\0') return true;
    if (!haystack) return false;

    size_t needle_len = strlen(needle);
    for (const char* start = haystack; *start; start++) {
        size_t i = 0;
        while (i < needle_len && start[i] &&
               tolower((unsigned char)start[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == needle_len) return true;
    }
    return false;
}

static bool event_matches_time(const event_entry_t* evt, time_t now) {
    switch (state.time_filter) {
        case EVENT_TIME_TODAY:
            return evt->timestamp > 0 &&
                   local_date_key((time_t)evt->timestamp) == local_date_key(now);
        case EVENT_TIME_LAST_24_HOURS:
            return evt->timestamp > 0 && (time_t)evt->timestamp >= now - (24 * 60 * 60);
        case EVENT_TIME_LAST_7_DAYS:
            return evt->timestamp > 0 && (time_t)evt->timestamp >= now - (7 * 24 * 60 * 60);
        case EVENT_TIME_CURRENT_BOOT:
            return evt->boot_id != 0 && evt->boot_id == event_log_current_boot_id();
        case EVENT_TIME_ALL:
        default:
            return true;
    }
}

static bool event_matches_search(const event_entry_t* evt) {
    if (state.search_query[0] == '\0') return true;

    char detail[128];
    format_event_detail(detail, sizeof(detail), evt);
    return contains_case_insensitive(i18n_get(event_type_to_str_id(evt->type)), state.search_query) ||
           contains_case_insensitive(event_type_name(evt->type), state.search_query) ||
           contains_case_insensitive(detail, state.search_query);
}

static bool event_matches_filters(const event_entry_t* evt, time_t now) {
    if (state.category_mask != 0 &&
        ((1u << event_type_category(evt->type)) & state.category_mask) == 0) {
        return false;
    }
    return event_matches_time(evt, now) && event_matches_search(evt);
}

static int active_filter_count() {
    int count = state.time_filter == EVENT_TIME_ALL ? 0 : 1;
    for (uint8_t bit = 1; bit <= EVENT_CATEGORY_MASK_SYSTEM; bit <<= 1) {
        if (state.category_mask & bit) count++;
    }
    return count;
}

static void style_toggle_button(lv_obj_t* btn) {
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x252b38), LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        btn, COLOR_ACCENT,
        static_cast<lv_style_selector_t>(LV_PART_MAIN) |
            static_cast<lv_style_selector_t>(LV_STATE_CHECKED));
    lv_obj_set_style_text_color(btn, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_color(
        btn, COLOR_BG,
        static_cast<lv_style_selector_t>(LV_PART_MAIN) |
            static_cast<lv_style_selector_t>(LV_STATE_CHECKED));
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x4b5563), LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
}

static void set_toggle_checked(lv_obj_t* btn, bool checked) {
    if (!btn) return;
    if (checked) {
        lv_obj_add_state(btn, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(btn, LV_STATE_CHECKED);
    }
    lv_obj_t* label = lv_obj_get_child(btn, 0);
    if (label) {
        lv_obj_set_style_text_color(label, checked ? COLOR_BG : COLOR_TEXT, LV_PART_MAIN);
    }
}

static void format_event_time(char* buf, size_t buf_size, const event_entry_t* evt) {
    if (evt->timestamp > 0) {
        time_t t = (time_t)evt->timestamp;
        struct tm tm;
        localtime_r(&t, &tm);
        // Respect the user's 12/24-hour preference (Settings → Time).
        const char* fmt = time_mgr_get_24h_format() ? "%H:%M:%S" : "%I:%M:%S %p";
        strftime(buf, buf_size, fmt, &tm);
    } else {
        // Use uptime
        uint64_t sec = evt->uptime_ms / 1000;
        uint64_t min = sec / 60;
        uint64_t hr = min / 60;
        snprintf(buf, buf_size, "+%luh%02lum%02lus", (unsigned long)hr, (unsigned long)(min % 60), (unsigned long)(sec % 60));
    }
}

static int local_date_key(time_t timestamp) {
    struct tm tm;
    localtime_r(&timestamp, &tm);
    return (tm.tm_year * 512) + tm.tm_yday;
}

static int event_date_key(const event_entry_t* evt) {
    return evt->timestamp > 0 ? local_date_key((time_t)evt->timestamp) : -1;
}

static void format_event_date(char* buf, size_t buf_size, const event_entry_t* evt) {
    if (evt->timestamp == 0) {
        snprintf(buf, buf_size, "%s", i18n_get(STR_EVENT_SINCE_RESTART));
        return;
    }

    time_t event_time = (time_t)evt->timestamp;
    struct tm event_tm;
    localtime_r(&event_time, &event_tm);

    time_t now = time(nullptr);
    struct tm today_tm;
    localtime_r(&now, &today_tm);
    if (event_tm.tm_year == today_tm.tm_year && event_tm.tm_yday == today_tm.tm_yday) {
        snprintf(buf, buf_size, "%s", i18n_get(STR_EVENT_TODAY));
        return;
    }

    struct tm yesterday_tm = today_tm;
    yesterday_tm.tm_mday -= 1;
    mktime(&yesterday_tm);
    if (event_tm.tm_year == yesterday_tm.tm_year && event_tm.tm_yday == yesterday_tm.tm_yday) {
        snprintf(buf, buf_size, "%s", i18n_get(STR_EVENT_YESTERDAY));
        return;
    }

    const char* month = i18n_get((string_id_t)(STR_EVENT_MONTH_JAN + event_tm.tm_mon));
    bool current_year = event_tm.tm_year == today_tm.tm_year;
    if (i18n_get_language() == LANG_ENGLISH) {
        if (current_year) {
            snprintf(buf, buf_size, "%s %d", month, event_tm.tm_mday);
        } else {
            snprintf(buf, buf_size, "%s %d, %d", month, event_tm.tm_mday, event_tm.tm_year + 1900);
        }
    } else {
        if (current_year) {
            snprintf(buf, buf_size, "%d %s", event_tm.tm_mday, month);
        } else {
            snprintf(buf, buf_size, "%d %s %d", event_tm.tm_mday, month, event_tm.tm_year + 1900);
        }
    }
}

// Forward declarations
static void clear_btn_cb(lv_event_t* e);
static void rebuild_event_list();
static void close_overlays();
static void reset_screen_refs();

// ============================================================================
// Build / Rebuild Event List
// ============================================================================

// Rows are rendered lazily in batches. Rendering all 128 rows at once inside a
// flex container triggers O(n²) layout recalculation (10+ seconds, HTTP
// timeouts while the click handler holds the display lock). Instead we render
// EVENT_BATCH_SIZE rows up front and append more as the user scrolls (infinite
// scroll), keeping each layout pass small.
static const int EVENT_BATCH_SIZE = 10;  // rows per lazy-load batch (infinite scroll)
// Start loading the next batch when the user scrolls within this many px of the bottom.
static const int32_t EVENT_SCROLL_THRESHOLD_PX = 300;

static void create_date_separator(lv_obj_t* parent, const event_entry_t* evt) {
    int date_key = event_date_key(evt);
    if (date_key == state.last_date_key) return;
    state.last_date_key = date_key;

    char date_buf[48];
    format_event_date(date_buf, sizeof(date_buf), evt);

    lv_obj_t* separator = lv_obj_create(parent);
    lv_obj_set_size(separator, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(separator, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(separator, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(separator, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_top(separator, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(separator, 4, LV_PART_MAIN);
    lv_obj_clear_flag(separator, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(separator, (void*)"event_date_separator");

    lv_obj_t* label = lv_label_create(separator);
    lv_label_set_text(label, date_buf);
    lv_obj_set_style_text_font(label, &montserrat_24_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, COLOR_ACCENT, LV_PART_MAIN);
}

// Build a single event row (card) into the given flex-column parent.
static void create_event_row(lv_obj_t* parent, const event_entry_t* evt) {
    char detail_buf[128];
    char time_buf[32];

    create_date_separator(parent, evt);

    // Row container - flex column layout like error cards
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 15, LV_PART_MAIN);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(row, 6, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    // Top row: event name + timestamp
    lv_obj_t* top_row = lv_obj_create(row);
    lv_obj_set_size(top_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(top_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(top_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(top_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(top_row, LV_OBJ_FLAG_SCROLLABLE);

    // Color indicator bar on left
    lv_color_t evt_color = event_type_color(evt->type);
    lv_obj_t* indicator = lv_obj_create(top_row);
    lv_obj_set_size(indicator, 4, LV_PCT(100));
    lv_obj_align(indicator, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(indicator, evt_color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(indicator, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(indicator, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(indicator, 2, LV_PART_MAIN);
    lv_obj_clear_flag(indicator, LV_OBJ_FLAG_SCROLLABLE);

    // Event type icon + name
    char name_buf[96];
    snprintf(name_buf, sizeof(name_buf), "%s  %s", event_type_icon(evt->type), i18n_get(event_type_to_str_id(evt->type)));
    lv_obj_t* name_lbl = lv_label_create(top_row);
    lv_label_set_text(name_lbl, name_buf);
    lv_obj_set_style_text_font(name_lbl, &montserrat_32_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(name_lbl, evt_color, LV_PART_MAIN);
    lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 16, 0);

    // Timestamp on the right
    format_event_time(time_buf, sizeof(time_buf), evt);
    lv_obj_t* time_lbl = lv_label_create(top_row);
    lv_label_set_text(time_lbl, time_buf);
    lv_obj_set_style_text_font(time_lbl, &montserrat_24_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(time_lbl, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(time_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    // Detail text (mode change, setpoint, error description)
    format_event_detail(detail_buf, sizeof(detail_buf), evt);
    if (detail_buf[0] != '\0') {
        lv_obj_t* detail_lbl = lv_label_create(row);
        lv_label_set_text(detail_lbl, detail_buf);
        lv_label_set_long_mode(detail_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(detail_lbl, LV_PCT(100));
        lv_obj_set_style_text_font(detail_lbl, &montserrat_24_latin, LV_PART_MAIN);
        lv_obj_set_style_text_color(detail_lbl, COLOR_TEXT_DIM, LV_PART_MAIN);
    }
}

// Create / update / remove the dim footer that indicates more events will load.
static void update_footer(int remaining) {
    if (remaining <= 0) {
        if (state.footer) {
            lv_obj_del(state.footer);
            state.footer = nullptr;
        }
        return;
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "%s (%d)", i18n_get(STR_EVENT_SHOW_OLDER), remaining);

    if (!state.footer) {
        state.footer = lv_label_create(state.content);
        lv_obj_set_width(state.footer, LV_PCT(100));
        lv_obj_set_style_text_align(state.footer, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_color(state.footer, COLOR_TEXT_DIM, LV_PART_MAIN);
        lv_obj_set_style_text_font(state.footer, &montserrat_24_latin, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(state.footer, 12, LV_PART_MAIN);
        lv_obj_set_user_data(state.footer, (void*)"event_log_more");
    }
    // Keep the footer as the last child so it stays at the bottom of the list.
    lv_obj_move_to_index(state.footer, lv_obj_get_child_count(state.content) - 1);
    lv_label_set_text(state.footer, buf);
}

// Append the next batch of older events (used by the infinite-scroll handler).
static void append_next_batch() {
    if (!state.content || state.loading) return;

    int count = state.filtered_count;
    int to_fetch = count - state.displayed;
    if (to_fetch > EVENT_BATCH_SIZE) to_fetch = EVENT_BATCH_SIZE;
    if (to_fetch <= 0) {
        update_footer(0);
        return;
    }

    state.loading = true;

    int got = to_fetch;

    // Batch-render with layout suspended to avoid O(n²) recalc thrash.
    int32_t scroll_y = lv_obj_get_scroll_y(state.content);
    lv_obj_add_flag(state.content, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < got; i++) {
        create_event_row(state.content, &state.filtered_events[state.displayed + i]);
    }
    state.displayed += got;
    update_footer(count - state.displayed);
    lv_obj_clear_flag(state.content, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(state.content);
    lv_obj_scroll_to_y(state.content, scroll_y, LV_ANIM_OFF);

    state.loading = false;
}

// Infinite scroll: load the next batch when the user nears the bottom.
static void content_scroll_cb(lv_event_t* e) {
    (void)e;
    if (!state.content || state.loading) return;
    if (state.displayed >= state.filtered_count) return;
    if (lv_obj_get_scroll_bottom(state.content) <= EVENT_SCROLL_THRESHOLD_PX) {
        append_next_batch();
    }
}

static void rebuild_event_list() {
    if (!state.content) return;
    
    // Deleting children can synchronously emit LV_EVENT_SCROLL as the content
    // height collapses. Block lazy loading and invalidate the footer first so
    // the scroll callback cannot touch objects being deleted.
    state.loading = true;
    state.footer = nullptr;
    lv_obj_clean(state.content);
    state.loading = false;
    state.displayed = 0;
    state.last_date_key = INT_MIN;
    state.today_key = local_date_key(time(nullptr));
    state.pending_refresh = false;
    if (state.new_events_btn) {
        lv_obj_add_flag(state.new_events_btn, LV_OBJ_FLAG_HIDDEN);
    }

    // Disable scrolling/layout during batch creation to avoid O(n²) recalc
    lv_obj_add_flag(state.content, LV_OBJ_FLAG_HIDDEN);

    int total_count = event_log_count();
    state.filtered_count = 0;
    time_t now = time(nullptr);
    constexpr int FILTER_BATCH_SIZE = 32;
    event_entry_t batch[FILTER_BATCH_SIZE];
    for (int offset = 0; offset < total_count; offset += FILTER_BATCH_SIZE) {
        int got = event_log_get(batch, FILTER_BATCH_SIZE, offset);
        for (int i = 0; i < got; i++) {
            if (event_matches_filters(&batch[i], now)) {
                state.filtered_events[state.filtered_count++] = batch[i];
            }
        }
    }
    int count = state.filtered_count;

    if (state.search_label) {
        lv_label_set_text(
            state.search_label,
            state.search_query[0] ? state.search_query : i18n_get(STR_EVENT_SEARCH));
    }
    if (state.search_clear_btn) {
        if (state.search_query[0]) {
            lv_obj_clear_flag(state.search_clear_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(state.search_clear_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (state.filters_label) {
        char filters_buf[48];
        int filter_count = active_filter_count();
        if (filter_count > 0) {
            snprintf(filters_buf, sizeof(filters_buf), "%s (%d)",
                     i18n_get(STR_EVENT_FILTERS), filter_count);
        } else {
            snprintf(filters_buf, sizeof(filters_buf), "%s", i18n_get(STR_EVENT_FILTERS));
        }
        lv_label_set_text(state.filters_label, filters_buf);
    }
    if (state.results_label) {
        char results_buf[64];
        snprintf(results_buf, sizeof(results_buf), i18n_get(STR_EVENT_RESULTS_FMT),
                 count, total_count);
        lv_label_set_text(state.results_label, results_buf);
    }
    
    // Update count label in header
    if (state.count_label) {
        char title_buf[64];
        snprintf(title_buf, sizeof(title_buf), "%s (%d)", i18n_get(STR_EVENT_LOG), count);
        lv_label_set_text(state.count_label, title_buf);
    }
    
    if (count == 0) {
        lv_obj_t* msg = lv_label_create(state.content);
        bool has_filters = state.search_query[0] != '\0' ||
                           state.category_mask != 0 ||
                           state.time_filter != EVENT_TIME_ALL;
        lv_label_set_text(msg, i18n_get(has_filters ? STR_EVENT_NO_MATCHES : STR_EVENT_NO_EVENTS));
        lv_obj_set_user_data(
            msg, (void*)(has_filters ? "event_log_no_matches" : "event_log_empty"));
        lv_obj_set_style_text_color(msg, COLOR_TEXT_DIM, LV_PART_MAIN);
        lv_obj_set_style_text_font(msg, &montserrat_24_latin, LV_PART_MAIN);
        lv_obj_set_width(msg, LV_PCT(100));
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_pad_top(msg, 40, LV_PART_MAIN);

        lv_obj_clear_flag(state.content, LV_OBJ_FLAG_HIDDEN);
        state.last_count = total_count;
        state.last_revision = event_log_revision();
        return;
    }
    
    // Reset lazy-load state; the footer will be recreated below if needed.
    state.footer = nullptr;
    state.loading = false;
    state.displayed = 0;

    // Render the first batch only; older events load on scroll (infinite scroll).
    int first_batch = count < EVENT_BATCH_SIZE ? count : EVENT_BATCH_SIZE;

    for (int i = 0; i < first_batch; i++) {
        create_event_row(state.content, &state.filtered_events[i]);
    }
    state.displayed = first_batch;

    // Dim footer hints at the remaining (up to 128) older events that load on scroll.
    update_footer(count - state.displayed);

    // Re-enable visibility and force layout calculation once
    lv_obj_clear_flag(state.content, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(state.content);

    state.last_count = total_count;
    state.last_revision = event_log_revision();

    // Guarantee the list overflows the viewport; otherwise there is nothing to
    // scroll and infinite scroll could never trigger. Append until scrollable.
    int guard = 0;
    while (state.displayed < count &&
           lv_obj_get_scroll_bottom(state.content) <= 0 &&
           guard++ < 20) {
        append_next_batch();
    }
}

// ============================================================================
// Timer callback - only rebuild if count changed
// ============================================================================

static void update_timer_cb(lv_timer_t* timer) {
    (void)timer;
    if (!state.shown) return;
    int count = event_log_count();
    uint32_t revision = event_log_revision();
    int today_key = local_date_key(time(nullptr));
    if (count != state.last_count || revision != state.last_revision || today_key != state.today_key) {
        if (state.content && lv_obj_get_scroll_y(state.content) > 100) {
            state.pending_refresh = true;
            if (state.new_events_btn) {
                lv_obj_clear_flag(state.new_events_btn, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            rebuild_event_list();
        }
    }
}

// ============================================================================
// Callbacks
// ============================================================================

static void clear_btn_cb(lv_event_t* e) {
    (void)e;
    lv_obj_t* mbox = lv_msgbox_create(lv_scr_act());
    lv_msgbox_add_title(mbox, i18n_get(STR_EVENT_CLEAR_CONFIRM_TITLE));
    lv_msgbox_add_text(mbox, i18n_get(STR_EVENT_CLEAR_CONFIRM_TEXT));
    lv_obj_t* clear_btn = lv_msgbox_add_footer_button(mbox, i18n_get(STR_EVENT_CLEAR));
    lv_obj_set_user_data(clear_btn, (void*)"event_log_clear_confirm");
    lv_obj_add_event_cb(clear_btn, [](lv_event_t* event) {
        lv_obj_t* box = (lv_obj_t*)lv_event_get_user_data(event);
        ESP_LOGI(TAG, "Clear events");
        event_log_clear();
        lv_msgbox_close(box);
        rebuild_event_list();
    }, LV_EVENT_CLICKED, mbox);
    lv_msgbox_add_close_button(mbox);
    lv_obj_set_width(mbox, 520);
    lv_obj_center(mbox);
}

static void new_events_btn_cb(lv_event_t* e) {
    (void)e;
    rebuild_event_list();
}

static void category_btn_cb(lv_event_t* e) {
    lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
    uint8_t category = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (state.category_mask & category) {
        state.category_mask &= ~category;
        set_toggle_checked(btn, false);
    } else {
        state.category_mask |= category;
        set_toggle_checked(btn, true);
    }
    rebuild_event_list();
}

static void search_cancel_cb(lv_event_t* e) {
    (void)e;
    if (state.search_overlay) {
        lv_obj_add_flag(state.search_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void search_apply_cb(lv_event_t* e) {
    (void)e;
    if (!state.search_textarea) return;
    snprintf(state.search_query, sizeof(state.search_query), "%s",
             lv_textarea_get_text(state.search_textarea));
    if (state.search_overlay) {
        lv_obj_add_flag(state.search_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    rebuild_event_list();
}

static void search_open_cb(lv_event_t* e) {
    (void)e;
    if (!state.search_overlay || !state.search_textarea) return;
    lv_textarea_set_text(state.search_textarea, state.search_query);
    lv_obj_clear_flag(state.search_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(state.search_overlay);
    lv_obj_add_state(state.search_textarea, LV_STATE_FOCUSED);
}

static void search_clear_cb(lv_event_t* e) {
    (void)e;
    state.search_query[0] = '\0';
    rebuild_event_list();
}

static void sync_time_buttons() {
    for (int i = 0; i < EVENT_TIME_COUNT; i++) {
        if (!state.time_btn[i]) continue;
        set_toggle_checked(state.time_btn[i], i == state.pending_time_filter);
    }
}

static void sync_category_buttons() {
    const uint8_t categories[] = {
        EVENT_CATEGORY_MASK_PROBLEMS, EVENT_CATEGORY_MASK_EQUIPMENT,
        EVENT_CATEGORY_MASK_CHANGES, EVENT_CATEGORY_MASK_SYSTEM
    };
    for (int i = 0; i < 4; i++) {
        if (!state.category_btn[i]) continue;
        set_toggle_checked(
            state.category_btn[i], (state.category_mask & categories[i]) != 0);
    }
}

static void time_filter_btn_cb(lv_event_t* e) {
    state.pending_time_filter =
        (event_time_filter_t)(uintptr_t)lv_event_get_user_data(e);
    sync_time_buttons();
}

static void filters_open_cb(lv_event_t* e) {
    (void)e;
    if (!state.filter_overlay) return;
    state.pending_time_filter = state.time_filter;
    state.pending_category_mask = state.category_mask;
    sync_time_buttons();
    lv_obj_clear_flag(state.filter_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(state.filter_overlay);
}

static void filters_cancel_cb(lv_event_t* e) {
    (void)e;
    if (state.filter_overlay) {
        lv_obj_add_flag(state.filter_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void filters_reset_cb(lv_event_t* e) {
    (void)e;
    state.pending_time_filter = EVENT_TIME_ALL;
    state.pending_category_mask = 0;
    sync_time_buttons();
}

static void filters_apply_cb(lv_event_t* e) {
    (void)e;
    state.time_filter = state.pending_time_filter;
    state.category_mask = state.pending_category_mask;
    sync_category_buttons();
    if (state.filter_overlay) {
        lv_obj_add_flag(state.filter_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    rebuild_event_list();
}

static lv_obj_t* create_text_button(lv_obj_t* parent, const char* text,
                                    const char* tag, lv_event_cb_t cb) {
    lv_obj_t* btn = lv_btn_create(parent);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x252b38), LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x4b5563), LV_PART_MAIN);
    lv_obj_set_user_data(btn, (void*)tag);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_center(label);
    return btn;
}

static void create_filter_controls() {
    state.controls = lv_obj_create(state.screen);
    lv_obj_set_size(state.controls, LV_PCT(100), FILTER_BAR_H);
    lv_obj_align(state.controls, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(state.controls, COLOR_HEADER_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state.controls, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.controls, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.controls, 10, LV_PART_MAIN);
    lv_obj_clear_flag(state.controls, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* search_row = lv_obj_create(state.controls);
    lv_obj_set_size(search_row, LV_PCT(100), 58);
    lv_obj_align(search_row, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(search_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(search_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(search_row, 0, LV_PART_MAIN);
    lv_obj_set_layout(search_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(search_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(search_row, 8, LV_PART_MAIN);
    lv_obj_clear_flag(search_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* search_btn = create_text_button(
        search_row, i18n_get(STR_EVENT_SEARCH), "event_search_open", search_open_cb);
    lv_obj_set_width(search_btn, 0);
    lv_obj_set_height(search_btn, LV_PCT(100));
    lv_obj_set_flex_grow(search_btn, 1);
    state.search_label = lv_obj_get_child(search_btn, 0);
    lv_obj_set_width(state.search_label, LV_PCT(82));
    lv_label_set_long_mode(state.search_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(state.search_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_align(state.search_label, LV_ALIGN_LEFT_MID, 12, 0);

    state.search_clear_btn = create_text_button(
        search_btn, LV_SYMBOL_CLOSE, "event_search_clear", search_clear_cb);
    lv_obj_set_size(state.search_clear_btn, 50, 46);
    lv_obj_align(state.search_clear_btn, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_opa(state.search_clear_btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.search_clear_btn, 0, LV_PART_MAIN);
    lv_obj_add_flag(state.search_clear_btn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* filters_btn = create_text_button(
        search_row, i18n_get(STR_EVENT_FILTERS), "event_filters_open", filters_open_cb);
    lv_obj_set_size(filters_btn, 210, LV_PCT(100));
    state.filters_label = lv_obj_get_child(filters_btn, 0);
    lv_obj_set_user_data(state.filters_label, (void*)"event_filters_label");

    lv_obj_t* clear_btn = create_text_button(
        search_row, LV_SYMBOL_TRASH, "event_log_clear", clear_btn_cb);
    lv_obj_set_size(clear_btn, 58, LV_PCT(100));
    lv_obj_set_style_text_color(lv_obj_get_child(clear_btn, 0), COLOR_WARNING, LV_PART_MAIN);

    lv_obj_t* chips = lv_obj_create(state.controls);
    lv_obj_set_size(chips, LV_PCT(100), 50);
    lv_obj_align(chips, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_set_style_bg_opa(chips, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(chips, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chips, 0, LV_PART_MAIN);
    lv_obj_set_layout(chips, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(chips, 8, LV_PART_MAIN);
    lv_obj_clear_flag(chips, LV_OBJ_FLAG_SCROLLABLE);

    const string_id_t category_labels[] = {
        STR_EVENT_PROBLEMS, STR_EVENT_EQUIPMENT, STR_EVENT_CHANGES, STR_EVENT_SYSTEM
    };
    const uint8_t categories[] = {
        EVENT_CATEGORY_MASK_PROBLEMS, EVENT_CATEGORY_MASK_EQUIPMENT,
        EVENT_CATEGORY_MASK_CHANGES, EVENT_CATEGORY_MASK_SYSTEM
    };
    const char* tags[] = {
        "event_filter_problems", "event_filter_equipment",
        "event_filter_changes", "event_filter_system"
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t* btn = create_text_button(
            chips, i18n_get(category_labels[i]), tags[i], nullptr);
        lv_obj_set_width(btn, 0);
        lv_obj_set_height(btn, LV_PCT(100));
        lv_obj_set_flex_grow(btn, 1);
        style_toggle_button(btn);
        lv_obj_set_style_text_font(
            lv_obj_get_child(btn, 0), &montserrat_24_latin, LV_PART_MAIN);
        lv_obj_add_event_cb(
            btn, category_btn_cb, LV_EVENT_CLICKED,
            (void*)(uintptr_t)categories[i]);
        state.category_btn[i] = btn;
    }

    state.results_label = lv_label_create(state.controls);
    lv_label_set_text(state.results_label, "");
    lv_obj_set_user_data(state.results_label, (void*)"event_filter_summary");
    lv_obj_set_style_text_font(state.results_label, &montserrat_24_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.results_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(state.results_label, LV_ALIGN_BOTTOM_LEFT, 4, 0);

    state.new_events_btn = create_text_button(
        state.controls, i18n_get(STR_EVENT_NEW_MATCHES),
        "event_log_new_events", new_events_btn_cb);
    lv_obj_set_size(state.new_events_btn, 300, 38);
    lv_obj_align(state.new_events_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_flag(state.new_events_btn, LV_OBJ_FLAG_HIDDEN);
}

static void create_search_overlay() {
    state.search_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(state.search_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_center(state.search_overlay);
    lv_obj_set_style_bg_color(state.search_overlay, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state.search_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.search_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.search_overlay, 0, LV_PART_MAIN);
    lv_obj_clear_flag(state.search_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* content = lv_obj_create(state.search_overlay);
    lv_obj_set_size(content, LV_PCT(90), 115);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(content, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(content, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(content, 18, LV_PART_MAIN);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    state.search_textarea = lv_textarea_create(content);
    lv_obj_set_size(state.search_textarea, LV_PCT(100), LV_PCT(100));
    lv_textarea_set_one_line(state.search_textarea, true);
    lv_textarea_set_password_mode(state.search_textarea, false);
    lv_textarea_set_max_length(state.search_textarea, sizeof(state.search_query) - 1);
    lv_textarea_set_placeholder_text(state.search_textarea, i18n_get(STR_EVENT_SEARCH));
    lv_obj_set_user_data(state.search_textarea, (void*)"event_search_input");
    lv_obj_set_style_text_font(state.search_textarea, &lv_font_montserrat_32, LV_PART_MAIN);

    lv_obj_t* keyboard = lv_keyboard_create(state.search_overlay);
    lv_obj_set_size(keyboard, LV_PCT(100), LV_PCT(25));
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, -25);
    lv_obj_set_style_bg_color(keyboard, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_text_font(keyboard, UI_FONT_BODY, LV_PART_ITEMS);
    lv_obj_set_user_data(keyboard, (void*)"event_search_keyboard");
    lv_keyboard_set_textarea(keyboard, state.search_textarea);
    lv_obj_add_event_cb(keyboard, search_apply_cb, LV_EVENT_READY, nullptr);
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER, kb_map_lc, kb_ctrl_lc);
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_UPPER, kb_map_uc, kb_ctrl_uc);
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_SPECIAL, kb_map_spec, kb_ctrl_spec);
    lv_keyboard_set_popovers(keyboard, true);

    lv_obj_t* action_bar = lv_obj_create(state.search_overlay);
    lv_obj_set_size(action_bar, LV_PCT(100), 110);
    lv_obj_align_to(action_bar, keyboard, LV_ALIGN_OUT_TOP_MID, 0, -8);
    lv_obj_set_style_bg_color(action_bar, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(action_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(action_bar, 30, LV_PART_MAIN);
    lv_obj_clear_flag(action_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* cancel = create_text_button(
        action_bar, i18n_get(STR_CANCEL), "event_search_cancel", search_cancel_cb);
    lv_obj_set_size(cancel, 300, 80);
    lv_obj_align(cancel, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t* apply = create_text_button(
        action_bar, i18n_get(STR_EVENT_APPLY_SEARCH),
        "event_search_apply", search_apply_cb);
    lv_obj_set_size(apply, 300, 80);
    lv_obj_align(apply, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(apply, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_obj_get_child(apply, 0), COLOR_BG, LV_PART_MAIN);

    lv_obj_move_foreground(keyboard);
    lv_obj_add_flag(state.search_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void create_filter_overlay() {
    state.filter_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(state.filter_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_center(state.filter_overlay);
    lv_obj_set_style_bg_color(state.filter_overlay, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state.filter_overlay, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.filter_overlay, 0, LV_PART_MAIN);
    lv_obj_clear_flag(state.filter_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* card = lv_obj_create(state.filter_overlay);
    lv_obj_set_size(card, 720, 600);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, COLOR_CARD_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 24, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, i18n_get(STR_EVENT_FILTERS));
    lv_obj_set_style_text_font(title, &montserrat_32_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t* close = create_text_button(
        card, LV_SYMBOL_CLOSE, "event_filters_cancel", filters_cancel_cb);
    lv_obj_set_size(close, 55, 55);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, 0, -8);

    lv_obj_t* options = lv_obj_create(card);
    lv_obj_set_size(options, LV_PCT(100), 380);
    lv_obj_align(options, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_bg_opa(options, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(options, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(options, 0, LV_PART_MAIN);
    lv_obj_set_layout(options, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(options, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(options, 10, LV_PART_MAIN);
    lv_obj_clear_flag(options, LV_OBJ_FLAG_SCROLLABLE);

    const string_id_t time_labels[EVENT_TIME_COUNT] = {
        STR_EVENT_TIME_ALL, STR_EVENT_TIME_TODAY, STR_EVENT_TIME_24_HOURS,
        STR_EVENT_TIME_7_DAYS, STR_EVENT_TIME_SINCE_RESTART
    };
    const char* time_tags[EVENT_TIME_COUNT] = {
        "event_time_all", "event_time_today", "event_time_24h",
        "event_time_7d", "event_time_restart"
    };
    for (int i = 0; i < EVENT_TIME_COUNT; i++) {
        lv_obj_t* btn = create_text_button(
            options, i18n_get(time_labels[i]), time_tags[i], nullptr);
        lv_obj_set_size(btn, LV_PCT(100), 66);
        style_toggle_button(btn);
        lv_obj_add_event_cb(
            btn, time_filter_btn_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)i);
        state.time_btn[i] = btn;
    }

    lv_obj_t* reset = create_text_button(
        card, i18n_get(STR_EVENT_RESET_FILTERS),
        "event_filters_reset", filters_reset_cb);
    lv_obj_set_size(reset, 300, 70);
    lv_obj_align(reset, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t* apply = create_text_button(
        card, i18n_get(STR_EVENT_APPLY_FILTERS),
        "event_filters_apply", filters_apply_cb);
    lv_obj_set_size(apply, 300, 70);
    lv_obj_align(apply, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(apply, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_obj_get_child(apply, 0), COLOR_BG, LV_PART_MAIN);

    lv_obj_add_flag(state.filter_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void close_overlays() {
    if (state.search_overlay) {
        lv_obj_del(state.search_overlay);
        state.search_overlay = nullptr;
        state.search_textarea = nullptr;
    }
    if (state.filter_overlay) {
        lv_obj_del(state.filter_overlay);
        state.filter_overlay = nullptr;
        for (lv_obj_t*& btn : state.time_btn) btn = nullptr;
    }
}

static void reset_screen_refs() {
    state.controls = nullptr;
    state.search_label = nullptr;
    state.search_clear_btn = nullptr;
    state.filters_label = nullptr;
    state.results_label = nullptr;
    state.new_events_btn = nullptr;
    for (lv_obj_t*& btn : state.category_btn) btn = nullptr;
}

// ============================================================================
// Public Functions
// ============================================================================

void event_log_screen_create_in(lv_obj_t* parent) {
    if (state.shown) {
        return;
    }

    ESP_LOGI(TAG, "Building event log tab");
    state.on_close = nullptr;

    // The panel provided by the tab shell is our root; build directly into it.
    state.screen = parent;
    state.count_label = nullptr;  // no per-tab title header in the tab shell

    create_filter_controls();

    // The filter bar stays pinned while only the event list scrolls.
    state.content = lv_obj_create(state.screen);
    lv_obj_set_user_data(state.content, (void*)"event_log_content");
    lv_obj_update_layout(state.screen);
    int32_t content_h = lv_obj_get_height(state.screen) - FILTER_BAR_H;
    lv_obj_set_size(state.content, LV_PCT(100), content_h);
    lv_obj_align(state.content, LV_ALIGN_TOP_MID, 0, FILTER_BAR_H);
    lv_obj_set_style_bg_opa(state.content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.content, 15, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(state.content, NAV_BAR_H + 15, LV_PART_MAIN);
    lv_obj_set_flex_flow(state.content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(state.content, 8, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(state.content, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(state.content, content_scroll_cb, LV_EVENT_SCROLL, nullptr);

    create_search_overlay();
    create_filter_overlay();
    
    state.shown = true;
    state.last_count = -1;  // Force initial build
    
    // Build the event list
    rebuild_event_list();
    
    // Update timer (check for new events every 2 seconds)
    state.update_timer = lv_timer_create(update_timer_cb, 2000, nullptr);
}

void event_log_screen_set_active(bool active) {
    if (state.update_timer) {
        if (active) {
            lv_timer_resume(state.update_timer);
        } else {
            lv_timer_pause(state.update_timer);
        }
    }
    if (!active) {
        if (state.search_overlay) lv_obj_add_flag(state.search_overlay, LV_OBJ_FLAG_HIDDEN);
        if (state.filter_overlay) lv_obj_add_flag(state.filter_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    if (active) {
        state.last_count = -1;   // force a fresh rebuild on activation
        rebuild_event_list();
    }
}

void event_log_screen_dismiss_overlays(void) {
    close_overlays();
}

void event_log_screen_hide(void) {
    if (!state.shown) {
        return;
    }
    
    ESP_LOGI(TAG, "Hiding event log screen");
    
    if (state.update_timer) {
        lv_timer_del(state.update_timer);
        state.update_timer = nullptr;
    }
    
    event_log_screen_close_cb_t cb = state.on_close;

    close_overlays();
    
    state.shown = false;
    state.on_close = nullptr;
    state.screen = nullptr;
    state.content = nullptr;
    state.count_label = nullptr;
    state.last_count = -1;
    state.footer = nullptr;
    state.loading = false;
    state.displayed = 0;
    reset_screen_refs();
    
    // Call callback to restore previous screen
    if (cb) {
        cb();
    }
}

bool event_log_screen_is_shown(void) {
    return state.shown;
}
