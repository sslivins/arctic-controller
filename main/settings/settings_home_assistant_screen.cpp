/*
 * Arctic Heat Pump Controller
 * Settings - Home Assistant pairing screen
 */
#include "settings_home_assistant_screen.h"

#include "settings_common.h"
#include "settings_menu.h"
#include "auth_manager.h"
#include "ha_pairing.h"
#include "ha_integration.h"
#include "i18n/i18n.h"

#include <esp_log.h>
#include <mbedtls/platform_util.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char* TAG = "ha_settings";

// The pairing screen shows the exact name Home Assistant will display for the
// device so a user with more than one controller can tell them apart. This
// must stay in sync with the title produced by the hass-macon integration
// (custom_components/macon/config_flow.py): "Macon Heat Pump Controller <LAST4>"
// where <LAST4> is the upper-cased last 4 characters of the device_id.
static const char* HA_DEVICE_NAME_PREFIX = "Macon Heat Pump Controller";

typedef struct {
    bool visible;
    bool pairing_started;
    home_assistant_screen_config_t config;
    lv_obj_t* screen;
    lv_obj_t* status_label;
    lv_obj_t* description_label;
    lv_obj_t* code_title;
    lv_obj_t* code_label;
    lv_obj_t* countdown_label;
    lv_obj_t* pairing_btn;
    lv_obj_t* pairing_btn_label;
    lv_obj_t* revoke_btn;
    lv_obj_t* revoke_overlay;
    lv_timer_t* timer;
    char pairing_code[HA_PAIRING_CODE_LEN + 1];
} home_assistant_screen_state_t;

static home_assistant_screen_state_t state = {};

