/*
 * Test Instrumentation Endpoints
 * 
 * GET  /api/test/ui-state  — Walk the LVGL widget tree and return visible widgets
 * POST /api/test/click     — Find a widget by label text and click it
 *
 * All LVGL access is protected by bsp_display_lock/unlock.
 */

#include "sdkconfig.h"

#ifdef CONFIG_TEST_ENDPOINTS

#include "test_endpoints.h"
#include <cstring>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <cJSON.h>
#include <lvgl.h>
#include <bsp/m5stack_tab5.h>
#include "settings/settings_menu.h"
#include "settings/settings_display_screen.h"
#include "settings/settings_wifi_screen.h"
#include "settings/settings_firmware_screen.h"
#include "settings/settings_time_screen.h"
#include "settings/settings_language_screen.h"
#include "settings/settings_types.h"
#include "i18n/i18n.h"
#include "heatpump_controller.h"
#include "app_preferences.h"
#include "heatpump_errors.h"
#include "heatpump_temps_screen.h"
#include "heatpump_control_screen.h"
#include "heatpump_errors_screen.h"
#include "event_log.h"
#include "event_log_screen.h"
#include "tab_shell.h"
#include "status_bar.h"
#include "display_idle.h"
#include <esp_timer.h>
#include "png_encoder.h"

static const char* TAG = "test_api";

// ============================================================================
// Session Lock State — prevents concurrent test sessions on the device
// ============================================================================

static char s_lock_session_id[64] = {0};       // Current lock holder (empty = unlocked)
static int64_t s_lock_acquired_us = 0;          // Timestamp when lock was acquired
static int64_t s_lock_ttl_us = 15 * 60 * 1000000LL;  // Default TTL: 15 minutes

// ============================================================================
// Helpers
// ============================================================================

static void set_json_content_type(httpd_req_t* req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

static void send_json_error(httpd_req_t* req, const char* status, const char* message)
{
    httpd_resp_set_status(req, status);
    set_json_content_type(req);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", message);
    char* json = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
}

// ============================================================================
// Session Lock Helpers
// ============================================================================

static bool is_lock_held(void)
{
    if (s_lock_session_id[0] == '\0') return false;
    int64_t elapsed = esp_timer_get_time() - s_lock_acquired_us;
    if (elapsed > s_lock_ttl_us) {
        ESP_LOGW(TAG, "Session lock expired (held by '%s' for %lds)", s_lock_session_id, (long)(elapsed / 1000000LL));
        s_lock_session_id[0] = '\0';
        return false;
    }
    return true;
}

static bool check_session_lock(httpd_req_t* req)
{
    if (!is_lock_held()) return true;
    char session_id[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Session-Id", session_id, sizeof(session_id)) == ESP_OK) {
        if (strcmp(session_id, s_lock_session_id) == 0) return true;
    }
    return false;
}

static void reject_locked(httpd_req_t* req)
{
    httpd_resp_set_status(req, "423 Locked");
    set_json_content_type(req);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", "Device is locked by another test session");
    cJSON_AddStringToObject(root, "locked_by", s_lock_session_id);
    int64_t remaining_s = (s_lock_ttl_us - (esp_timer_get_time() - s_lock_acquired_us)) / 1000000LL;
    cJSON_AddNumberToObject(root, "remaining_seconds", remaining_s > 0 ? remaining_s : 0);
    char* json = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
}

// Macro: reject request if device is locked by a different session
#define CHECK_SESSION_LOCK(req) do { \
    if (!check_session_lock(req)) { reject_locked(req); return ESP_OK; } \
} while(0)

// Get a human-readable type name for an LVGL object
static const char* get_widget_type(lv_obj_t* obj)
{
    if (lv_obj_check_type(obj, &lv_label_class))       return "label";
    if (lv_obj_check_type(obj, &lv_button_class))      return "button";
    if (lv_obj_check_type(obj, &lv_switch_class))      return "switch";
    if (lv_obj_check_type(obj, &lv_checkbox_class))     return "checkbox";
    if (lv_obj_check_type(obj, &lv_slider_class))       return "slider";
    if (lv_obj_check_type(obj, &lv_dropdown_class))     return "dropdown";
    if (lv_obj_check_type(obj, &lv_roller_class))        return "roller";
    if (lv_obj_check_type(obj, &lv_textarea_class))     return "textarea";
    if (lv_obj_check_type(obj, &lv_image_class))        return "image";
    if (lv_obj_check_type(obj, &lv_bar_class))          return "bar";
    if (lv_obj_check_type(obj, &lv_spinner_class))      return "spinner";
    if (lv_obj_check_type(obj, &lv_obj_class))          return "container";
    return "unknown";
}

// Get the text content of a widget (if it has any)
static const char* get_widget_text(lv_obj_t* obj)
{
    if (lv_obj_check_type(obj, &lv_label_class)) {
        return lv_label_get_text(obj);
    }
    if (lv_obj_check_type(obj, &lv_checkbox_class)) {
        return lv_checkbox_get_text(obj);
    }
    if (lv_obj_check_type(obj, &lv_textarea_class)) {
        return lv_textarea_get_text(obj);
    }
    return NULL;
}

// Check if user_data looks like a valid tag string (printable ASCII, reasonable length).
// LVGL user_data is void* — it could be a tag string or anything else (struct ptr, number).
// Only treat it as a string if it points to valid memory and the first N bytes are printable ASCII.
static bool is_valid_tag(const void* ud)
{
    if (!ud) return false;
    // Reject small integers masquerading as pointers (e.g. user_data = (void*)1)
    // Valid pointers on ESP32-P4 are in IRAM (0x4ff...), flash (0x480...) or PSRAM (0x485...)
    uintptr_t addr = (uintptr_t)ud;
    if (addr < 0x40000000) return false;  // clearly not a valid pointer
    const unsigned char* p = (const unsigned char*)ud;
    // Check first 4 bytes: must be printable ASCII (0x20-0x7E) or underscore-style identifiers
    for (int i = 0; i < 4 && p[i] != '\0'; i++) {
        unsigned char c = p[i];
        if (c < 0x20 || c > 0x7E) return false;  // non-printable or high byte → not a tag
    }
    // Must have at least 1 character
    return p[0] >= 0x20 && p[0] <= 0x7E;
}

// Helper: JSON-escape a string into buf (handles \, ", control chars)
static int json_escape_into(char* buf, int max, const char* s)
{
    int p = 0;
    for (; *s && p < max - 2; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"') { if (p + 2 > max - 1) break; buf[p++] = '\\'; buf[p++] = '"'; }
        else if (c == '\\') { if (p + 2 > max - 1) break; buf[p++] = '\\'; buf[p++] = '\\'; }
        else if (c == '\n') { if (p + 2 > max - 1) break; buf[p++] = '\\'; buf[p++] = 'n'; }
        else if (c == '\r') { if (p + 2 > max - 1) break; buf[p++] = '\\'; buf[p++] = 'r'; }
        else if (c < 0x20) { continue; }  // skip other control chars
        else { buf[p++] = c; }
    }
    buf[p] = '\0';
    return p;
}

