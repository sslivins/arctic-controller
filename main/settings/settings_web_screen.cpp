/*
 * Arctic Heat Pump Controller
 * Settings - Web Interface Screen Implementation (iOS-style full screen)
 *
 * Shows a QR code and the plain-text URL of the device's browser dashboard so
 * a phone can scan it (or a user can type it) to open the web interface.
 * Portrait mode: 720x1280
 */
#include "settings_web_screen.h"
#include "settings_menu.h"
#include "settings_common.h"
#include "i18n/i18n.h"
#include "fonts/fonts.h"
#include "../api_server.h"
#include "../tls_manager.h"
#include <esp_log.h>
#include <string.h>
#include <stdio.h>

static const char* TAG = "web_screen";

// ============================================================================
// Screen State
// ============================================================================

typedef struct {
    bool visible;
    web_screen_config_t config;

    lv_obj_t* screen;
    lv_obj_t* header;
    lv_obj_t* back_btn;
    lv_obj_t* content;
} web_screen_state_t;

static web_screen_state_t s_state = {};

// ============================================================================
// Forward Declarations
// ============================================================================

static void create_header(void);
static void create_content(void);
static void back_btn_cb(lv_event_t* e);

// Builds the dashboard URL (e.g. "https://arctic-ab12.local").
// Returns false if no hostname is available yet (e.g. Wi-Fi not connected).
static bool build_dashboard_url(char* buf, size_t size)
{
    const char* host = api_server_get_hostname();
    if (!host || host[0] == '\0') {
        return false;
    }
    snprintf(buf, size, "%s://%s.local",
             tls_mgr_is_https_active() ? "https" : "http", host);
    return true;
}

// ============================================================================
// Public API
// ============================================================================

void web_screen_create(const web_screen_config_t* config)
{
    if (s_state.visible) {
        ESP_LOGW(TAG, "Web Interface screen already visible");
        return;
    }

    ESP_LOGI(TAG, "Creating Web Interface screen");

    memset(&s_state, 0, sizeof(s_state));
    if (config) {
        s_state.config = *config;
    }

    s_state.screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_state.screen, COLOR_BG, LV_PART_MAIN);
    disable_scrolling(s_state.screen);

    create_header();
    create_content();

    lv_screen_load_anim(s_state.screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
    s_state.visible = true;
}

void web_screen_close(void)
{
    if (!s_state.visible) return;

    ESP_LOGI(TAG, "Closing Web Interface screen");

    s_state.visible = false;
    s_state.screen = NULL;
}

bool web_screen_is_visible(void)
{
    return s_state.visible;
}

// ============================================================================
// UI Creation
// ============================================================================

