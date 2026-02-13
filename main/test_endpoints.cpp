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
#include <cJSON.h>
#include <lvgl.h>
#include <bsp/m5stack_tab5.h>
#include "settings/settings_menu.h"
#include "i18n/i18n.h"

static const char* TAG = "test_api";

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

// Get a human-readable type name for an LVGL object
static const char* get_widget_type(lv_obj_t* obj)
{
    if (lv_obj_check_type(obj, &lv_label_class))       return "label";
    if (lv_obj_check_type(obj, &lv_button_class))      return "button";
    if (lv_obj_check_type(obj, &lv_switch_class))      return "switch";
    if (lv_obj_check_type(obj, &lv_checkbox_class))     return "checkbox";
    if (lv_obj_check_type(obj, &lv_slider_class))       return "slider";
    if (lv_obj_check_type(obj, &lv_dropdown_class))     return "dropdown";
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

// Recursively walk LVGL object tree and add widgets to JSON array
static void walk_tree(lv_obj_t* obj, cJSON* arr, int depth)
{
    if (!obj || depth > 20) return;  // prevent runaway recursion

    bool hidden = lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    const char* type = get_widget_type(obj);
    const char* text = get_widget_text(obj);

    // Report widgets that have text or are interactive (buttons, switches, etc.)
    bool is_interesting = (text != NULL) ||
                          strcmp(type, "button") == 0 ||
                          strcmp(type, "switch") == 0 ||
                          strcmp(type, "checkbox") == 0 ||
                          strcmp(type, "slider") == 0 ||
                          strcmp(type, "dropdown") == 0 ||
                          strcmp(type, "textarea") == 0;

    if (is_interesting && !hidden) {
        cJSON* w = cJSON_CreateObject();
        cJSON_AddStringToObject(w, "type", type);

        if (text) {
            cJSON_AddStringToObject(w, "text", text);

            // Include English key so tests can match language-independently
            const char* en = i18n_get_english(text);
            if (en && strcmp(en, text) != 0) {
                cJSON_AddStringToObject(w, "text_en", en);
            }
        }

        // Position and size
        lv_area_t coords;
        lv_obj_get_coords(obj, &coords);
        cJSON_AddNumberToObject(w, "x", coords.x1);
        cJSON_AddNumberToObject(w, "y", coords.y1);
        cJSON_AddNumberToObject(w, "w", lv_area_get_width(&coords));
        cJSON_AddNumberToObject(w, "h", lv_area_get_height(&coords));

        // State info
        if (lv_obj_check_type(obj, &lv_switch_class) || lv_obj_check_type(obj, &lv_checkbox_class)) {
            cJSON_AddBoolToObject(w, "checked", lv_obj_has_state(obj, LV_STATE_CHECKED));
        }
        if (lv_obj_has_state(obj, LV_STATE_DISABLED)) {
            cJSON_AddBoolToObject(w, "disabled", true);
        }

        // User data tag (if set)
        void* ud = lv_obj_get_user_data(obj);
        if (ud) {
            cJSON_AddStringToObject(w, "tag", (const char*)ud);
        }

        cJSON_AddItemToArray(arr, w);
    }

    // Recurse into children (only for visible containers)
    if (!hidden) {
        uint32_t count = lv_obj_get_child_count(obj);
        for (uint32_t i = 0; i < count; i++) {
            walk_tree(lv_obj_get_child(obj, i), arr, depth + 1);
        }
    }
}

// Find a label widget whose text matches (exact or contains)
// Returns the label itself, not its parent
static lv_obj_t* find_label_by_text(lv_obj_t* root, const char* exact, const char* contains, int depth)
{
    if (!root || depth > 20) return NULL;

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
    if (!root || depth > 20) return NULL;

    if (!lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) {
        void* ud = lv_obj_get_user_data(root);
        if (ud && strcmp((const char*)ud, tag) == 0) {
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
    if (settings_menu_is_visible()) return "settings";

    // Check active screen — try to identify by known patterns
    // For now return "main" as default; screens don't self-identify
    // This can be extended by having each screen register itself
    return "main";
}

// ============================================================================
// GET /api/test/ui-state
// ============================================================================

static esp_err_t ui_state_get_handler(httpd_req_t* req)
{
    set_json_content_type(req);

    cJSON* root = cJSON_CreateObject();
    cJSON* widgets = cJSON_CreateArray();

    if (!bsp_display_lock(1000)) {
        send_json_error(req, "503 Service Unavailable", "Could not acquire display lock");
        cJSON_Delete(root);
        return ESP_OK;
    }

    cJSON_AddStringToObject(root, "screen", get_screen_name());

    lv_obj_t* scr = lv_scr_act();
    walk_tree(scr, widgets, 0);

    bsp_display_unlock();

    cJSON_AddItemToObject(root, "widgets", widgets);
    cJSON_AddNumberToObject(root, "count", cJSON_GetArraySize(widgets));

    char* json = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "ui-state: screen=%s, %d widgets", get_screen_name(), cJSON_GetArraySize(widgets));
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

    // Fire the click event
    lv_obj_send_event(target, LV_EVENT_CLICKED, NULL);

    // Get info about what was clicked for the response
    const char* clicked_text = get_widget_text(found);
    const char* clicked_type = get_widget_type(target);

    bsp_display_unlock();

    // Send success response
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    cJSON_AddStringToObject(resp, "clicked_type", clicked_type);
    if (clicked_text) {
        cJSON_AddStringToObject(resp, "clicked_text", clicked_text);
    }

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

    ESP_LOGI(TAG, "Test instrumentation endpoints registered");
}

#endif // CONFIG_TEST_ENDPOINTS
