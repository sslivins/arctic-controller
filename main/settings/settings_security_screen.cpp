/*
 * Arctic Heat Pump Controller
 * Settings - Security screen
 *
 * Surfaces the physical setup code used to replace the factory administrator
 * password from the web interface. This flow is intentionally independent of
 * Home Assistant: securing the controller must not require any integration.
 */
#include "settings_security_screen.h"

#include "settings_common.h"
#include "settings_menu.h"
#include "auth_manager.h"
#include "setup_pairing.h"
#include "i18n/i18n.h"

#include <esp_log.h>
#include <mbedtls/platform_util.h>
#include <stdio.h>
#include <string.h>

static const char* TAG = "security_settings";

typedef struct {
    bool visible;
    bool code_shown;
    security_screen_config_t config;
    lv_obj_t* screen;
    lv_obj_t* status_label;
    lv_obj_t* description_label;
    lv_obj_t* code_title;
    lv_obj_t* code_label;
    lv_obj_t* countdown_label;
    lv_obj_t* code_btn;
    lv_obj_t* code_btn_label;
    lv_timer_t* timer;
    char setup_code[SETUP_PAIRING_CODE_LEN + 1];
} security_screen_state_t;

static security_screen_state_t state = {};

static void refresh_ui(void);
static void back_btn_cb(lv_event_t* event);
static void code_btn_cb(lv_event_t* event);

static lv_obj_t* create_button(
    lv_obj_t* parent,
    const char* text,
    lv_color_t color,
    lv_event_cb_t callback,
    const char* tag)
{
    lv_obj_t* button = lv_btn_create(parent);
    lv_obj_set_size(button, LV_PCT(100), 70);
    lv_obj_set_style_bg_color(button, color, LV_PART_MAIN);
    lv_obj_set_style_radius(button, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_set_user_data(button, (void*)tag);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(0x0d1117), LV_PART_MAIN);
    lv_obj_center(label);
    return button;
}