// Serialize a single widget into the JSON buffer using snprintf (no cJSON, no heap alloc).
// Returns true if the widget was added, false if buffer full or not interesting.
static bool serialize_widget(lv_obj_t* obj, char* buf, int* pos, int buf_size, int* widget_count)
{
    if (*pos >= buf_size - 1024 || *widget_count >= 300) return false;

    bool hidden = lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    if (hidden) return false;

    const char* type = get_widget_type(obj);
    const char* text = get_widget_text(obj);
    bool has_tag = is_valid_tag(lv_obj_get_user_data(obj));

    bool is_interesting = has_tag || (text != NULL) ||
                          strcmp(type, "button") == 0 || strcmp(type, "switch") == 0 ||
                          strcmp(type, "checkbox") == 0 || strcmp(type, "slider") == 0 ||
                          strcmp(type, "dropdown") == 0 || strcmp(type, "roller") == 0 ||
                          strcmp(type, "textarea") == 0;
    if (!is_interesting) return false;

    // Temporary buffer on stack for building this widget's JSON
    char tmp[512];
    int tp = 0;
    int rem = sizeof(tmp);

    #define TPRINTF(...) do { int n = snprintf(tmp + tp, rem, __VA_ARGS__); if (n > 0 && n < rem) { tp += n; rem -= n; } } while(0)

    TPRINTF("{\"type\":\"%s\"", type);

    if (text) {
        char escaped[256];
        json_escape_into(escaped, sizeof(escaped), text);
        TPRINTF(",\"text\":\"%s\"", escaped);
        const char* en = i18n_get_english(text);
        if (en && strcmp(en, text) != 0) {
            json_escape_into(escaped, sizeof(escaped), en);
            TPRINTF(",\"text_en\":\"%s\"", escaped);
        }
    }

    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    TPRINTF(",\"x\":%ld,\"y\":%ld,\"w\":%ld,\"h\":%ld",
            (long)coords.x1, (long)coords.y1,
            (long)lv_area_get_width(&coords), (long)lv_area_get_height(&coords));

    if (lv_obj_check_type(obj, &lv_switch_class) || lv_obj_check_type(obj, &lv_checkbox_class)) {
        TPRINTF(",\"checked\":%s", lv_obj_has_state(obj, LV_STATE_CHECKED) ? "true" : "false");
    }
    if (lv_obj_check_type(obj, &lv_slider_class)) {
        TPRINTF(",\"value\":%ld,\"min\":%ld,\"max\":%ld",
                (long)lv_slider_get_value(obj),
                (long)lv_slider_get_min_value(obj),
                (long)lv_slider_get_max_value(obj));
    }
    if (lv_obj_check_type(obj, &lv_textarea_class)) {
        TPRINTF(",\"password_mode\":%s", lv_textarea_get_password_mode(obj) ? "true" : "false");
    }
    if (lv_obj_check_type(obj, &lv_roller_class)) {
        char sel_text[64] = {0};
        lv_roller_get_selected_str(obj, sel_text, sizeof(sel_text));
        char escaped[128];
        json_escape_into(escaped, sizeof(escaped), sel_text);
        TPRINTF(",\"value\":%lu,\"option_count\":%lu,\"selected_text\":\"%s\"",
                (unsigned long)lv_roller_get_selected(obj),
                (unsigned long)lv_roller_get_option_count(obj), escaped);
    }
    if (lv_obj_has_state(obj, LV_STATE_DISABLED)) {
        TPRINTF(",\"disabled\":true");
    }

    void* ud = lv_obj_get_user_data(obj);
    if (is_valid_tag(ud)) {
        char escaped[128];
        json_escape_into(escaped, sizeof(escaped), (const char*)ud);
        TPRINTF(",\"tag\":\"%s\"", escaped);
        lv_color_t bg = lv_obj_get_style_bg_color(obj, LV_PART_MAIN);
        TPRINTF(",\"bg_color\":\"#%02x%02x%02x\"", bg.red, bg.green, bg.blue);
    }

    TPRINTF("}");
    #undef TPRINTF

    // Append to output buffer
    if (*pos + tp + 2 < buf_size - 512) {
        if (*widget_count > 0) buf[(*pos)++] = ',';
        memcpy(buf + *pos, tmp, tp);
        *pos += tp;
        (*widget_count)++;
        return true;
    }
    return false;
}

// Iteratively walk LVGL object tree using an explicit stack (avoids thread stack overflow).
// The stack is allocated from PSRAM to avoid using precious internal RAM.
static void walk_tree_iterative(lv_obj_t* root, char* buf, int* pos, int buf_size, int* widget_count)
{
    if (!root) return;

    // Explicit traversal stack in PSRAM — supports up to 512 pending nodes
    static const int MAX_STACK = 512;
    lv_obj_t** stack = (lv_obj_t**)heap_caps_malloc(MAX_STACK * sizeof(lv_obj_t*), MALLOC_CAP_SPIRAM);
    if (!stack) return;

    int top = 0;
    int node_num = 0;
    stack[top++] = root;

    while (top > 0 && *widget_count < 300 && *pos < buf_size - 1024) {
        lv_obj_t* obj = stack[--top];
        if (!obj) continue;

        // Skip hidden subtrees entirely
        if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) continue;

        node_num++;

        // Try to serialize this widget (only emits "interesting" ones)
        serialize_widget(obj, buf, pos, buf_size, widget_count);

        // Push children in reverse order so first child is processed first
        uint32_t count = lv_obj_get_child_count(obj);
        for (int i = (int)count - 1; i >= 0 && top < MAX_STACK; i--) {
            stack[top++] = lv_obj_get_child(obj, (uint32_t)i);
        }
    }

    heap_caps_free(stack);
}

// Find a label widget whose text matches (exact or contains)
// Returns the label itself, not its parent
static lv_obj_t* find_label_by_text(lv_obj_t* root, const char* exact, const char* contains, int depth)
{
    if (!root || depth > 10) return NULL;

    if (!lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) {
        const char* text = get_widget_text(root);
        if (text) {
            if (exact && strcmp(text, exact) == 0) return root;
            if (contains && strstr(text, contains) != NULL) return root;
        }

        uint32_t count = lv_obj_get_child_count(root);
        for (uint32_t i = 0; i < count; i++) {
            lv_obj_t* found = find_label_by_text(lv_obj_get_child(root, i), exact, contains, depth + 1);
            if (found) return found;
        }
    }
    return NULL;
}

// Find a widget by its user_data tag string
static lv_obj_t* find_by_tag(lv_obj_t* root, const char* tag, int depth)
{
    if (!root || depth > 10) return NULL;

    if (!lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) {
        void* ud = lv_obj_get_user_data(root);
        if (is_valid_tag(ud) && strcmp((const char*)ud, tag) == 0) {
            return root;
        }
        uint32_t count = lv_obj_get_child_count(root);
        for (uint32_t i = 0; i < count; i++) {
            lv_obj_t* found = find_by_tag(lv_obj_get_child(root, i), tag, depth + 1);
            if (found) return found;
        }
    }
    return NULL;
}

// Find the nearest clickable ancestor of a widget
static lv_obj_t* find_clickable_parent(lv_obj_t* obj)
{
    while (obj) {
        if (lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE)) {
            return obj;
        }
        obj = lv_obj_get_parent(obj);
    }
    return NULL;
}

// Determine what screen is currently showing
static const char* get_screen_name(void)
{
    // Settings sub-screens and the errors screen are overlays drawn on top of
    // the persistent tab shell, so they take precedence when visible.
    if (display_screen_is_visible()) return "display";
    if (wifi_screen_is_visible()) return "wifi";
    if (firmware_screen_is_visible()) return "firmware";
    if (time_screen_is_visible()) return "time";
    if (language_screen_is_visible()) return "language";
    if (settings_menu_is_visible()) return "settings";
    if (heatpump_errors_is_shown()) return "errors";

    // Otherwise the tab shell is showing. All four tab panels are persistent
    // (their individual _is_shown() flags are permanently true), so the visible
    // screen is determined by the active tab, not by which panels exist.
    switch (tab_shell_current()) {
        case NAV_TAB_HOME:    return "main";
        case NAV_TAB_STATUS:  return "status";
        case NAV_TAB_CONTROL: return "control";
        case NAV_TAB_EVENTS:  return "event_log";
        default:              return "main";
    }
}

// ============================================================================
// GET /api/test/ui-state
// Uses a pre-allocated PSRAM buffer to avoid heap fragmentation on complex screens
// ============================================================================

static esp_err_t ui_state_get_handler(httpd_req_t* req)
{
    set_json_content_type(req);

    // Allocate response buffer from PSRAM (64KB) to avoid internal RAM fragmentation
    static const int BUF_SIZE = 65536;
    char* buf = (char*)heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGW(TAG, "ui-state: PSRAM alloc failed, trying malloc");
        buf = (char*)malloc(BUF_SIZE);
    }
    if (!buf) {
        ESP_LOGE(TAG, "ui-state: all allocation failed!");
        send_json_error(req, "500 Internal Server Error", "Out of memory for ui-state buffer");
        return ESP_OK;
    }

    if (!bsp_display_lock(1000)) {
        heap_caps_free(buf);
        ESP_LOGW(TAG, "ui-state: display lock timeout!");
        send_json_error(req, "503 Service Unavailable", "Could not acquire display lock");
        return ESP_OK;
    }

    const char* screen_name = get_screen_name();

    int pos = 0;
    pos += snprintf(buf + pos, BUF_SIZE - pos, "{\"screen\":\"%s\",\"widgets\":[", screen_name);

    int widget_count = 0;
    lv_obj_t* scr = lv_scr_act();
    walk_tree_iterative(scr, buf, &pos, BUF_SIZE, &widget_count);

    bsp_display_unlock();

    pos += snprintf(buf + pos, BUF_SIZE - pos, "],\"count\":%d}", widget_count);
    buf[pos] = '\0';

    httpd_resp_sendstr(req, buf);
    ESP_LOGI(TAG, "ui-state: screen=%s, %d widgets, %d bytes", screen_name, widget_count, pos);
    heap_caps_free(buf);
    return ESP_OK;
}

