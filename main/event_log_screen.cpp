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
#include <esp_log.h>
#include <limits.h>
#include <stdio.h>
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
static struct {
    bool shown = false;
    lv_obj_t* screen = nullptr;
    event_log_screen_close_cb_t on_close = nullptr;
    lv_timer_t* update_timer = nullptr;
    lv_obj_t* content = nullptr;
    lv_obj_t* count_label = nullptr;
    int last_count = -1;    // Force rebuild on first update
    int displayed = 0;      // Rows currently rendered (for lazy "load more")
    lv_obj_t* footer = nullptr;    // Dim "loading older events" footer / scroll anchor
    bool loading = false;   // Re-entrancy guard while appending a batch
    int last_date_key = INT_MIN;
    int today_key = INT_MIN;
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
        default:
            buf[0] = '\0';
            break;
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
        uint32_t sec = evt->uptime_ms / 1000;
        uint32_t min = sec / 60;
        uint32_t hr = min / 60;
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

    int count = event_log_count();
    int to_fetch = count - state.displayed;
    if (to_fetch > EVENT_BATCH_SIZE) to_fetch = EVENT_BATCH_SIZE;
    if (to_fetch <= 0) {
        update_footer(0);
        return;
    }

    state.loading = true;

    event_entry_t events[EVENT_BATCH_SIZE];
    int got = event_log_get(events, to_fetch, state.displayed);