static void refresh_ui(void);
static void back_btn_cb(lv_event_t* event);
static void pairing_btn_cb(lv_event_t* event);
static void revoke_btn_cb(lv_event_t* event);
static void revoke_confirm_cb(lv_event_t* event);
static void revoke_cancel_cb(lv_event_t* event);

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

    const ha_pairing_status_t pairing = ha_pairing_get_status();
    const bool paired = auth_mgr_has_integration_token();

    lv_label_set_text(
        state.status_label,
        paired ? i18n_get(STR_HA_PAIRED) : i18n_get(STR_HA_NOT_PAIRED));
    lv_obj_set_style_text_color(
        state.status_label,
        paired ? COLOR_SUCCESS : COLOR_WARNING,
        LV_PART_MAIN);

    if (pairing.active && state.pairing_started) {
        lv_label_set_text(
            state.description_label,
            i18n_get(STR_HA_PAIRING_ACTIVE));
        lv_obj_clear_flag(state.code_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(state.code_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(state.countdown_label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(state.code_label, state.pairing_code);

        char countdown[64];
        snprintf(
            countdown,
            sizeof(countdown),
            i18n_get(STR_HA_EXPIRES_IN),
            pairing.remaining_seconds / 60,
            pairing.remaining_seconds % 60);
        lv_label_set_text(state.countdown_label, countdown);
        lv_label_set_text(
            state.pairing_btn_label,
            i18n_get(STR_HA_CANCEL_PAIRING));
        lv_obj_set_style_bg_color(
            state.pairing_btn, COLOR_WARNING, LV_PART_MAIN);
    } else {
        if (state.pairing_started) {
            state.pairing_started = false;
            mbedtls_platform_zeroize(
                state.pairing_code, sizeof(state.pairing_code));
        }
        lv_label_set_text(state.code_label, "");
        lv_label_set_text(
            state.description_label,
            i18n_get(STR_HA_PAIRING_DESCRIPTION));
        lv_obj_add_flag(state.code_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(state.code_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(state.countdown_label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(
            state.pairing_btn_label,
            i18n_get(STR_HA_START_PAIRING));
        lv_obj_set_style_bg_color(
            state.pairing_btn, COLOR_ACCENT, LV_PART_MAIN);
    }

    if (paired) {
        lv_obj_clear_flag(state.revoke_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(state.revoke_btn, LV_OBJ_FLAG_HIDDEN);
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
    home_assistant_screen_close();
    if (state.config.on_back) {
        state.config.on_back();
    }
}

static void pairing_btn_cb(lv_event_t* event)
{
    (void)event;
    const ha_pairing_status_t pairing = ha_pairing_get_status();
    if (pairing.active && state.pairing_started) {
        ha_pairing_cancel();
        state.pairing_started = false;
        mbedtls_platform_zeroize(
            state.pairing_code, sizeof(state.pairing_code));
    } else if (!ha_pairing_start(state.pairing_code)) {
        lv_label_set_text(
            state.description_label, i18n_get(STR_HA_PAIRING_FAILED));
        return;
    } else {
        state.pairing_started = true;
    }
    refresh_ui();
}

static void dismiss_revoke_overlay(void)
{
    if (state.revoke_overlay) {
        lv_obj_delete(state.revoke_overlay);
        state.revoke_overlay = NULL;
    }
}

static void revoke_confirm_cb(lv_event_t* event)
{
    (void)event;
    if (!auth_mgr_revoke_integration_token()) {
        ESP_LOGE(TAG, "Failed to revoke Home Assistant credential");
    }
    ha_pairing_cancel();
    dismiss_revoke_overlay();
    refresh_ui();
}

static void revoke_cancel_cb(lv_event_t* event)
{
    (void)event;
    dismiss_revoke_overlay();
}

static void revoke_btn_cb(lv_event_t* event)
{
    (void)event;
    if (state.revoke_overlay) {
        return;
    }

    state.revoke_overlay = lv_obj_create(state.screen);
    lv_obj_set_size(state.revoke_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(
        state.revoke_overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(
        state.revoke_overlay, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.revoke_overlay, 0, LV_PART_MAIN);
    disable_scrolling(state.revoke_overlay);

    lv_obj_t* panel = lv_obj_create(state.revoke_overlay);
    lv_obj_set_size(panel, 620, 330);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 25, LV_PART_MAIN);
    disable_scrolling(panel);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, i18n_get(STR_HA_REVOKE_CONFIRM));
    lv_obj_set_style_text_font(title, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t* description = lv_label_create(panel);
    lv_label_set_text(description, i18n_get(STR_HA_REVOKE_DESCRIPTION));
    lv_obj_set_width(description, 540);
    lv_label_set_long_mode(description, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(description, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(description, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_align(description, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(description, LV_ALIGN_CENTER, 0, -15);

    lv_obj_t* cancel = lv_btn_create(panel);
    lv_obj_set_size(cancel, 250, 65);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(cancel, revoke_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* cancel_label = lv_label_create(cancel);
    lv_label_set_text(cancel_label, i18n_get(STR_CANCEL));
    lv_obj_set_style_text_font(cancel_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_center(cancel_label);

    lv_obj_t* confirm = lv_btn_create(panel);
    lv_obj_set_size(confirm, 250, 65);
    lv_obj_align(confirm, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(confirm, COLOR_ERROR, LV_PART_MAIN);
    lv_obj_add_event_cb(confirm, revoke_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* confirm_label = lv_label_create(confirm);
    lv_label_set_text(confirm_label, i18n_get(STR_HA_REVOKE_ACTION));
    lv_obj_set_style_text_font(confirm_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_center(confirm_label);
}

void home_assistant_screen_create(
    const home_assistant_screen_config_t* config)
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
    lv_obj_set_user_data(back, (void*)"home_assistant_back");
    lv_obj_add_event_cb(back, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* back_label = lv_label_create(back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_label, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(back_label, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_center(back_label);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, i18n_get(STR_HA_TITLE));
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
    lv_obj_set_user_data(
        state.status_label, (void*)"home_assistant_status");
    lv_obj_set_style_text_font(state.status_label, FONT_LARGE, LV_PART_MAIN);

    lv_obj_t* device_name_label = lv_label_create(card);
    lv_obj_set_user_data(
        device_name_label, (void*)"home_assistant_device_name");
    lv_obj_set_width(device_name_label, LV_PCT(100));
    lv_label_set_long_mode(device_name_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(device_name_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(device_name_label, COLOR_TEXT, LV_PART_MAIN);
    {
        const char* device_id = arctic::ha::deviceId();
        const size_t id_len = strlen(device_id);
        char last4[5] = {};
        const char* suffix =
            id_len >= 4 ? device_id + (id_len - 4) : device_id;
        for (size_t i = 0; i < 4 && suffix[i] != '\0'; ++i) {
            last4[i] = (char)toupper((unsigned char)suffix[i]);
        }
        char device_name[64];
        snprintf(
            device_name, sizeof(device_name), "%s %s",
            HA_DEVICE_NAME_PREFIX, last4);
        lv_label_set_text(device_name_label, device_name);
    }

    state.description_label = lv_label_create(card);
    lv_obj_set_width(state.description_label, LV_PCT(100));
    lv_label_set_long_mode(
        state.description_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(
        state.description_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(
        state.description_label, COLOR_TEXT_DIM, LV_PART_MAIN);

    state.code_title = lv_label_create(card);
    lv_label_set_text(state.code_title, i18n_get(STR_HA_PAIRING_CODE));
    lv_obj_set_style_text_font(state.code_title, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.code_title, COLOR_TEXT_DIM, LV_PART_MAIN);

    state.code_label = lv_label_create(card);
    lv_obj_set_user_data(
        state.code_label, (void*)"home_assistant_code");
    lv_obj_set_width(state.code_label, LV_PCT(100));
    lv_obj_set_style_text_font(
        state.code_label, &montserrat_40_latin, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.code_label, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(state.code_label, 12, LV_PART_MAIN);
    lv_obj_set_style_text_align(
        state.code_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    state.countdown_label = lv_label_create(card);
    lv_obj_set_user_data(
        state.countdown_label, (void*)"home_assistant_countdown");
    lv_obj_set_width(state.countdown_label, LV_PCT(100));
    lv_obj_set_style_text_font(
        state.countdown_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(
        state.countdown_label, COLOR_WARNING, LV_PART_MAIN);
    lv_obj_set_style_text_align(
        state.countdown_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    state.pairing_btn = create_button(
        content,
        i18n_get(STR_HA_START_PAIRING),
        COLOR_ACCENT,
        pairing_btn_cb,
        "home_assistant_pair");
    state.pairing_btn_label = lv_obj_get_child(state.pairing_btn, 0);

    state.revoke_btn = create_button(
        content,
        i18n_get(STR_HA_REVOKE),
        COLOR_ERROR,
        revoke_btn_cb,
        "home_assistant_revoke");

    state.visible = true;
    state.timer = lv_timer_create(timer_cb, 1000, NULL);
    refresh_ui();
    lv_screen_load_anim(
        state.screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}

void home_assistant_screen_close(void)
{
    if (!state.visible) {
        return;
    }
    if (state.timer) {
        lv_timer_delete(state.timer);
        state.timer = NULL;
    }
    if (state.pairing_started) {
        ha_pairing_cancel();
    }
    mbedtls_platform_zeroize(
        state.pairing_code, sizeof(state.pairing_code));
    state.visible = false;
    state.screen = NULL;
}

bool home_assistant_screen_is_visible(void)
{
    return state.visible;
}