// ============================================================================
// GET /api/test/screen
// Lightweight endpoint returning only the screen name (no widget tree walk)
// ============================================================================

static esp_err_t screen_get_handler(httpd_req_t* req)
{
    set_json_content_type(req);

    if (!bsp_display_lock(1000)) {
        send_json_error(req, "503 Service Unavailable", "Could not acquire display lock");
        return ESP_OK;
    }

    const char* screen_name = get_screen_name();

    bsp_display_unlock();

    char buf[128];
    snprintf(buf, sizeof(buf), "{\"screen\":\"%s\"}", screen_name);
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

// ============================================================================
// POST /api/test/click
//
// Body: {"label": "exact text"}        — match by exact label text (auto-translated via i18n)
//       {"label_contains": "partial"}  — match by substring (auto-translated via i18n)
//       {"symbol": "SETTINGS"}         — match by LVGL symbol name
// ============================================================================

// Map symbol name strings to LVGL symbol UTF-8 sequences
static const char* resolve_symbol(const char* name)
{
    struct { const char* name; const char* sym; } symbols[] = {
        {"AUDIO",        LV_SYMBOL_AUDIO},
        {"VIDEO",        LV_SYMBOL_VIDEO},
        {"LIST",         LV_SYMBOL_LIST},
        {"OK",           LV_SYMBOL_OK},
        {"CLOSE",        LV_SYMBOL_CLOSE},
        {"POWER",        LV_SYMBOL_POWER},
        {"SETTINGS",     LV_SYMBOL_SETTINGS},
        {"HOME",         LV_SYMBOL_HOME},
        {"DOWNLOAD",     LV_SYMBOL_DOWNLOAD},
        {"REFRESH",      LV_SYMBOL_REFRESH},
        {"EDIT",         LV_SYMBOL_EDIT},
        {"PREV",         LV_SYMBOL_PREV},
        {"NEXT",         LV_SYMBOL_NEXT},
        {"PLAY",         LV_SYMBOL_PLAY},
        {"PAUSE",        LV_SYMBOL_PAUSE},
        {"STOP",         LV_SYMBOL_STOP},
        {"IMAGE",        LV_SYMBOL_IMAGE},
        {"TINT",         LV_SYMBOL_TINT},
        {"PLUS",         LV_SYMBOL_PLUS},
        {"MINUS",        LV_SYMBOL_MINUS},
        {"WARNING",      LV_SYMBOL_WARNING},
        {"SHUFFLE",      LV_SYMBOL_SHUFFLE},
        {"UP",           LV_SYMBOL_UP},
        {"DOWN",         LV_SYMBOL_DOWN},
        {"LEFT",         LV_SYMBOL_LEFT},
        {"RIGHT",        LV_SYMBOL_RIGHT},
        {"TRASH",        LV_SYMBOL_TRASH},
        {"BACKSPACE",    LV_SYMBOL_BACKSPACE},
        {"CHARGE",       LV_SYMBOL_CHARGE},
        {"EYE_OPEN",     LV_SYMBOL_EYE_OPEN},
        {"EYE_CLOSE",    LV_SYMBOL_EYE_CLOSE},
        {"WIFI",         LV_SYMBOL_WIFI},
        {"LOOP",         LV_SYMBOL_LOOP},
    };
    for (const auto& s : symbols) {
        if (strcmp(s.name, name) == 0) return s.sym;
    }
    return NULL;
}

static esp_err_t click_post_handler(httpd_req_t* req)
{
    CHECK_SESSION_LOCK(req);
    set_json_content_type(req);

    // Read request body
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 1024) {
        send_json_error(req, "400 Bad Request", "Invalid body");
        return ESP_OK;
    }

    char buf[1024];
    int received = httpd_req_recv(req, buf, content_len);
    if (received <= 0) {
        send_json_error(req, "400 Bad Request", "Failed to read body");
        return ESP_OK;
    }
    buf[received] = '\0';

    // Parse JSON
    cJSON* body = cJSON_Parse(buf);
    if (!body) {
        send_json_error(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }

    // Extract search criteria — copy strings so they survive cJSON_Delete
    char search_exact[256] = {0};
    char search_contains[256] = {0};
    char search_tag[64] = {0};
    bool has_exact = false;
    bool has_contains = false;
    bool has_tag = false;

    cJSON* j_tag = cJSON_GetObjectItem(body, "tag");
    cJSON* j_symbol = cJSON_GetObjectItem(body, "symbol");
    cJSON* j_label = cJSON_GetObjectItem(body, "label");
    cJSON* j_contains = cJSON_GetObjectItem(body, "label_contains");

    if (j_tag && cJSON_IsString(j_tag)) {
        strncpy(search_tag, j_tag->valuestring, sizeof(search_tag) - 1);
        has_tag = true;
        ESP_LOGI(TAG, "click: tag='%s'", search_tag);
    } else if (j_symbol && cJSON_IsString(j_symbol)) {
        // Resolve symbol name to UTF-8 glyph
        const char* sym = resolve_symbol(j_symbol->valuestring);
        if (!sym) {
            cJSON_Delete(body);
            send_json_error(req, "400 Bad Request", "Unknown symbol name");
            return ESP_OK;
        }
        strncpy(search_exact, sym, sizeof(search_exact) - 1);
        has_exact = true;
        ESP_LOGI(TAG, "click: symbol=%s", j_symbol->valuestring);
    } else if (j_label && cJSON_IsString(j_label)) {
        // Translate English label to current UI language
        const char* translated = i18n_translate(j_label->valuestring);
        strncpy(search_exact, translated, sizeof(search_exact) - 1);
        has_exact = true;
        ESP_LOGI(TAG, "click: label='%s' (translated='%s')", j_label->valuestring, translated);
    } else if (j_contains && cJSON_IsString(j_contains)) {
        const char* translated = i18n_translate(j_contains->valuestring);
        strncpy(search_contains, translated, sizeof(search_contains) - 1);
        has_contains = true;
        ESP_LOGI(TAG, "click: label_contains='%s' (translated='%s')", j_contains->valuestring, translated);
    } else {
        cJSON_Delete(body);
        send_json_error(req, "400 Bad Request", "Provide 'tag', 'label', 'label_contains', or 'symbol'");
        return ESP_OK;
    }

    // Also keep original English text for fallback search
    char original_exact[256] = {0};
    char original_contains[256] = {0};
    if (j_label && cJSON_IsString(j_label)) {
        strncpy(original_exact, j_label->valuestring, sizeof(original_exact) - 1);
    }
    if (j_contains && cJSON_IsString(j_contains)) {
        strncpy(original_contains, j_contains->valuestring, sizeof(original_contains) - 1);
    }

    cJSON_Delete(body);  // Safe to delete now — we've copied all strings

    if (!bsp_display_lock(1000)) {
        send_json_error(req, "503 Service Unavailable", "Could not acquire display lock");
        return ESP_OK;
    }

    lv_obj_t* scr = lv_scr_act();
    lv_obj_t* found = NULL;

    // Search by tag first (most specific)
    if (has_tag) {
        found = find_by_tag(scr, search_tag, 0);
    }

    // Search with translated/resolved text
    if (!found) {
        found = find_label_by_text(scr,
            has_exact ? search_exact : NULL,
            has_contains ? search_contains : NULL, 0);
    }

    // Fallback: try original English text (for non-i18n labels)
    if (!found && original_exact[0]) {
        found = find_label_by_text(scr, original_exact, NULL, 0);
    }
    if (!found && original_contains[0]) {
        found = find_label_by_text(scr, NULL, original_contains, 0);
    }

    if (!found) {
        bsp_display_unlock();
        send_json_error(req, "404 Not Found",
                        has_exact ? search_exact : search_contains);
        return ESP_OK;
    }

    // Find the clickable parent (the button, not the label inside it)
    lv_obj_t* target = find_clickable_parent(found);
    if (!target) {
        target = found;
    }

    // Test clicks emulate physical touch activity, including wake-only behavior.
    const bool consumed = display_idle_handle_activity();
    if (consumed) {
        bsp_display_unlock();
        cJSON* resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "success", true);
        cJSON_AddBoolToObject(resp, "consumed", true);
        char* json = cJSON_PrintUnformatted(resp);
        httpd_resp_sendstr(req, json);
        free(json);
        cJSON_Delete(resp);
        return ESP_OK;
    }

    // Fire the click event (measure render time for performance profiling)
    int64_t t0 = esp_timer_get_time();
    lv_obj_send_event(target, LV_EVENT_CLICKED, NULL);
    int64_t render_us = esp_timer_get_time() - t0;

    // Get info about what was clicked for the response
    const char* clicked_text = get_widget_text(found);
    const char* clicked_type = get_widget_type(target);

    bsp_display_unlock();

    // Send success response
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    cJSON_AddBoolToObject(resp, "consumed", false);
    cJSON_AddStringToObject(resp, "clicked_type", clicked_type);
    if (clicked_text) {
        cJSON_AddStringToObject(resp, "clicked_text", clicked_text);
    }
    cJSON_AddNumberToObject(resp, "render_time_us", (double)render_us);

    char* json = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(resp);

    return ESP_OK;
}