static void create_header(void)
{
    lv_display_t* disp = lv_display_get_default();
    int32_t header_height = lv_display_get_vertical_resolution(disp) * HEADER_HEIGHT_PCT / 100;

    s_state.header = lv_obj_create(s_state.screen);
    lv_obj_set_size(s_state.header, LV_PCT(100), header_height);
    lv_obj_align(s_state.header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_state.header, COLOR_HEADER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_state.header, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_state.header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_state.header, 15, LV_PART_MAIN);
    disable_scrolling(s_state.header);

    // Back button with circular background
    s_state.back_btn = lv_btn_create(s_state.header);
    lv_obj_set_size(s_state.back_btn, 50, 50);
    lv_obj_align(s_state.back_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_state.back_btn, lv_color_hex(0x3d4f6f), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_state.back_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_state.back_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_state.back_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_state.back_btn, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_state.back_btn, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_state.back_btn, LV_OPA_50, LV_PART_MAIN);
    lv_obj_add_event_cb(s_state.back_btn, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_user_data(s_state.back_btn, (void*)"web_back");

    lv_obj_t* back_icon = lv_label_create(s_state.back_btn);
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_icon, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(back_icon, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_center(back_icon);

    // Title
    lv_obj_t* title = lv_label_create(s_state.header);
    lv_label_set_text(title, i18n_get(STR_WEB_TITLE));
    lv_obj_set_style_text_font(title, UI_FONT_HEADER, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
}

static void create_content(void)
{
    lv_display_t* disp = lv_display_get_default();
    int32_t screen_height = lv_display_get_vertical_resolution(disp);
    int32_t header_height = screen_height * HEADER_HEIGHT_PCT / 100;

    s_state.content = lv_obj_create(s_state.screen);
    lv_obj_set_size(s_state.content, LV_PCT(100), screen_height - header_height);
    lv_obj_set_pos(s_state.content, 0, header_height);
    lv_obj_set_style_bg_opa(s_state.content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_state.content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_state.content, 20, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_state.content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_state.content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_state.content, 20, LV_PART_MAIN);
    disable_scrolling(s_state.content);

    char url[64];
    bool have_url = build_dashboard_url(url, sizeof(url));

    if (!have_url) {
        // No hostname yet (Wi-Fi not connected) — show a helpful message.
        lv_obj_t* msg = lv_label_create(s_state.content);
        lv_label_set_text(msg, i18n_get(STR_WEB_URL_UNAVAILABLE));
        lv_obj_set_width(msg, LV_PCT(90));
        lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_font(msg, FONT_NORMAL, LV_PART_MAIN);
        lv_obj_set_style_text_color(msg, COLOR_TEXT_DIM, LV_PART_MAIN);
        lv_obj_set_user_data(msg, (void*)"web_url");
        return;
    }

    // QR code on a white plate so cameras read it reliably.
    lv_obj_t* qr_plate = lv_obj_create(s_state.content);
    lv_obj_set_size(qr_plate, 360, 360);
    lv_obj_set_style_bg_color(qr_plate, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(qr_plate, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(qr_plate, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(qr_plate, 0, LV_PART_MAIN);
    disable_scrolling(qr_plate);

    lv_obj_t* qr = lv_qrcode_create(qr_plate);
    lv_qrcode_set_size(qr, 320);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, url, strlen(url));
    lv_obj_center(qr);
    lv_obj_set_user_data(qr, (void*)"web_qrcode");

    // Scan hint
    lv_obj_t* hint = lv_label_create(s_state.content);
    lv_label_set_text(hint, i18n_get(STR_WEB_SCAN_HINT));
    lv_obj_set_width(hint, LV_PCT(90));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, COLOR_TEXT, LV_PART_MAIN);

    // URL card: manual-entry label + the address in plain text
    lv_obj_t* url_card = lv_obj_create(s_state.content);
    lv_obj_set_size(url_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(url_card, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_width(url_card, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(url_card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(url_card, 20, LV_PART_MAIN);
    lv_obj_set_flex_flow(url_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(url_card, 10, LV_PART_MAIN);
    disable_scrolling(url_card);

    lv_obj_t* url_label = lv_label_create(url_card);
    lv_label_set_text(url_label, i18n_get(STR_WEB_URL_LABEL));
    lv_obj_set_width(url_label, LV_PCT(100));
    lv_label_set_long_mode(url_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(url_label, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(url_label, COLOR_TEXT_DIM, LV_PART_MAIN);

    lv_obj_t* url_value = lv_label_create(url_card);
    lv_label_set_text(url_value, url);
    lv_obj_set_width(url_value, LV_PCT(100));
    lv_label_set_long_mode(url_value, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(url_value, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(url_value, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_user_data(url_value, (void*)"web_url");
}

// ============================================================================
// Event Handlers
// ============================================================================

static void back_btn_cb(lv_event_t* e)
{
    (void)e;
    ESP_LOGI(TAG, "Back button clicked");

    void (*on_back)(void) = s_state.config.on_back;

    if (on_back) {
        on_back();
    } else {
        settings_menu_show();
    }

    web_screen_close();
}