    // Batch-render with layout suspended to avoid O(n²) recalc thrash.
    int32_t scroll_y = lv_obj_get_scroll_y(state.content);
    lv_obj_add_flag(state.content, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < got; i++) {
        create_event_row(state.content, &events[i]);
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
    if (state.displayed >= event_log_count()) return;
    if (lv_obj_get_scroll_bottom(state.content) <= EVENT_SCROLL_THRESHOLD_PX) {
        append_next_batch();
    }
}

static void rebuild_event_list() {
    if (!state.content) return;
    
    // Remove all children from content
    lv_obj_clean(state.content);
    state.footer = nullptr;   // destroyed by lv_obj_clean above
    state.loading = false;
    state.displayed = 0;
    state.last_date_key = INT_MIN;
    state.today_key = local_date_key(time(nullptr));

    // Disable scrolling/layout during batch creation to avoid O(n²) recalc
    lv_obj_add_flag(state.content, LV_OBJ_FLAG_HIDDEN);
    
    // Clear History button row (inline, matching errors screen style)
    lv_obj_t* clear_row = lv_obj_create(state.content);
    lv_obj_set_size(clear_row, LV_PCT(100), 55);
    lv_obj_set_style_bg_opa(clear_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(clear_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(clear_row, 0, LV_PART_MAIN);
    lv_obj_set_layout(clear_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(clear_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(clear_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(clear_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* clear_btn = lv_btn_create(clear_row);
    lv_obj_set_size(clear_btn, LV_SIZE_CONTENT, 50);
    lv_obj_set_style_min_width(clear_btn, 100, LV_PART_MAIN);
    lv_obj_set_style_bg_color(clear_btn, lv_color_hex(0x3d4f6f), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(clear_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(clear_btn, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(clear_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(clear_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(clear_btn, COLOR_WARNING, LV_PART_MAIN);
    lv_obj_set_style_border_opa(clear_btn, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(clear_btn, 25, LV_PART_MAIN);
    lv_obj_add_event_cb(clear_btn, clear_btn_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_user_data(clear_btn, (void*)"event_log_clear");

    lv_obj_t* clear_label = lv_label_create(clear_btn);
    lv_label_set_text(clear_label, i18n_get(STR_HP_CLEAR_HISTORY));
    lv_obj_set_style_text_font(clear_label, UI_FONT_BODY, LV_PART_MAIN);
    lv_obj_set_style_text_color(clear_label, COLOR_WARNING, LV_PART_MAIN);
    lv_obj_center(clear_label);

    int count = event_log_count();
    
    // Update count label in header
    if (state.count_label) {
        char title_buf[64];
        snprintf(title_buf, sizeof(title_buf), "%s (%d)", i18n_get(STR_EVENT_LOG), count);
        lv_label_set_text(state.count_label, title_buf);
    }
    
    if (count == 0) {
        // Show "no events" message
        lv_obj_t* msg = lv_label_create(state.content);
        lv_label_set_text(msg, i18n_get(STR_EVENT_NO_EVENTS));
        lv_obj_set_user_data(msg, (void*)"event_log_empty");
        lv_obj_set_style_text_color(msg, COLOR_TEXT_DIM, LV_PART_MAIN);
        lv_obj_set_style_text_font(msg, &montserrat_24_latin, LV_PART_MAIN);
        lv_obj_set_width(msg, LV_PCT(100));
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_pad_top(msg, 40, LV_PART_MAIN);

        lv_obj_clear_flag(state.content, LV_OBJ_FLAG_HIDDEN);
        state.last_count = count;
        return;
    }
    
    // Reset lazy-load state; the footer will be recreated below if needed.
    state.footer = nullptr;
    state.loading = false;
    state.displayed = 0;

    // Render the first batch only; older events load on scroll (infinite scroll).
    int first_batch = count < EVENT_BATCH_SIZE ? count : EVENT_BATCH_SIZE;
    event_entry_t events[EVENT_BATCH_SIZE];
    int got = event_log_get(events, first_batch, 0);

    for (int i = 0; i < got; i++) {
        create_event_row(state.content, &events[i]);
    }
    state.displayed = got;

    // Dim footer hints at the remaining (up to 128) older events that load on scroll.
    update_footer(count - state.displayed);

    // Re-enable visibility and force layout calculation once
    lv_obj_clear_flag(state.content, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(state.content);

    state.last_count = count;

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
    if (!state.shown) return;
    int count = event_log_count();
    int today_key = local_date_key(time(nullptr));
    if (count != state.last_count || today_key != state.today_key) {
        rebuild_event_list();
    }
}

// ============================================================================
// Callbacks
// ============================================================================

static void back_btn_cb(lv_event_t* e) {
    (void)e;
    ESP_LOGI(TAG, "Back button clicked");
    
    // Stop timer first to prevent use-after-free
    if (state.update_timer) {
        lv_timer_del(state.update_timer);
        state.update_timer = nullptr;
    }
    
    // Save and clear callback
    event_log_screen_close_cb_t cb = state.on_close;
    state.on_close = nullptr;
    state.shown = false;
    state.screen = nullptr;
    state.content = nullptr;
    state.count_label = nullptr;
    state.last_count = -1;
    state.footer = nullptr;
    state.loading = false;
    state.displayed = 0;
    
    // Call callback - it will load the previous screen with auto_del=true
    if (cb) {
        cb();
    }
}

static void clear_btn_cb(lv_event_t* e) {
    (void)e;
    ESP_LOGI(TAG, "Clear events");
    event_log_clear();
    rebuild_event_list();
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

    // Scrollable content fills the panel; reserve room for the persistent nav
    // bar (drawn by the tab shell) at the bottom.
    state.content = lv_obj_create(state.screen);
    lv_obj_set_size(state.content, LV_PCT(100), LV_PCT(100));
    lv_obj_align(state.content, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(state.content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.content, 15, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(state.content, NAV_BAR_H + 15, LV_PART_MAIN);
    lv_obj_set_flex_flow(state.content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(state.content, 8, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(state.content, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(state.content, content_scroll_cb, LV_EVENT_SCROLL, nullptr);
    
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
    if (active) {
        state.last_count = -1;   // force a fresh rebuild on activation
        rebuild_event_list();
    }
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
    
    state.shown = false;
    state.on_close = nullptr;
    state.screen = nullptr;
    state.content = nullptr;
    state.count_label = nullptr;
    state.last_count = -1;
    state.footer = nullptr;
    state.loading = false;
    state.displayed = 0;
    
    // Call callback to restore previous screen
    if (cb) {
        cb();
    }
}

bool event_log_screen_is_shown(void) {
    return state.shown;
}