// ============================================================================
// POST /api/test/set-slider
// Body: {"tag": "brightness_slider", "value": 50}
// Sets a slider widget's value and fires LV_EVENT_VALUE_CHANGED + LV_EVENT_RELEASED
// ============================================================================

static esp_err_t set_slider_post_handler(httpd_req_t* req)
{
    CHECK_SESSION_LOCK(req);
    set_json_content_type(req);

    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 1024) {
        send_json_error(req, "400 Bad Request", "Invalid body");
        return ESP_OK;
    }

    char buf[1024];
    int received = httpd_req_recv(req, buf, content_len);
    if (received <= 0) {
        send_json_error(req, "400 Bad Request", "Failed to read body");
        return ESP_OK;
    }
    buf[received] = '\0';

    cJSON* body = cJSON_Parse(buf);
    if (!body) {
        send_json_error(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }

    cJSON* j_tag = cJSON_GetObjectItem(body, "tag");
    cJSON* j_value = cJSON_GetObjectItem(body, "value");

    if (!j_tag || !cJSON_IsString(j_tag) || !j_value || !cJSON_IsNumber(j_value)) {
        cJSON_Delete(body);
        send_json_error(req, "400 Bad Request", "Provide 'tag' (string) and 'value' (number)");
        return ESP_OK;
    }

    char search_tag[64] = {0};
    strncpy(search_tag, j_tag->valuestring, sizeof(search_tag) - 1);
    int value = (int)j_value->valuedouble;
    cJSON_Delete(body);

    ESP_LOGI(TAG, "set-slider: tag='%s', value=%d", search_tag, value);

    if (!bsp_display_lock(1000)) {
        send_json_error(req, "503 Service Unavailable", "Could not acquire display lock");
        return ESP_OK;
    }

    lv_obj_t* scr = lv_scr_act();
    lv_obj_t* found = find_by_tag(scr, search_tag, 0);

    if (!found) {
        bsp_display_unlock();
        send_json_error(req, "404 Not Found", search_tag);
        return ESP_OK;
    }

    if (!lv_obj_check_type(found, &lv_slider_class)) {
        bsp_display_unlock();
        send_json_error(req, "400 Bad Request", "Widget is not a slider");
        return ESP_OK;
    }

    // Set the slider value and fire events (mimics user dragging + releasing)
    lv_slider_set_value(found, value, LV_ANIM_OFF);
    lv_obj_send_event(found, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_send_event(found, LV_EVENT_RELEASED, NULL);

    int actual = lv_slider_get_value(found);
    bsp_display_unlock();

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    cJSON_AddNumberToObject(resp, "value", actual);

    char* json = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(resp);

    return ESP_OK;
}

// ============================================================================
// POST /api/test/display-idle
// Body: {"action": "dim" | "wake" | "status"}
// ============================================================================

static esp_err_t display_idle_post_handler(httpd_req_t* req)
{
    CHECK_SESSION_LOCK(req);
    set_json_content_type(req);

    if (req->content_len <= 0 || req->content_len >= 128) {
        send_json_error(req, "400 Bad Request", "Invalid body");
        return ESP_OK;
    }

    char buf[128];
    int received = httpd_req_recv(req, buf, req->content_len);
    if (received <= 0) {
        send_json_error(req, "400 Bad Request", "Failed to read body");
        return ESP_OK;
    }
    buf[received] = '\0';

    cJSON* body = cJSON_Parse(buf);
    cJSON* action = body ? cJSON_GetObjectItem(body, "action") : nullptr;
    if (!cJSON_IsString(action)) {
        cJSON_Delete(body);
        send_json_error(req, "400 Bad Request", "Action must be dim, wake, or status");
        return ESP_OK;
    }

    const bool should_dim = strcmp(action->valuestring, "dim") == 0;
    const bool should_wake = strcmp(action->valuestring, "wake") == 0;
    const bool should_report = strcmp(action->valuestring, "status") == 0;
    if (!should_dim && !should_wake && !should_report) {
        cJSON_Delete(body);
        send_json_error(req, "400 Bad Request", "Action must be dim, wake, or status");
        return ESP_OK;
    }
    cJSON_Delete(body);

    if (!bsp_display_lock(1000)) {
        send_json_error(req, "503 Service Unavailable", "Could not acquire display lock");
        return ESP_OK;
    }
    bool consumed = false;
    if (should_dim) {
        display_idle_force_dim();
    } else if (should_wake) {
        consumed = display_idle_handle_activity();
    }
    const bool dimmed = display_idle_is_dimmed();
    const int saved_brightness = display_screen_get_brightness();
    bsp_display_unlock();

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    cJSON_AddBoolToObject(resp, "dimmed", dimmed);
    cJSON_AddBoolToObject(resp, "consumed", consumed);
    cJSON_AddNumberToObject(resp, "saved_brightness", saved_brightness);
    char* json = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(resp);
    return ESP_OK;
}

// ============================================================================
// POST /api/test/scroll
// Body: {"tag": "event_log_content", "y": 300}
// Scrolls a tagged container to an absolute vertical position.
// ============================================================================

static esp_err_t scroll_post_handler(httpd_req_t* req)
{
    CHECK_SESSION_LOCK(req);
    set_json_content_type(req);

    char buf[256];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        send_json_error(req, "400 Bad Request", "Invalid body");
        return ESP_OK;
    }
    buf[received] = '\0';

    cJSON* body = cJSON_Parse(buf);
    cJSON* j_tag = body ? cJSON_GetObjectItem(body, "tag") : NULL;
    cJSON* j_y = body ? cJSON_GetObjectItem(body, "y") : NULL;
    if (!j_tag || !cJSON_IsString(j_tag) || !j_y || !cJSON_IsNumber(j_y)) {
        cJSON_Delete(body);
        send_json_error(req, "400 Bad Request", "Provide 'tag' and 'y'");
        return ESP_OK;
    }

    char search_tag[64] = {0};
    strncpy(search_tag, j_tag->valuestring, sizeof(search_tag) - 1);
    int y = (int)j_y->valuedouble;
    cJSON_Delete(body);

    if (!bsp_display_lock(1000)) {
        send_json_error(req, "503 Service Unavailable", "Could not acquire display lock");
        return ESP_OK;
    }

    lv_obj_t* found = find_by_tag(lv_scr_act(), search_tag, 0);
    if (!found) {
        bsp_display_unlock();
        send_json_error(req, "404 Not Found", search_tag);
        return ESP_OK;
    }

    lv_obj_scroll_to_y(found, y, LV_ANIM_OFF);
    lv_obj_send_event(found, LV_EVENT_SCROLL, NULL);
    int actual = lv_obj_get_scroll_y(found);
    bsp_display_unlock();

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    cJSON_AddNumberToObject(resp, "y", actual);
    char* json = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(resp);
    return ESP_OK;
}

// ============================================================================
// POST /api/test/set-roller
// Body: {"tag": "timezone_roller", "index": 3}
// Sets a roller widget's selected index and fires LV_EVENT_VALUE_CHANGED
// ============================================================================