static void refresh_ui(void)
{
    if (!state.visible) {
        return;
    }

    const setup_pairing_status_t pairing = setup_pairing_get_status();
    const bool secured = !auth_mgr_credentials_change_required();

    lv_label_set_text(
        state.status_label,
        secured ? i18n_get(STR_SECURITY_SECURED)
                : i18n_get(STR_SECURITY_NOT_SECURED));
    lv_obj_set_style_text_color(
        state.status_label,
        secured ? COLOR_SUCCESS : COLOR_WARNING,
        LV_PART_MAIN);

    if (pairing.active && state.code_shown) {
        lv_label_set_text(
            state.description_label, i18n_get(STR_SECURITY_CODE_ACTIVE));
        lv_obj_clear_flag(state.code_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(state.code_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(state.countdown_label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(state.code_label, state.setup_code);

        char countdown[64];
        snprintf(
            countdown,
            sizeof(countdown),
            i18n_get(STR_SECURITY_EXPIRES_IN),
            pairing.remaining_seconds / 60,
            pairing.remaining_seconds % 60);
        lv_label_set_text(state.countdown_label, countdown);
        lv_label_set_text(
            state.code_btn_label, i18n_get(STR_SECURITY_HIDE_CODE));
        lv_obj_set_style_bg_color(
            state.code_btn, COLOR_WARNING, LV_PART_MAIN);
    } else {
        if (state.code_shown) {
            state.code_shown = false;
            mbedtls_platform_zeroize(
                state.setup_code, sizeof(state.setup_code));
        }
        lv_label_set_text(state.code_label, "");
        lv_label_set_text(
            state.description_label,
            secured ? i18n_get(STR_SECURITY_SECURED_DESC)
                    : i18n_get(STR_SECURITY_NOT_SECURED_DESC));
        lv_obj_add_flag(state.code_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(state.code_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(state.countdown_label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(
            state.code_btn_label, i18n_get(STR_SECURITY_SHOW_CODE));
        lv_obj_set_style_bg_color(state.code_btn, COLOR_ACCENT, LV_PART_MAIN);
    }
}

static void timer_cb(lv_timer_t* timer)
{
    (void)timer;
    refresh_ui();
}

static void back_btn_cb(lv_event_t* event)
{
    (void)event;
    security_screen_close();
    if (state.config.on_back) {
        state.config.on_back();
    }
}

static void code_btn_cb(lv_event_t* event)
{
    (void)event;
    const setup_pairing_status_t pairing = setup_pairing_get_status();
    if (pairing.active && state.code_shown) {
        setup_pairing_cancel();
        state.code_shown = false;
        mbedtls_platform_zeroize(state.setup_code, sizeof(state.setup_code));
    } else if (!setup_pairing_start(state.setup_code)) {
        lv_label_set_text(
            state.description_label, i18n_get(STR_SECURITY_FAILED));
        return;
    } else {
        state.code_shown = true;
    }
    refresh_ui();
}

void security_screen_create(const security_screen_config_t* config)
{
    if (state.visible) {
        return;
    }

    memset(&state, 0, sizeof(state));
    if (config) {
        state.config = *config;
    }

    state.screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(state.screen, COLOR_BG, LV_PART_MAIN);
    disable_scrolling(state.screen);

    const int32_t screen_height =
        lv_display_get_vertical_resolution(lv_display_get_default());
    const int32_t header_height =
        screen_height * HEADER_HEIGHT_PCT / 100;

    lv_obj_t* header = lv_obj_create(state.screen);
    lv_obj_set_size(header, LV_PCT(100), header_height);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, COLOR_HEADER, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN);
    disable_scrolling(header);

    lv_obj_t* back = lv_btn_create(header);
    lv_obj_set_size(back, 50, 50);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 15, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x3d4f6f), LV_PART_MAIN);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_user_data(back, (void*)"security_back");
    lv_obj_add_event_cb(back, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* back_label = lv_label_create(back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_label, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(back_label, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_center(back_label);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, i18n_get(STR_SECURITY_TITLE));
    lv_obj_set_style_text_font(title, UI_FONT_HEADER, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_center(title);

    lv_obj_t* content = lv_obj_create(state.screen);
    lv_obj_set_size(content, LV_PCT(100), screen_height - header_height);
    lv_obj_set_pos(content, 0, header_height);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(content, 20, LV_PART_MAIN);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 16, LV_PART_MAIN);
    disable_scrolling(content);

    lv_obj_t* card = lv_obj_create(content);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 22, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 12, LV_PART_MAIN);
    disable_scrolling(card);

    state.status_label = lv_label_create(card);
    lv_obj_set_user_data(state.status_label, (void*)"security_status");
    lv_obj_set_style_text_font(state.status_label, FONT_LARGE, LV_PART_MAIN);

    state.description_label = lv_label_create(card);
    lv_obj_set_width(state.description_label, LV_PCT(100));
    lv_label_set_long_mode(state.description_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(
        state.description_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(
        state.description_label, COLOR_TEXT_DIM, LV_PART_MAIN);

    state.code_title = lv_label_create(card);
    lv_label_set_text(state.code_title, i18n_get(STR_SECURITY_CODE_LABEL));
    lv_obj_set_style_text_font(state.code_title, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(
        state.code_title, COLOR_TEXT_DIM, LV_PART_MAIN);

    state.code_label = lv_label_create(card);
    lv_obj_set_user_data(state.code_label, (void*)"security_code");
    lv_obj_set_width(state.code_label, LV_PCT(100));
    lv_obj_set_style_text_font(
        state.code_label, &montserrat_40_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.code_label, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(state.code_label, 12, LV_PART_MAIN);
    lv_obj_set_style_text_align(
        state.code_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    state.countdown_label = lv_label_create(card);
    lv_obj_set_user_data(state.countdown_label, (void*)"security_countdown");
    lv_obj_set_width(state.countdown_label, LV_PCT(100));
    lv_obj_set_style_text_font(
        state.countdown_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(
        state.countdown_label, COLOR_WARNING, LV_PART_MAIN);
    lv_obj_set_style_text_align(
        state.countdown_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    state.code_btn = create_button(
        content,
        i18n_get(STR_SECURITY_SHOW_CODE),
        COLOR_ACCENT,
        code_btn_cb,
        "security_show_code");
    state.code_btn_label = lv_obj_get_child(state.code_btn, 0);

    state.visible = true;
    state.timer = lv_timer_create(timer_cb, 1000, NULL);
    refresh_ui();
    lv_screen_load_anim(
        state.screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}

void security_screen_close(void)
{
    if (!state.visible) {
        return;
    }
    if (state.timer) {
        lv_timer_delete(state.timer);
        state.timer = NULL;
    }
    if (state.code_shown) {
        setup_pairing_cancel();
    }
    mbedtls_platform_zeroize(state.setup_code, sizeof(state.setup_code));
    state.visible = false;
    state.screen = NULL;
}

bool security_screen_is_visible(void)
{
    return state.visible;
}
