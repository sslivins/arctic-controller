/*
 * Arctic Heat Pump Controller
 * Event Log Screen Implementation
 * 
 * Full-screen scrollable list of recent operational events, newest first.
 */

#include "event_log_screen.h"
#include "event_log.h"
#include "heatpump_errors.h"
#include "ui_common.h"
#include "fonts/fonts.h"
#include "i18n/i18n.h"
#include <esp_log.h>
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
        snprintf(buf, buf_size, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        // Use uptime
        uint32_t sec = evt->uptime_ms / 1000;
        uint32_t min = sec / 60;
        uint32_t hr = min / 60;
        snprintf(buf, buf_size, "+%luh%02lum%02lus", (unsigned long)hr, (unsigned long)(min % 60), (unsigned long)(sec % 60));
    }
}

// Forward declarations
static void clear_btn_cb(lv_event_t* e);

// ============================================================================
// Build / Rebuild Event List
// ============================================================================

static void rebuild_event_list() {
    if (!state.content) return;
    
    // Remove all children from content
    lv_obj_clean(state.content);
    
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
        return;
    }
    
    // Get all events (newest first)
    event_entry_t events[EVENT_LOG_MAX_ENTRIES];
    int got = event_log_get(events, EVENT_LOG_MAX_ENTRIES, 0);
    
    char detail_buf[128];
    char time_buf[32];
    
    for (int i = 0; i < got; i++) {
        const event_entry_t* evt = &events[i];
        
        // Row container - flex column layout like error cards
        lv_obj_t* row = lv_obj_create(state.content);
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
    
    state.last_count = count;
}

// ============================================================================
// Timer callback - only rebuild if count changed
// ============================================================================

static void update_timer_cb(lv_timer_t* timer) {
    if (!state.shown) return;
    int count = event_log_count();
    if (count != state.last_count) {
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

void event_log_screen_show(event_log_screen_close_cb_t on_close) {
    if (state.shown) {
        return;
    }
    
    ESP_LOGI(TAG, "Showing event log screen");
    state.on_close = on_close;
    
    // Create screen
    state.screen = lv_obj_create(NULL);
    lv_obj_set_size(state.screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(state.screen, COLOR_BG, LV_PART_MAIN);
    lv_obj_clear_flag(state.screen, LV_OBJ_FLAG_SCROLLABLE);
    
    // Header (8% height)
    lv_obj_t* header = lv_obj_create(state.screen);
    lv_obj_set_size(header, LV_PCT(100), LV_PCT(8));
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, COLOR_HEADER_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(header, 15, LV_PART_MAIN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    
    // Close button (X on right) with circular background
    lv_obj_t* back_btn = lv_btn_create(header);
    lv_obj_set_size(back_btn, 50, 50);
    lv_obj_align(back_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x3d4f6f), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(back_btn, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(back_btn, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_border_opa(back_btn, LV_OPA_50, LV_PART_MAIN);
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_user_data(back_btn, (void*)"event_log_close");
    
    lv_obj_t* back_icon = lv_label_create(back_btn);
    lv_label_set_text(back_icon, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(back_icon, &montserrat_32_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(back_icon, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_center(back_icon);
    
    // Title
    state.count_label = lv_label_create(header);
    char title_buf[64];
    snprintf(title_buf, sizeof(title_buf), "%s (%d)", i18n_get(STR_EVENT_LOG), event_log_count());
    lv_label_set_text(state.count_label, title_buf);
    lv_obj_set_style_text_color(state.count_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(state.count_label, UI_FONT_HEADER, LV_PART_MAIN);
    lv_obj_align(state.count_label, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_user_data(state.count_label, (void*)"event_log_title");
    
    // Scrollable content
    state.content = lv_obj_create(state.screen);
    lv_obj_set_size(state.content, LV_PCT(100), LV_PCT(92));
    lv_obj_align(state.content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(state.content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.content, 15, LV_PART_MAIN);
    lv_obj_set_flex_flow(state.content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(state.content, 8, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(state.content, LV_SCROLLBAR_MODE_AUTO);
    
    state.shown = true;
    state.last_count = -1;  // Force initial build
    
    // Build the event list
    rebuild_event_list();
    
    // Update timer (check for new events every 2 seconds)
    state.update_timer = lv_timer_create(update_timer_cb, 2000, nullptr);
    
    // Load with fade animation
    lv_screen_load_anim(state.screen, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, false);
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
    
    // Call callback to restore previous screen
    if (cb) {
        cb();
    }
}

bool event_log_screen_is_shown(void) {
    return state.shown;
}