static esp_err_t set_roller_post_handler(httpd_req_t* req)
{
    CHECK_SESSION_LOCK(req);
    set_json_content_type(req);

    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 1024) {
        send_json_error(req, "400 Bad Request", "Invalid body");
        return ESP_OK;
    }

    char buf[1024];
    int received = httpd_req_recv(req, buf, content_len);
    if (received <= 0) {
        send_json_error(req, "400 Bad Request", "Failed to read body");
        return ESP_OK;
    }
    buf[received] = '\0';

    cJSON* body = cJSON_Parse(buf);
    if (!body) {
        send_json_error(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }

    cJSON* j_tag = cJSON_GetObjectItem(body, "tag");
    cJSON* j_index = cJSON_GetObjectItem(body, "index");

    if (!j_tag || !cJSON_IsString(j_tag) || !j_index || !cJSON_IsNumber(j_index)) {
        cJSON_Delete(body);
        send_json_error(req, "400 Bad Request", "Provide 'tag' (string) and 'index' (number)");
        return ESP_OK;
    }

    char search_tag[64] = {0};
    strncpy(search_tag, j_tag->valuestring, sizeof(search_tag) - 1);
    int index = (int)j_index->valuedouble;
    cJSON_Delete(body);

    ESP_LOGI(TAG, "set-roller: tag='%s', index=%d", search_tag, index);

    if (!bsp_display_lock(1000)) {
        send_json_error(req, "503 Service Unavailable", "Could not acquire display lock");
        return ESP_OK;
    }

    lv_obj_t* scr = lv_scr_act();
    lv_obj_t* found = find_by_tag(scr, search_tag, 0);

    if (!found) {
        bsp_display_unlock();
        send_json_error(req, "404 Not Found", search_tag);
        return ESP_OK;
    }

    if (!lv_obj_check_type(found, &lv_roller_class)) {
        bsp_display_unlock();
        send_json_error(req, "400 Bad Request", "Widget is not a roller");
        return ESP_OK;
    }

    uint32_t option_count = lv_roller_get_option_count(found);
    if (index < 0 || (uint32_t)index >= option_count) {
        bsp_display_unlock();
        char err[64];
        snprintf(err, sizeof(err), "Index %d out of range (0-%lu)", index, (unsigned long)(option_count - 1));
        send_json_error(req, "400 Bad Request", err);
        return ESP_OK;
    }

    lv_roller_set_selected(found, index, LV_ANIM_ON);
    lv_obj_send_event(found, LV_EVENT_VALUE_CHANGED, NULL);

    uint32_t actual = lv_roller_get_selected(found);
    char sel_text[64] = {0};
    lv_roller_get_selected_str(found, sel_text, sizeof(sel_text));
    bsp_display_unlock();

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    cJSON_AddNumberToObject(resp, "value", (int)actual);
    cJSON_AddStringToObject(resp, "selected_text", sel_text);

    char* json = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(resp);

    return ESP_OK;
}

// ============================================================================
// POST /api/test/toggle
// Body: {"tag": "demo_mode_switch"}
// Toggles a switch widget and fires LV_EVENT_VALUE_CHANGED
// ============================================================================

static esp_err_t toggle_post_handler(httpd_req_t* req)
{
    CHECK_SESSION_LOCK(req);
    set_json_content_type(req);

    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 1024) {
        send_json_error(req, "400 Bad Request", "Invalid body");
        return ESP_OK;
    }

    char buf[1024];
    int received = httpd_req_recv(req, buf, content_len);
    if (received <= 0) {
        send_json_error(req, "400 Bad Request", "Failed to read body");
        return ESP_OK;
    }
    buf[received] = '\0';

    cJSON* body = cJSON_Parse(buf);
    if (!body) {
        send_json_error(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }

    cJSON* j_tag = cJSON_GetObjectItem(body, "tag");
    if (!j_tag || !cJSON_IsString(j_tag)) {
        cJSON_Delete(body);
        send_json_error(req, "400 Bad Request", "Provide 'tag' (string)");
        return ESP_OK;
    }

    char search_tag[64] = {0};
    strncpy(search_tag, j_tag->valuestring, sizeof(search_tag) - 1);
    cJSON_Delete(body);

    ESP_LOGI(TAG, "toggle: tag='%s'", search_tag);

    if (!bsp_display_lock(1000)) {
        send_json_error(req, "503 Service Unavailable", "Could not acquire display lock");
        return ESP_OK;
    }

    lv_obj_t* scr = lv_scr_act();
    lv_obj_t* found = find_by_tag(scr, search_tag, 0);

    if (!found) {
        bsp_display_unlock();
        send_json_error(req, "404 Not Found", search_tag);
        return ESP_OK;
    }

    if (!lv_obj_check_type(found, &lv_switch_class)) {
        bsp_display_unlock();
        send_json_error(req, "400 Bad Request", "Widget is not a switch");
        return ESP_OK;
    }

    // Toggle the switch state
    if (lv_obj_has_state(found, LV_STATE_CHECKED)) {
        lv_obj_remove_state(found, LV_STATE_CHECKED);
    } else {
        lv_obj_add_state(found, LV_STATE_CHECKED);
    }
    lv_obj_send_event(found, LV_EVENT_VALUE_CHANGED, NULL);

    bool checked = lv_obj_has_state(found, LV_STATE_CHECKED);
    bsp_display_unlock();

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    cJSON_AddBoolToObject(resp, "checked", checked);

    char* json = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(resp);

    return ESP_OK;
}

// ============================================================================
// CORS preflight handler for POST endpoints
// ============================================================================

static esp_err_t test_options_handler(httpd_req_t* req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, X-API-Key");
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ============================================================================
// POST /api/test/wifi-mock — inject fake WiFi networks and pause scanning
// Body: {"networks": [{"ssid": "Name", "rssi": -45, "authmode": 3}, ...]}
// ============================================================================

static esp_err_t wifi_mock_post_handler(httpd_req_t* req)
{
    CHECK_SESSION_LOCK(req);
    char buf[1024] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        send_json_error(req, "400 Bad Request", "Empty body");
        return ESP_OK;
    }

    cJSON* body = cJSON_Parse(buf);
    if (!body) {
        send_json_error(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }

    cJSON* networks_arr = cJSON_GetObjectItem(body, "networks");
    if (!networks_arr || !cJSON_IsArray(networks_arr)) {
        cJSON_Delete(body);
        send_json_error(req, "400 Bad Request", "Missing 'networks' array");
        return ESP_OK;
    }

    int count = cJSON_GetArraySize(networks_arr);
    if (count > 20) count = 20;

    settings_wifi_network_t nets[20] = {};
    for (int i = 0; i < count; i++) {
        cJSON* item = cJSON_GetArrayItem(networks_arr, i);
        cJSON* j_ssid = cJSON_GetObjectItem(item, "ssid");
        cJSON* j_rssi = cJSON_GetObjectItem(item, "rssi");
        cJSON* j_auth = cJSON_GetObjectItem(item, "authmode");

        if (j_ssid && cJSON_IsString(j_ssid)) {
            strncpy(nets[i].ssid, j_ssid->valuestring, sizeof(nets[i].ssid) - 1);
        }
        nets[i].rssi = (j_rssi && cJSON_IsNumber(j_rssi)) ? (int8_t)j_rssi->valueint : -50;
        nets[i].authmode = (j_auth && cJSON_IsNumber(j_auth)) ? (uint8_t)j_auth->valueint : 0;
    }
    cJSON_Delete(body);

    // Enable mock mode (pauses scan timer, blocks real connects)
    bsp_display_lock(0);
    wifi_screen_set_mock_mode(true);
    wifi_screen_update_networks(nets, (uint8_t)count);
    wifi_screen_set_scanning(false);
    bsp_display_unlock();

    set_json_content_type(req);
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    cJSON_AddNumberToObject(resp, "injected", count);
    char* json = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(resp);
    return ESP_OK;
}

// ============================================================================
// POST /api/test/wifi-mock-reset — exit mock mode, resume real scanning
// ============================================================================

static esp_err_t wifi_mock_reset_post_handler(httpd_req_t* req)
{
    CHECK_SESSION_LOCK(req);
    bsp_display_lock(0);
    wifi_screen_set_mock_mode(false);
    wifi_screen_trigger_scan();
    bsp_display_unlock();

    set_json_content_type(req);
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    char* json = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(resp);
    return ESP_OK;
}

// ============================================================================
// POST /api/test/type-text — type text into a textarea widget
// Body: {"tag": "wifi_password_input", "text": "mypassword"}
// ============================================================================

static esp_err_t type_text_post_handler(httpd_req_t* req)
{
    CHECK_SESSION_LOCK(req);
    char buf[512] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        send_json_error(req, "400 Bad Request", "Empty body");
        return ESP_OK;
    }

    cJSON* body = cJSON_Parse(buf);
    if (!body) {
        send_json_error(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }

    cJSON* j_tag = cJSON_GetObjectItem(body, "tag");
    cJSON* j_text = cJSON_GetObjectItem(body, "text");
    if (!j_tag || !cJSON_IsString(j_tag) || !j_text || !cJSON_IsString(j_text)) {
        cJSON_Delete(body);
        send_json_error(req, "400 Bad Request", "Need 'tag' and 'text' strings");
        return ESP_OK;
    }

    bsp_display_lock(0);
    lv_obj_t* screen = lv_screen_active();
    lv_obj_t* obj = find_by_tag(screen, j_tag->valuestring, 0);

    if (!obj) {
        bsp_display_unlock();
        cJSON_Delete(body);
        send_json_error(req, "404 Not Found", "Widget not found");
        return ESP_OK;
    }

    if (!lv_obj_check_type(obj, &lv_textarea_class)) {
        bsp_display_unlock();
        cJSON_Delete(body);
        send_json_error(req, "400 Bad Request", "Widget is not a textarea");
        return ESP_OK;
    }

    lv_textarea_set_text(obj, j_text->valuestring);

    // Check password mode state for the response
    bool is_password_mode = lv_textarea_get_password_mode(obj);
    const char* displayed_text = lv_textarea_get_text(obj);

    bsp_display_unlock();

    set_json_content_type(req);
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    cJSON_AddStringToObject(resp, "text", displayed_text);
    cJSON_AddBoolToObject(resp, "password_mode", is_password_mode);
    char* json = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(resp);
    cJSON_Delete(body);
    return ESP_OK;
}

// ============================================================================
// POST /api/test/firmware-mock — inject fake firmware check result
// Body: {"version": "99.0.0", "update_available": true}
// ============================================================================

static esp_err_t firmware_mock_post_handler(httpd_req_t* req)
{
    CHECK_SESSION_LOCK(req);
    char buf[256] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        send_json_error(req, "400 Bad Request", "Empty body");
        return ESP_OK;
    }

    cJSON* body = cJSON_Parse(buf);
    if (!body) {
        send_json_error(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }

    cJSON* j_version = cJSON_GetObjectItem(body, "version");
    cJSON* j_update = cJSON_GetObjectItem(body, "update_available");

    if (!j_version || !cJSON_IsString(j_version)) {
        cJSON_Delete(body);
        send_json_error(req, "400 Bad Request", "Missing 'version' string");
        return ESP_OK;
    }

    bool update_available = (j_update && cJSON_IsBool(j_update)) ? cJSON_IsTrue(j_update) : false;

    bsp_display_lock(0);
    firmware_screen_set_mock_result(j_version->valuestring, update_available);
    bsp_display_unlock();

    set_json_content_type(req);
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    cJSON_AddStringToObject(resp, "version", j_version->valuestring);
    cJSON_AddBoolToObject(resp, "update_available", update_available);
    char* json = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(resp);
    cJSON_Delete(body);
    return ESP_OK;
}

// ============================================================================
// POST /api/test/firmware-mock-reset — clear mock state
// ============================================================================

static esp_err_t firmware_mock_reset_post_handler(httpd_req_t* req)
{
    CHECK_SESSION_LOCK(req);
    bsp_display_lock(0);
    firmware_screen_clear_mock();
    bsp_display_unlock();

    set_json_content_type(req);
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    char* json = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(resp);
    return ESP_OK;
}

// ============================================================================
// POST /api/test/notification-mock — add a notification to the status bar
// Body: {"type": 0, "message": "Firmware update available"}
// type: 0=firmware update, 1=wifi unstable, 2=low battery
// ============================================================================

static esp_err_t notification_mock_post_handler(httpd_req_t* req)
{
    CHECK_SESSION_LOCK(req);
    char buf[256] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        send_json_error(req, "400 Bad Request", "Empty body");
        return ESP_OK;
    }

    cJSON* body = cJSON_Parse(buf);
    if (!body) {
        send_json_error(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }

    cJSON* j_type = cJSON_GetObjectItem(body, "type");
    cJSON* j_message = cJSON_GetObjectItem(body, "message");

    if (!j_type || !cJSON_IsNumber(j_type)) {
        cJSON_Delete(body);
        send_json_error(req, "400 Bad Request", "Missing or invalid 'type' number");
        return ESP_OK;
    }

    int type_val = j_type->valueint;
    if (type_val < 0 || type_val >= STATUS_BAR_NOTIFY_MAX) {
        cJSON_Delete(body);
        send_json_error(req, "400 Bad Request", "Invalid notification type");
        return ESP_OK;
    }

    const char* message = (j_message && cJSON_IsString(j_message)) ? j_message->valuestring : NULL;

    bsp_display_lock(0);
    status_bar_add_notification((status_bar_notify_type_t)type_val, message);
    bsp_display_unlock();

    set_json_content_type(req);
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    cJSON_AddNumberToObject(resp, "type", type_val);
    if (message) {
        cJSON_AddStringToObject(resp, "message", message);
    }
    char* json = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(resp);
    cJSON_Delete(body);
    return ESP_OK;
}

// ============================================================================
// POST /api/test/notification-mock-reset — clear all notifications
// ============================================================================

static esp_err_t notification_mock_reset_post_handler(httpd_req_t* req)
{
    CHECK_SESSION_LOCK(req);
    bsp_display_lock(0);
    status_bar_clear_all_notifications();
    bsp_display_unlock();

    set_json_content_type(req);
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    char* json = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(resp);
    return ESP_OK;
}

// ============================================================================
// POST /api/test/set-preference — set app preferences directly (no UI)
// ============================================================================
// Body: {"demo_mode": true}
// Sets the specified preference(s) without triggering any UI interaction.
// Returns the updated preferences snapshot.

static esp_err_t set_preference_post_handler(httpd_req_t* req)
{
    CHECK_SESSION_LOCK(req);

    char body[256];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        send_json_error(req, "400 Bad Request", "Empty request body");
        return ESP_OK;
    }
    body[received] = '\0';

    cJSON* root = cJSON_Parse(body);
    if (!root) {
        send_json_error(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }

    // Apply supported preferences
    cJSON* demo = cJSON_GetObjectItem(root, "demo_mode");
    if (demo && cJSON_IsBool(demo)) {
        bool enable = cJSON_IsTrue(demo);
        app_prefs_set_demo_mode(enable);
        // Also set the runtime flag so isDemoMode() reflects the change immediately
        if (enable && !arctic::isDemoMode()) {
            arctic::initDemoState();
        }
        ESP_LOGI(TAG, "set-preference: demo_mode=%s", enable ? "true" : "false");
    }

    cJSON_Delete(root);

    // Return updated preferences
    set_json_content_type(req);
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    cJSON_AddBoolToObject(resp, "demo_mode", app_prefs_is_demo_mode());
    char* json = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(resp);
    return ESP_OK;
}

// ============================================================================
// POST /api/test/set-demo-field — set demo state fields (no auth)
// ============================================================================

static esp_err_t set_demo_field_post_handler(httpd_req_t* req)
{
    CHECK_SESSION_LOCK(req);
    if (!arctic::isDemoMode()) {
        send_json_error(req, "403 Forbidden", "Demo mode is not enabled");
        return ESP_OK;
    }

    char body[512];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        send_json_error(req, "400 Bad Request", "Empty request body");
        return ESP_OK;
    }
    body[received] = '\0';

    cJSON* root = cJSON_Parse(body);
    if (!root) {
        send_json_error(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }

    set_json_content_type(req);
    cJSON* resp = cJSON_CreateObject();
    cJSON* results = cJSON_AddObjectToObject(resp, "results");
    int success_count = 0;
    int fail_count = 0;

    cJSON* item = NULL;
    cJSON_ArrayForEach(item, root) {
        if (!cJSON_IsNumber(item)) {
            cJSON_AddStringToObject(results, item->string, "error: value must be a number");
            fail_count++;
            continue;
        }
        int32_t value = (int32_t)item->valuedouble;
        if (arctic::setDemoField(item->string, value)) {
            cJSON_AddStringToObject(results, item->string, "ok");
            success_count++;
        } else {
            cJSON_AddStringToObject(results, item->string, "error: unknown field");
            fail_count++;
        }
    }
    cJSON_Delete(root);

    cJSON_AddBoolToObject(resp, "success", fail_count == 0 && success_count > 0);
    cJSON_AddNumberToObject(resp, "updated", success_count);
    cJSON_AddNumberToObject(resp, "failed", fail_count);

    char* json = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(resp);
    return ESP_OK;
}

static esp_err_t record_reset_reason_post_handler(httpd_req_t* req)
{
    CHECK_SESSION_LOCK(req);

    char body[128];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        send_json_error(req, "400 Bad Request", "Empty request body");
        return ESP_OK;
    }
    body[received] = '\0';

    cJSON* root = cJSON_Parse(body);
    cJSON* reason = root ? cJSON_GetObjectItem(root, "reason") : NULL;
    if (!reason || !cJSON_IsNumber(reason)) {
        cJSON_Delete(root);
        send_json_error(req, "400 Bad Request", "Provide numeric 'reason'");
        return ESP_OK;
    }

    event_log_record_reset_reason((esp_reset_reason_t)reason->valueint);
    cJSON_Delete(root);
    httpd_resp_sendstr(req, "{\"success\":true}");
    return ESP_OK;
}

// ============================================================================
// POST /api/test/clear-error-history — clear the error history ring buffer
// ============================================================================

static esp_err_t clear_error_history_post_handler(httpd_req_t* req)
{
    CHECK_SESSION_LOCK(req);
    arctic::clearErrorHistory();

    set_json_content_type(req);
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    char* json = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(resp);
    return ESP_OK;
}

// ============================================================================
// POST /api/test/lock — acquire session lock
// Body: {"session_id": "uuid-here", "ttl_seconds": 900}
// ============================================================================

static esp_err_t lock_post_handler(httpd_req_t* req)
{
    char buf[256] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        send_json_error(req, "400 Bad Request", "Empty body");
        return ESP_OK;
    }

    cJSON* body = cJSON_Parse(buf);
    if (!body) {
        send_json_error(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }

    cJSON* j_session = cJSON_GetObjectItem(body, "session_id");
    if (!j_session || !cJSON_IsString(j_session) || strlen(j_session->valuestring) == 0) {
        cJSON_Delete(body);
        send_json_error(req, "400 Bad Request", "Missing 'session_id' string");
        return ESP_OK;
    }

    // Check if already locked by someone else
    if (is_lock_held() && strcmp(s_lock_session_id, j_session->valuestring) != 0) {
        cJSON_Delete(body);
        reject_locked(req);
        return ESP_OK;
    }

    // Acquire or renew lock
    strncpy(s_lock_session_id, j_session->valuestring, sizeof(s_lock_session_id) - 1);
    s_lock_session_id[sizeof(s_lock_session_id) - 1] = '\0';
    s_lock_acquired_us = esp_timer_get_time();

    cJSON* j_ttl = cJSON_GetObjectItem(body, "ttl_seconds");
    if (j_ttl && cJSON_IsNumber(j_ttl) && j_ttl->valueint > 0) {
        s_lock_ttl_us = (int64_t)j_ttl->valueint * 1000000LL;
    } else {
        s_lock_ttl_us = 15 * 60 * 1000000LL;  // Default 15 min
    }

    ESP_LOGI(TAG, "Session lock acquired: '%s' (TTL %lds)", s_lock_session_id, (long)(s_lock_ttl_us / 1000000LL));

    set_json_content_type(req);
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "acquired", true);
    cJSON_AddStringToObject(resp, "session_id", s_lock_session_id);
    cJSON_AddNumberToObject(resp, "ttl_seconds", (double)(s_lock_ttl_us / 1000000LL));
    char* json = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(resp);
    cJSON_Delete(body);
    return ESP_OK;
}

// ============================================================================
// POST /api/test/unlock — release session lock
// Body: {"session_id": "uuid-here"}  (must match holder, or force with "force": true)
// ============================================================================

static esp_err_t unlock_post_handler(httpd_req_t* req)
{
    char buf[256] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);

    // Allow empty body to force-unlock (useful for cleanup/debugging)
    bool force = false;
    char session_id[64] = {0};

    if (ret > 0) {
        cJSON* body = cJSON_Parse(buf);
        if (body) {
            cJSON* j_session = cJSON_GetObjectItem(body, "session_id");
            if (j_session && cJSON_IsString(j_session)) {
                strncpy(session_id, j_session->valuestring, sizeof(session_id) - 1);
            }
            cJSON* j_force = cJSON_GetObjectItem(body, "force");
            force = (j_force && cJSON_IsTrue(j_force));
            cJSON_Delete(body);
        }
    }

    if (!force && is_lock_held() && strlen(session_id) > 0
        && strcmp(session_id, s_lock_session_id) != 0) {
        reject_locked(req);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Session lock released (was '%s')", s_lock_session_id);
    s_lock_session_id[0] = '\0';

    set_json_content_type(req);
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "released", true);
    char* json = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(resp);
    return ESP_OK;
}

// ============================================================================
// GET /api/test/lock — check lock status
// ============================================================================

static esp_err_t lock_get_handler(httpd_req_t* req)
{
    set_json_content_type(req);
    cJSON* resp = cJSON_CreateObject();
    bool locked = is_lock_held();
    cJSON_AddBoolToObject(resp, "locked", locked);
    if (locked) {
        cJSON_AddStringToObject(resp, "session_id", s_lock_session_id);
        int64_t remaining_s = (s_lock_ttl_us - (esp_timer_get_time() - s_lock_acquired_us)) / 1000000LL;
        cJSON_AddNumberToObject(resp, "remaining_seconds", remaining_s > 0 ? remaining_s : 0);
    }
    char* json = cJSON_PrintUnformatted(resp);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(resp);
    return ESP_OK;
}

// ============================================================================
// Screenshot — capture display as PNG
// ============================================================================

static esp_err_t screenshot_get_handler(httpd_req_t* req)
{
    // Screenshot is read-only, no session lock needed
    ESP_LOGI(TAG, "Screenshot requested");

    // Get screen dimensions under LVGL lock
    bsp_display_lock(0);
    lv_obj_t* screen = lv_screen_active();
    lv_obj_update_layout(screen);
    int32_t w = lv_obj_get_width(screen);
    int32_t h = lv_obj_get_height(screen);
    bsp_display_unlock();

    ESP_LOGI(TAG, "Screen size: %ldx%ld", w, h);

    // Allocate pixel buffer in PSRAM (720x1280x3 ≈ 2.7MB — too large for internal RAM)
    uint32_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_RGB888);
    uint32_t buf_size = stride * h;
    void* pixel_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!pixel_buf) {
        ESP_LOGE(TAG, "Failed to allocate snapshot buffer (%lu bytes in PSRAM)", (unsigned long)buf_size);
        httpd_resp_set_status(req, "500 Internal Server Error");
        set_json_content_type(req);
        httpd_resp_sendstr(req, "{\"error\":\"Out of memory\"}");
        return ESP_OK;
    }

    // Init draw buffer with our PSRAM-backed memory
    lv_draw_buf_t snapshot;
    lv_draw_buf_init(&snapshot, w, h, LV_COLOR_FORMAT_RGB888, stride, pixel_buf, buf_size);

    // Take snapshot under LVGL lock
    bsp_display_lock(0);
    screen = lv_screen_active();
    lv_result_t snap_res = lv_snapshot_take_to_draw_buf(screen, LV_COLOR_FORMAT_RGB888, &snapshot);
    bsp_display_unlock();

    if (snap_res != LV_RESULT_OK) {
        ESP_LOGE(TAG, "Failed to take snapshot");
        heap_caps_free(pixel_buf);
        httpd_resp_set_status(req, "500 Internal Server Error");
        set_json_content_type(req);
        httpd_resp_sendstr(req, "{\"error\":\"Failed to capture screenshot\"}");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Snapshot captured: %ldx%ld, stride=%lu", w, h, (unsigned long)stride);

    // LVGL's RGB888 stores pixels as B,G,R in memory but PNG expects R,G,B.
    // Swap R↔B in-place and pack rows (remove stride padding) in one pass.
    uint8_t* dst = (uint8_t*)pixel_buf;
    const uint8_t* src_row = (const uint8_t*)pixel_buf;
    for (int32_t y = 0; y < h; y++) {
        const uint8_t* s = src_row;
        for (int32_t x = 0; x < w; x++) {
            uint8_t b = s[0], g = s[1], r = s[2];
            dst[0] = r;
            dst[1] = g;
            dst[2] = b;
            dst += 3;
            s += 3;
        }
        src_row += stride;
    }

    // Encode to PNG
    uint8_t* png_data = NULL;
    size_t png_size = 0;
    int64_t encode_start = esp_timer_get_time();
    png_encode_result_t error = png_encode_rgb888((const uint8_t*)pixel_buf, w, h, &png_data, &png_size);
    int64_t encode_ms = (esp_timer_get_time() - encode_start) / 1000;

    heap_caps_free(pixel_buf);

    if (error != PNG_ENCODE_OK) {
        ESP_LOGE(TAG, "PNG encoding failed (code %d)", error);
        if (png_data) heap_caps_free(png_data);
        httpd_resp_set_status(req, "500 Internal Server Error");
        set_json_content_type(req);
        char err_json[128];
        snprintf(err_json, sizeof(err_json),
                 "{\"error\":\"PNG encoding failed\",\"code\":%d}", error);
        httpd_resp_sendstr(req, err_json);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "PNG encoded: %u bytes in %ld ms", (unsigned)png_size, (long)encode_ms);

    // Send PNG response
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=\"screenshot.png\"");

    // Send in chunks to avoid stack issues with large buffers
    const size_t chunk_size = 8192;
    size_t sent = 0;
    esp_err_t ret = ESP_OK;
    while (sent < png_size) {
        size_t to_send = png_size - sent;
        if (to_send > chunk_size) to_send = chunk_size;
        ret = httpd_resp_send_chunk(req, (const char*)(png_data + sent), to_send);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send PNG chunk at offset %u", (unsigned)sent);
            break;
        }
        sent += to_send;
    }
    // Finalize chunked response
    if (ret == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    }

    heap_caps_free(png_data);
    return ESP_OK;
}

// ============================================================================
// Registration
// ============================================================================

void test_endpoints_register(httpd_handle_t server)
{
    httpd_uri_t ui_state_uri = {
        .uri = "/api/test/ui-state",
        .method = HTTP_GET,
        .handler = ui_state_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &ui_state_uri);

    httpd_uri_t screen_uri = {
        .uri = "/api/test/screen",
        .method = HTTP_GET,
        .handler = screen_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &screen_uri);

    httpd_uri_t click_uri = {
        .uri = "/api/test/click",
        .method = HTTP_POST,
        .handler = click_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &click_uri);

    httpd_uri_t click_options_uri = {
        .uri = "/api/test/click",
        .method = HTTP_OPTIONS,
        .handler = test_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &click_options_uri);

    httpd_uri_t set_slider_uri = {
        .uri = "/api/test/set-slider",
        .method = HTTP_POST,
        .handler = set_slider_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &set_slider_uri);

    httpd_uri_t set_slider_options_uri = {
        .uri = "/api/test/set-slider",
        .method = HTTP_OPTIONS,
        .handler = test_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &set_slider_options_uri);

    httpd_uri_t display_idle_uri = {
        .uri = "/api/test/display-idle",
        .method = HTTP_POST,
        .handler = display_idle_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &display_idle_uri);

    httpd_uri_t display_idle_options_uri = {
        .uri = "/api/test/display-idle",
        .method = HTTP_OPTIONS,
        .handler = test_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &display_idle_options_uri);

    httpd_uri_t scroll_uri = {
        .uri = "/api/test/scroll",
        .method = HTTP_POST,
        .handler = scroll_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &scroll_uri);

    httpd_uri_t scroll_options_uri = {
        .uri = "/api/test/scroll",
        .method = HTTP_OPTIONS,
        .handler = test_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &scroll_options_uri);

    httpd_uri_t set_roller_uri = {
        .uri = "/api/test/set-roller",
        .method = HTTP_POST,
        .handler = set_roller_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &set_roller_uri);

    httpd_uri_t set_roller_options_uri = {
        .uri = "/api/test/set-roller",
        .method = HTTP_OPTIONS,
        .handler = test_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &set_roller_options_uri);

    httpd_uri_t toggle_uri = {
        .uri = "/api/test/toggle",
        .method = HTTP_POST,
        .handler = toggle_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &toggle_uri);

    httpd_uri_t toggle_options_uri = {
        .uri = "/api/test/toggle",
        .method = HTTP_OPTIONS,
        .handler = test_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &toggle_options_uri);

    httpd_uri_t wifi_mock_uri = {
        .uri = "/api/test/wifi-mock",
        .method = HTTP_POST,
        .handler = wifi_mock_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &wifi_mock_uri);

    httpd_uri_t wifi_mock_options_uri = {
        .uri = "/api/test/wifi-mock",
        .method = HTTP_OPTIONS,
        .handler = test_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &wifi_mock_options_uri);

    httpd_uri_t wifi_mock_reset_uri = {
        .uri = "/api/test/wifi-mock-reset",
        .method = HTTP_POST,
        .handler = wifi_mock_reset_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &wifi_mock_reset_uri);

    httpd_uri_t wifi_mock_reset_options_uri = {
        .uri = "/api/test/wifi-mock-reset",
        .method = HTTP_OPTIONS,
        .handler = test_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &wifi_mock_reset_options_uri);

    httpd_uri_t type_text_uri = {
        .uri = "/api/test/type-text",
        .method = HTTP_POST,
        .handler = type_text_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &type_text_uri);

    httpd_uri_t type_text_options_uri = {
        .uri = "/api/test/type-text",
        .method = HTTP_OPTIONS,
        .handler = test_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &type_text_options_uri);

    httpd_uri_t firmware_mock_uri = {
        .uri = "/api/test/firmware-mock",
        .method = HTTP_POST,
        .handler = firmware_mock_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &firmware_mock_uri);

    httpd_uri_t firmware_mock_options_uri = {
        .uri = "/api/test/firmware-mock",
        .method = HTTP_OPTIONS,
        .handler = test_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &firmware_mock_options_uri);

    httpd_uri_t firmware_mock_reset_uri = {
        .uri = "/api/test/firmware-mock-reset",
        .method = HTTP_POST,
        .handler = firmware_mock_reset_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &firmware_mock_reset_uri);

    httpd_uri_t firmware_mock_reset_options_uri = {
        .uri = "/api/test/firmware-mock-reset",
        .method = HTTP_OPTIONS,
        .handler = test_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &firmware_mock_reset_options_uri);

    httpd_uri_t set_preference_uri = {
        .uri = "/api/test/set-preference",
        .method = HTTP_POST,
        .handler = set_preference_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &set_preference_uri);

    httpd_uri_t set_preference_options_uri = {
        .uri = "/api/test/set-preference",
        .method = HTTP_OPTIONS,
        .handler = test_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &set_preference_options_uri);

    httpd_uri_t set_demo_field_uri = {
        .uri = "/api/test/set-demo-field",
        .method = HTTP_POST,
        .handler = set_demo_field_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &set_demo_field_uri);

    httpd_uri_t set_demo_field_options_uri = {
        .uri = "/api/test/set-demo-field",
        .method = HTTP_OPTIONS,
        .handler = test_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &set_demo_field_options_uri);

    httpd_uri_t record_reset_reason_uri = {
        .uri = "/api/test/record-reset-reason",
        .method = HTTP_POST,
        .handler = record_reset_reason_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &record_reset_reason_uri);

    httpd_uri_t record_reset_reason_options_uri = {
        .uri = "/api/test/record-reset-reason",
        .method = HTTP_OPTIONS,
        .handler = test_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &record_reset_reason_options_uri);

    httpd_uri_t clear_error_history_uri = {
        .uri = "/api/test/clear-error-history",
        .method = HTTP_POST,
        .handler = clear_error_history_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &clear_error_history_uri);

    httpd_uri_t clear_error_history_options_uri = {
        .uri = "/api/test/clear-error-history",
        .method = HTTP_OPTIONS,
        .handler = test_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &clear_error_history_options_uri);

    httpd_uri_t notification_mock_uri = {
        .uri = "/api/test/notification-mock",
        .method = HTTP_POST,
        .handler = notification_mock_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &notification_mock_uri);

    httpd_uri_t notification_mock_options_uri = {
        .uri = "/api/test/notification-mock",
        .method = HTTP_OPTIONS,
        .handler = test_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &notification_mock_options_uri);

    httpd_uri_t notification_mock_reset_uri = {
        .uri = "/api/test/notification-mock-reset",
        .method = HTTP_POST,
        .handler = notification_mock_reset_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &notification_mock_reset_uri);

    httpd_uri_t notification_mock_reset_options_uri = {
        .uri = "/api/test/notification-mock-reset",
        .method = HTTP_OPTIONS,
        .handler = test_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &notification_mock_reset_options_uri);

    // --- Session lock endpoints ---

    httpd_uri_t lock_get_uri = {
        .uri = "/api/test/lock",
        .method = HTTP_GET,
        .handler = lock_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &lock_get_uri);

    httpd_uri_t lock_post_uri = {
        .uri = "/api/test/lock",
        .method = HTTP_POST,
        .handler = lock_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &lock_post_uri);

    httpd_uri_t lock_options_uri = {
        .uri = "/api/test/lock",
        .method = HTTP_OPTIONS,
        .handler = test_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &lock_options_uri);

    httpd_uri_t unlock_uri = {
        .uri = "/api/test/unlock",
        .method = HTTP_POST,
        .handler = unlock_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &unlock_uri);

    httpd_uri_t unlock_options_uri = {
        .uri = "/api/test/unlock",
        .method = HTTP_OPTIONS,
        .handler = test_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &unlock_options_uri);

    // Screenshot endpoint
    httpd_uri_t screenshot_uri = {
        .uri = "/api/test/screenshot",
        .method = HTTP_GET,
        .handler = screenshot_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &screenshot_uri);

    ESP_LOGI(TAG, "Test instrumentation endpoints registered");
}

#endif // CONFIG_TEST_ENDPOINTS
