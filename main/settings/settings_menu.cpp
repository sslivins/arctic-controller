/*
 * Arctic Heat Pump Controller
 * Settings Menu - iOS-style settings list
 * 
 * Main settings screen with a list of setting categories.
 * Each row opens a full-screen sub-setting when tapped.
 */
#include "sdkconfig.h"
#include "settings_menu.h"
#include "settings_wifi_screen.h"
#include "settings_firmware_screen.h"
#include "settings_time_screen.h"
#include "settings_language_screen.h"
#include "settings_display_screen.h"
#include "settings_home_assistant_screen.h"
#include "settings_security_screen.h"
#include "settings_web_screen.h"
#include "settings_types.h"  // For settings_wifi_network_t
#include "../ui_common.h"  // For ui_create_close_button
#include "../app_preferences.h"
#include "../factory_reset.h"
#include "../system_restart.h"
#include "../heatpump_screen.h"
#include "i18n/i18n.h"
#include "fonts/fonts.h"
#include "wifi_manager.h"
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

static const char* TAG = "settings_menu";

// ============================================================================
// Colors and Styles
// ============================================================================

#define COLOR_BG            lv_color_hex(0x1a1a2e)
#define COLOR_CARD          lv_color_hex(0x16213e)
#define COLOR_ROW           lv_color_hex(0x1e2a4a)
#define COLOR_ROW_PRESSED   lv_color_hex(0x2a3a5a)
#define COLOR_ACCENT        lv_color_hex(0x00d4ff)
#define COLOR_TEXT          lv_color_hex(0xffffff)
#define COLOR_TEXT_DIM      lv_color_hex(0x888888)
#define COLOR_DIVIDER       lv_color_hex(0x2a3a5a)

#define FONT_NORMAL   &montserrat_24_latin
#define FONT_LARGE    &montserrat_24_latin

// ============================================================================
// Menu State
// ============================================================================

typedef enum {
    SETTINGS_WIFI,
    SETTINGS_FIRMWARE,
    SETTINGS_TIME,
    SETTINGS_LANGUAGE,
    SETTINGS_DISPLAY,
    SETTINGS_HOME_ASSISTANT,
    SETTINGS_SECURITY,
    SETTINGS_WEB,
    SETTINGS_COUNT
} settings_item_t;

typedef struct {
    bool visible;
    settings_menu_config_t config;
    lv_obj_t* screen;
    lv_obj_t* header;
    lv_obj_t* list_container;
    lv_obj_t* rows[SETTINGS_COUNT];
    lv_obj_t* wifi_status_label;  // Shows connected SSID or "Not connected"
    
    // Toggle switches
    lv_obj_t* demo_mode_switch;
    lv_obj_t* temp_unit_switch;
    
    // Temperature unit label pointers (for switch callback)
    lv_obj_t* celsius_label;
    lv_obj_t* fahrenheit_label;
    
    // Reboot confirmation overlay
    lv_obj_t* reboot_overlay;

    // Factory reset confirmation overlay
    lv_obj_t* factory_reset_overlay;
    lv_obj_t* factory_reset_confirm_btn;
    lv_obj_t* factory_reset_confirm_label;
    
    // Track which sub-screen is active
    settings_item_t active_sub_screen;
    bool sub_screen_active;
} menu_state_t;

static menu_state_t state = {};

// ============================================================================
// Forward Declarations
// ============================================================================

static void create_header(void);
static void create_menu_list(void);
static void close_btn_event_cb(lv_event_t* e);
static void row_click_cb(lv_event_t* e);
#if CONFIG_DEMO_MODE
static void demo_mode_switch_cb(lv_event_t* e);
#endif
static void temp_unit_switch_cb(lv_event_t* e);
static void show_reboot_confirmation(void);
static void reboot_confirm_cb(lv_event_t* e);
static void reboot_cancel_cb(lv_event_t* e);
static void show_factory_reset_confirmation(void);
static void factory_reset_confirm_cb(lv_event_t* e);
static void factory_reset_cancel_cb(lv_event_t* e);

// ============================================================================
// Helper Functions
// ============================================================================

static void disable_scrolling(lv_obj_t* obj)
{
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

static lv_obj_t* create_settings_row(lv_obj_t* parent, const char* icon, 
                                       const char* label, const char* tag)
{
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 88);
    lv_obj_set_style_bg_color(row, COLOR_ROW, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 20, LV_PART_MAIN);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    disable_scrolling(row);
    
    // Pressed state
    lv_obj_set_style_bg_color(row, COLOR_ROW_PRESSED, LV_PART_MAIN | (lv_style_selector_t)LV_STATE_PRESSED);
    
    // Store string tag in user data for test automation
    lv_obj_set_user_data(row, (void*)tag);
    lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, NULL);
    
    // Icon on left
    lv_obj_t* icon_label = lv_label_create(row);
    lv_label_set_text(icon_label, icon);
    lv_obj_set_style_text_font(icon_label, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon_label, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(icon_label, LV_ALIGN_LEFT_MID, 0, 0);
    
    // Label
    lv_obj_t* text_label = lv_label_create(row);
    lv_label_set_text(text_label, label);
    lv_obj_set_style_text_font(text_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(text_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(text_label, LV_ALIGN_LEFT_MID, 45, 0);
    
    // Chevron on right (>)
    lv_obj_t* chevron = lv_label_create(row);
    lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(chevron, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(chevron, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, 0, 0);
    
    return row;
}

static lv_obj_t* create_toggle_row(lv_obj_t* parent, const char* icon, 
                                    const char* label, bool initial_state,
                                    lv_obj_t** switch_out, lv_event_cb_t switch_cb)
{
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 88);
    lv_obj_set_style_bg_color(row, COLOR_ROW, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 20, LV_PART_MAIN);
    disable_scrolling(row);
    
    // Icon on left
    lv_obj_t* icon_label = lv_label_create(row);
    lv_label_set_text(icon_label, icon);
    lv_obj_set_style_text_font(icon_label, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon_label, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_align(icon_label, LV_ALIGN_LEFT_MID, 0, 0);
    
    // Label
    lv_obj_t* text_label = lv_label_create(row);
    lv_label_set_text(text_label, label);
    lv_obj_set_style_text_font(text_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(text_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(text_label, LV_ALIGN_LEFT_MID, 45, 0);
    
    // Switch on right
    lv_obj_t* sw = lv_switch_create(row);
    lv_obj_set_size(sw, 80, 40);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x4caf50), 
        static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | static_cast<lv_style_selector_t>(LV_STATE_CHECKED));
    
    if (initial_state) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    
    lv_obj_add_event_cb(sw, switch_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    
    if (switch_out) {
        *switch_out = sw;
    }
    
    return row;
}

// ============================================================================
// Event Handlers
// ============================================================================

#if CONFIG_DEMO_MODE
static void demo_mode_switch_cb(lv_event_t* e)
{
    lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    app_prefs_set_demo_mode(on);
    heatpump_screen_set_demo_banner(on);
    ESP_LOGI(TAG, "Demo mode %s — showing reboot confirmation", on ? "enabled" : "disabled");
    show_reboot_confirmation();
}
#endif  // CONFIG_DEMO_MODE

// ============================================================================
// Reboot Confirmation Panel
// ============================================================================

static void dismiss_reboot_overlay(void)
{
    if (state.reboot_overlay) {
        lv_obj_delete(state.reboot_overlay);
        state.reboot_overlay = NULL;
    }
}

static void reboot_confirm_cb(lv_event_t* e)
{
    (void)e;
    ESP_LOGI(TAG, "User confirmed restart");
    system_safe_restart();
}

static void reboot_cancel_cb(lv_event_t* e)
{
    (void)e;
    // Revert the preference and switch state
    bool current = app_prefs_is_demo_mode();
    bool reverted = !current;
    app_prefs_set_demo_mode(reverted);
    heatpump_screen_set_demo_banner(reverted);

    if (state.demo_mode_switch) {
        if (reverted) {
            lv_obj_add_state(state.demo_mode_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(state.demo_mode_switch, LV_STATE_CHECKED);
        }
    }
    ESP_LOGI(TAG, "Reboot cancelled — demo mode reverted to %s", reverted ? "on" : "off");
    dismiss_reboot_overlay();
}

static void show_reboot_confirmation(void)
{
    if (!state.screen) return;

    // Clean up previous overlay if any
    dismiss_reboot_overlay();

    // --- Semi-transparent overlay covering the entire screen ---
    lv_obj_t* overlay = lv_obj_create(state.screen);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_50, LV_PART_MAIN);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);   // absorb taps
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(overlay, (void*)"reboot_overlay");
    state.reboot_overlay = overlay;

    // --- Bottom panel ---
    lv_obj_t* panel = lv_obj_create(overlay);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(panel, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 32, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(panel, 48, LV_PART_MAIN);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(panel, 12, LV_PART_MAIN);
    lv_obj_set_user_data(panel, (void*)"reboot_panel");

    // "Demo mode changed."
    lv_obj_t* line1 = lv_label_create(panel);
    lv_label_set_text(line1, i18n_get(STR_DEMO_MODE_CHANGED));
    lv_obj_set_style_text_font(line1, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(line1, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_align(line1, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // "Restart required to take effect."
    lv_obj_t* line2 = lv_label_create(panel);
    lv_label_set_text(line2, i18n_get(STR_RESTART_REQUIRED));
    lv_obj_set_style_text_font(line2, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(line2, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_align(line2, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // Button row
    lv_obj_t* btn_row = lv_obj_create(panel);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_top(btn_row, 12, LV_PART_MAIN);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    // Cancel button
    lv_obj_t* cancel_btn = lv_button_create(btn_row);
    lv_obj_set_size(cancel_btn, 200, 64);
    lv_obj_set_style_bg_color(cancel_btn, COLOR_ROW, LV_PART_MAIN);
    lv_obj_set_style_radius(cancel_btn, 12, LV_PART_MAIN);
    lv_obj_set_user_data(cancel_btn, (void*)"reboot_cancel");
    lv_obj_add_event_cb(cancel_btn, reboot_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, i18n_get(STR_CANCEL));
    lv_obj_set_style_text_font(cancel_lbl, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(cancel_lbl, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_center(cancel_lbl);

    // Restart button
    lv_obj_t* restart_btn = lv_button_create(btn_row);
    lv_obj_set_size(restart_btn, 200, 64);
    lv_obj_set_style_bg_color(restart_btn, COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_radius(restart_btn, 12, LV_PART_MAIN);
    lv_obj_set_user_data(restart_btn, (void*)"reboot_confirm");
    lv_obj_add_event_cb(restart_btn, reboot_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* restart_lbl = lv_label_create(restart_btn);
    lv_label_set_text(restart_lbl, i18n_get(STR_RESTART));
    lv_obj_set_style_text_font(restart_lbl, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(restart_lbl, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_center(restart_lbl);

    // Position panel at the bottom and animate it sliding up
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, 300);  // start off-screen
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, panel);
    lv_anim_set_values(&anim, 300, 0);
    lv_anim_set_duration(&anim, 300);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, [](void* obj, int32_t v) {
        lv_obj_align((lv_obj_t*)obj, LV_ALIGN_BOTTOM_MID, 0, v);
    });
    lv_anim_start(&anim);
}

static void temp_unit_switch_cb(lv_event_t* e)
{
    lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
    bool fahrenheit = lv_obj_has_state(sw, LV_STATE_CHECKED);
    app_prefs_set_temp_unit(fahrenheit ? TEMP_UNIT_FAHRENHEIT : TEMP_UNIT_CELSIUS);
    
    // Update the label colors to show which is active
    if (state.celsius_label && state.fahrenheit_label) {
        lv_obj_set_style_text_color(state.celsius_label, fahrenheit ? COLOR_TEXT_DIM : COLOR_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(state.fahrenheit_label, fahrenheit ? COLOR_TEXT : COLOR_TEXT_DIM, LV_PART_MAIN);
    }
    
    ESP_LOGI(TAG, "Temperature unit set to %s", fahrenheit ? "Fahrenheit" : "Celsius");
}

static void close_btn_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (state.config.on_close) {
            state.config.on_close();
        }
        settings_menu_close();
    }
}

static void row_click_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    
    lv_obj_t* row = (lv_obj_t*)lv_event_get_target(e);
    const char* tag = (const char*)lv_obj_get_user_data(row);
    if (!tag) return;
    
    ESP_LOGI(TAG, "Settings row clicked: %s", tag);
    
    if (strcmp(tag, "settings_wifi") == 0) {
        ESP_LOGI(TAG, "Opening WiFi settings...");
        wifi_screen_config_t wifi_cfg = {
            .on_wifi_scan = state.config.on_wifi_scan,
            .on_wifi_connect = state.config.on_wifi_connect,
            .on_wifi_disconnect = state.config.on_wifi_disconnect,
            .on_back = settings_menu_show,
            .use_instant_transition = false,
        };
        wifi_screen_create(&wifi_cfg);
        state.sub_screen_active = true;
        state.active_sub_screen = SETTINGS_WIFI;
    } else if (strcmp(tag, "settings_firmware") == 0) {
        ESP_LOGI(TAG, "Opening Firmware settings...");
        firmware_screen_config_t fw_cfg = {
            .on_back = settings_menu_show,
        };
        firmware_screen_create(&fw_cfg);
        state.sub_screen_active = true;
        state.active_sub_screen = SETTINGS_FIRMWARE;
    } else if (strcmp(tag, "settings_time") == 0) {
        ESP_LOGI(TAG, "Opening Time settings...");
        time_screen_config_t time_cfg = {
            .on_back = settings_menu_show,
        };
        time_screen_create(&time_cfg);
        state.sub_screen_active = true;
        state.active_sub_screen = SETTINGS_TIME;
    } else if (strcmp(tag, "settings_language") == 0) {
        ESP_LOGI(TAG, "Opening Language settings...");
        language_screen_config_t lang_cfg = {
            .on_back = settings_menu_show,
        };
        language_screen_create(&lang_cfg);
        state.sub_screen_active = true;
        state.active_sub_screen = SETTINGS_LANGUAGE;
    } else if (strcmp(tag, "settings_display") == 0) {
        ESP_LOGI(TAG, "Opening Display settings...");
        display_screen_config_t disp_cfg = {
            .on_back = settings_menu_show,
        };
        display_screen_create(&disp_cfg);
        state.sub_screen_active = true;
        state.active_sub_screen = SETTINGS_DISPLAY;
    } else if (strcmp(tag, "settings_home_assistant") == 0) {
        ESP_LOGI(TAG, "Opening Home Assistant settings...");
        home_assistant_screen_config_t ha_cfg = {
            .on_back = settings_menu_show,
        };
        home_assistant_screen_create(&ha_cfg);
        state.sub_screen_active = true;
        state.active_sub_screen = SETTINGS_HOME_ASSISTANT;
    } else if (strcmp(tag, "settings_security") == 0) {
        ESP_LOGI(TAG, "Opening Security settings...");
        security_screen_config_t sec_cfg = {
            .on_back = settings_menu_show,
        };
        security_screen_create(&sec_cfg);
        state.sub_screen_active = true;
        state.active_sub_screen = SETTINGS_SECURITY;
    } else if (strcmp(tag, "settings_web") == 0) {
        ESP_LOGI(TAG, "Opening Web Interface settings...");
        web_screen_config_t web_cfg = {
            .on_back = settings_menu_show,
        };
        web_screen_create(&web_cfg);
        state.sub_screen_active = true;
        state.active_sub_screen = SETTINGS_WEB;
    } else if (strcmp(tag, "settings_factory_reset") == 0) {
        show_factory_reset_confirmation();
    }
}

// ============================================================================
// Factory Reset Confirmation
// ============================================================================

static void dismiss_factory_reset_overlay(void)
{
    if (state.factory_reset_overlay) {
        lv_obj_delete(state.factory_reset_overlay);
        state.factory_reset_overlay = NULL;
        state.factory_reset_confirm_btn = NULL;
        state.factory_reset_confirm_label = NULL;
    }
}

static void factory_reset_confirm_cb(lv_event_t* e)
{
    (void)e;
    if (!state.factory_reset_confirm_btn) return;

    lv_obj_add_state(state.factory_reset_confirm_btn, LV_STATE_DISABLED);
    if (state.factory_reset_confirm_label) {
        lv_label_set_text(state.factory_reset_confirm_label,
                          i18n_get(STR_FACTORY_RESET_ERASING));
    }

    if (!factory_reset_start()) {
        lv_obj_clear_state(state.factory_reset_confirm_btn, LV_STATE_DISABLED);
        if (state.factory_reset_confirm_label) {
            lv_label_set_text(state.factory_reset_confirm_label,
                              i18n_get(STR_FACTORY_RESET_START_FAILED));
        }
    }
}

static void factory_reset_cancel_cb(lv_event_t* e)
{
    (void)e;
    dismiss_factory_reset_overlay();
}

static void show_factory_reset_confirmation(void)
{
    if (!state.screen) return;
    dismiss_factory_reset_overlay();

    lv_obj_t* overlay = lv_obj_create(state.screen);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_70, LV_PART_MAIN);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(overlay, (void*)"factory_reset_overlay");
    state.factory_reset_overlay = overlay;

    lv_obj_t* panel = lv_obj_create(overlay);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(panel, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 32, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(panel, 48, LV_PART_MAIN);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(panel, 14, LV_PART_MAIN);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(panel, (void*)"factory_reset_panel");

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, i18n_get(STR_FACTORY_RESET_TITLE));
    lv_obj_set_style_text_font(title, FONT_LARGE, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xff5a5f), LV_PART_MAIN);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t* description = lv_label_create(panel);
    lv_obj_set_width(description, LV_PCT(100));
    lv_label_set_long_mode(description, LV_LABEL_LONG_WRAP);
    lv_label_set_text(description, i18n_get(STR_FACTORY_RESET_DESCRIPTION));
    lv_obj_set_style_text_font(description, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(description, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_align(description, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t* warning = lv_label_create(panel);
    lv_label_set_text(warning, i18n_get(STR_FACTORY_RESET_WARNING));
    lv_obj_set_style_text_font(warning, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(warning, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_align(warning, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t* btn_row = lv_obj_create(panel);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_top(btn_row, 12, LV_PART_MAIN);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* cancel_btn = lv_button_create(btn_row);
    lv_obj_set_size(cancel_btn, 200, 64);
    lv_obj_set_style_bg_color(cancel_btn, COLOR_ROW, LV_PART_MAIN);
    lv_obj_set_style_radius(cancel_btn, 12, LV_PART_MAIN);
    lv_obj_set_user_data(cancel_btn, (void*)"factory_reset_cancel");
    lv_obj_add_event_cb(cancel_btn, factory_reset_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, i18n_get(STR_CANCEL));
    lv_obj_set_style_text_font(cancel_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(cancel_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_center(cancel_label);

    lv_obj_t* confirm_btn = lv_button_create(btn_row);
    lv_obj_set_size(confirm_btn, 260, 64);
    lv_obj_set_style_bg_color(confirm_btn, lv_color_hex(0xd32f2f), LV_PART_MAIN);
    lv_obj_set_style_radius(confirm_btn, 12, LV_PART_MAIN);
    lv_obj_set_user_data(confirm_btn, (void*)"factory_reset_confirm");
    lv_obj_add_event_cb(confirm_btn, factory_reset_confirm_cb, LV_EVENT_CLICKED, NULL);
    state.factory_reset_confirm_btn = confirm_btn;

    lv_obj_t* confirm_label = lv_label_create(confirm_btn);
    lv_label_set_text(confirm_label, i18n_get(STR_FACTORY_RESET_CONFIRM));
    lv_obj_set_style_text_font(confirm_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(confirm_label, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_center(confirm_label);
    state.factory_reset_confirm_label = confirm_label;

    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, 300);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, panel);
    lv_anim_set_values(&anim, 300, 0);
    lv_anim_set_duration(&anim, 300);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, [](void* obj, int32_t v) {
        lv_obj_align((lv_obj_t*)obj, LV_ALIGN_BOTTOM_MID, 0, v);
    });
    lv_anim_start(&anim);
}

// ============================================================================
// UI Creation
// ============================================================================

static void create_header(void)
{
    lv_display_t* disp = lv_display_get_default();
    int32_t header_height = lv_display_get_vertical_resolution(disp) * 8 / 100;
    
    state.header = lv_obj_create(state.screen);
    lv_obj_set_size(state.header, LV_PCT(100), header_height);
    lv_obj_align(state.header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(state.header, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state.header, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.header, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(state.header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.header, 10, LV_PART_MAIN);
    disable_scrolling(state.header);
    
    // Title
    lv_obj_t* title = lv_label_create(state.header);
    lv_label_set_text(title, i18n_get(STR_SETTINGS));
    lv_obj_set_style_text_font(title, UI_FONT_HEADER, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 10, 0);
    
    // Close button
    lv_obj_t* close_btn = ui_create_close_button(state.header, close_btn_event_cb);
    lv_obj_set_user_data(close_btn, (void*)"settings_close");
}

static void create_menu_list(void)
{
    lv_display_t* disp = lv_display_get_default();
    int32_t screen_height = lv_display_get_vertical_resolution(disp);
    int32_t header_height = screen_height * 8 / 100;
    
    state.list_container = lv_obj_create(state.screen);
    lv_obj_set_size(state.list_container, LV_PCT(100), screen_height - header_height);
    lv_obj_set_pos(state.list_container, 0, header_height);
    lv_obj_set_style_bg_opa(state.list_container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.list_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state.list_container, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_row(state.list_container, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(state.list_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(state.list_container, LV_SCROLLBAR_MODE_AUTO);
    
    // Create setting rows
    state.rows[SETTINGS_WIFI] = create_settings_row(
        state.list_container, LV_SYMBOL_WIFI, 
        i18n_get(STR_SETTINGS_WIFI), "settings_wifi");
    
    // Add WiFi status subtitle
    lv_obj_t* wifi_row = state.rows[SETTINGS_WIFI];
    state.wifi_status_label = lv_label_create(wifi_row);
    lv_label_set_text(state.wifi_status_label, i18n_get(STR_WIFI_DISCONNECTED));
    lv_obj_set_style_text_font(state.wifi_status_label, FONT_NORMAL, LV_PART_MAIN);
    lv_obj_set_style_text_color(state.wifi_status_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_align(state.wifi_status_label, LV_ALIGN_RIGHT_MID, -30, 0);
    
    state.rows[SETTINGS_FIRMWARE] = create_settings_row(
        state.list_container, LV_SYMBOL_DOWNLOAD, 
        i18n_get(STR_SETTINGS_UPDATE), "settings_firmware");
    
    state.rows[SETTINGS_TIME] = create_settings_row(
        state.list_container, LV_SYMBOL_BELL, 
        i18n_get(STR_SETTINGS_TIME), "settings_time");
    
    state.rows[SETTINGS_LANGUAGE] = create_settings_row(
        state.list_container, LV_SYMBOL_SETTINGS, 
        i18n_get(STR_SETTINGS_LANGUAGE), "settings_language");
    
    state.rows[SETTINGS_DISPLAY] = create_settings_row(
        state.list_container, LV_SYMBOL_IMAGE, 
        i18n_get(STR_SETTINGS_DISPLAY), "settings_display");

    state.rows[SETTINGS_HOME_ASSISTANT] = create_settings_row(
        state.list_container, LV_SYMBOL_HOME,
        i18n_get(STR_SETTINGS_HOME_ASSISTANT),
        "settings_home_assistant");

    state.rows[SETTINGS_SECURITY] = create_settings_row(
        state.list_container, LV_SYMBOL_KEYBOARD,
        i18n_get(STR_SETTINGS_SECURITY),
        "settings_security");

    state.rows[SETTINGS_WEB] = create_settings_row(
        state.list_container, LV_SYMBOL_IMAGE,
        i18n_get(STR_SETTINGS_WEB),
        "settings_web");
    
    // Demo Mode toggle
#if CONFIG_DEMO_MODE
    lv_obj_t* demo_row = create_toggle_row(state.list_container, LV_SYMBOL_PLAY,
                      i18n_get(STR_SETTINGS_DEMO_MODE), app_prefs_is_demo_mode(),
                      &state.demo_mode_switch, demo_mode_switch_cb);
    lv_obj_set_user_data(demo_row, (void*)"settings_demo_mode");
    lv_obj_set_user_data(state.demo_mode_switch, (void*)"demo_mode_switch");
#endif
    
    // Temperature Units toggle with °C / °F labels
    {
        bool is_fahrenheit = app_prefs_get_temp_unit() == TEMP_UNIT_FAHRENHEIT;
        
        lv_obj_t* row = lv_obj_create(state.list_container);
        lv_obj_set_size(row, LV_PCT(100), 88);
        lv_obj_set_style_bg_color(row, COLOR_ROW, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(row, 15, LV_PART_MAIN);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        
        // Icon on left
        lv_obj_t* icon_label = lv_label_create(row);
        lv_label_set_text(icon_label, LV_SYMBOL_SETTINGS);
        lv_obj_set_style_text_font(icon_label, FONT_LARGE, LV_PART_MAIN);
        lv_obj_set_style_text_color(icon_label, COLOR_ACCENT, LV_PART_MAIN);
        lv_obj_align(icon_label, LV_ALIGN_LEFT_MID, 0, 0);
        
        // "Temperature" label
        lv_obj_t* text_label = lv_label_create(row);
        lv_label_set_text(text_label, i18n_get(STR_SETTINGS_TEMPERATURE));
        lv_obj_set_style_text_font(text_label, FONT_NORMAL, LV_PART_MAIN);
        lv_obj_set_style_text_color(text_label, COLOR_TEXT, LV_PART_MAIN);
        lv_obj_align(text_label, LV_ALIGN_LEFT_MID, 45, 0);
        
        // °C label (right side, before switch)
        lv_obj_t* celsius_lbl = lv_label_create(row);
        lv_label_set_text(celsius_lbl, "°C");
        lv_obj_set_style_text_font(celsius_lbl, FONT_LARGE, LV_PART_MAIN);
        lv_obj_set_style_text_color(celsius_lbl, is_fahrenheit ? COLOR_TEXT_DIM : COLOR_TEXT, LV_PART_MAIN);
        lv_obj_align(celsius_lbl, LV_ALIGN_RIGHT_MID, -145, 0);
        state.celsius_label = celsius_lbl;
        
        // Switch in middle-right
        lv_obj_t* sw = lv_switch_create(row);
        lv_obj_set_size(sw, 80, 40);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -55, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(0x555555), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sw, COLOR_ACCENT, 
            static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | static_cast<lv_style_selector_t>(LV_STATE_CHECKED));
        if (is_fahrenheit) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
        
        // °F label (far right)
        lv_obj_t* fahrenheit_lbl = lv_label_create(row);
        lv_label_set_text(fahrenheit_lbl, "°F");
        lv_obj_set_style_text_font(fahrenheit_lbl, FONT_LARGE, LV_PART_MAIN);
        lv_obj_set_style_text_color(fahrenheit_lbl, is_fahrenheit ? COLOR_TEXT : COLOR_TEXT_DIM, LV_PART_MAIN);
        lv_obj_align(fahrenheit_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
        state.fahrenheit_label = fahrenheit_lbl;
        
        // Tag the row for test automation
        lv_obj_set_user_data(row, (void*)"settings_temp_unit");
        lv_obj_add_event_cb(sw, temp_unit_switch_cb, LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_set_user_data(sw, (void*)"temp_unit_switch");
        
        state.temp_unit_switch = sw;
    }

    lv_obj_t* reset_row = create_settings_row(
        state.list_container, LV_SYMBOL_WARNING,
        i18n_get(STR_SETTINGS_FACTORY_RESET), "settings_factory_reset");
    lv_obj_set_style_border_width(reset_row, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(reset_row, lv_color_hex(0x7f1d1d), LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_obj_get_child(reset_row, 0),
                                lv_color_hex(0xff5a5f), LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_obj_get_child(reset_row, 1),
                                lv_color_hex(0xff5a5f), LV_PART_MAIN);
}

// ============================================================================
// Public API
// ============================================================================

void settings_menu_create(const settings_menu_config_t* config)
{
    if (state.visible) {
        return;
    }
    
    ESP_LOGI(TAG, "Creating settings menu");
    
    if (config) {
        state.config = *config;
    } else {
        memset(&state.config, 0, sizeof(state.config));
    }
    
    state.visible = true;
    state.sub_screen_active = false;
    
    state.screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(state.screen, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state.screen, LV_OPA_COVER, LV_PART_MAIN);
    
    create_header();
    create_menu_list();
    
    // Update WiFi status if connected
    if (wifi_mgr_get_state() == WIFI_MGR_STATE_CONNECTED) {
        settings_menu_update_wifi_status(true, wifi_mgr_get_connected_ssid());
    }
    
    lv_screen_load(state.screen);
}

void settings_menu_close(void)
{
    if (!state.visible) {
        return;
    }
    
    ESP_LOGI(TAG, "Closing settings menu");
    
    state.visible = false;
    // Screen deletion is handled by LVGL auto_del when returning to main screen
    state.screen = NULL;
}

void settings_menu_force_close(lv_obj_t* return_screen)
{
    if (!state.visible) return;

    lv_obj_t* active_screen = lv_screen_active();
    if (state.sub_screen_active) {
        switch (state.active_sub_screen) {
            case SETTINGS_WIFI: wifi_screen_close(); break;
            case SETTINGS_FIRMWARE: firmware_screen_close(); break;
            case SETTINGS_TIME: time_screen_close(); break;
            case SETTINGS_LANGUAGE: language_screen_close(); break;
            case SETTINGS_DISPLAY: display_screen_close(); break;
            case SETTINGS_HOME_ASSISTANT:
                home_assistant_screen_close();
                break;
            case SETTINGS_SECURITY:
                security_screen_close();
                break;
            case SETTINGS_WEB:
                web_screen_close();
                break;
            default: break;
        }
    }

    // A settings sub-screen leaves the settings root allocated but hidden.
    if (state.screen && state.screen != active_screen) {
        lv_obj_delete(state.screen);
    }

    state.visible = false;
    state.sub_screen_active = false;
    state.screen = NULL;
    state.reboot_overlay = NULL;
    state.factory_reset_overlay = NULL;
    state.factory_reset_confirm_btn = NULL;
    state.factory_reset_confirm_label = NULL;

    if (return_screen && active_screen != return_screen) {
        lv_screen_load_anim(return_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
    }
}

bool settings_menu_is_visible(void)
{
    return state.visible;
}

void settings_menu_show(void)
{
    if (!state.visible || !state.screen) {
        return;
    }
    
    state.sub_screen_active = false;
    
    // Rebuild screen contents to pick up language/setting changes
    lv_obj_clean(state.screen);
    create_header();
    create_menu_list();
    
    // Update WiFi status if connected
    if (wifi_mgr_get_state() == WIFI_MGR_STATE_CONNECTED) {
        settings_menu_update_wifi_status(true, wifi_mgr_get_connected_ssid());
    }
    
    // Slide right animation with auto_del=true - LVGL will delete the old screen after animation
    lv_screen_load_anim(state.screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, true);
}

void settings_menu_refresh(void)
{
    if (!state.visible) {
        return;
    }
    
    // Store config and recreate
    settings_menu_config_t saved_config = state.config;
    settings_menu_close();
    settings_menu_create(&saved_config);
}

void settings_menu_update_wifi_status(bool connected, const char* ssid)
{
    // Forward to WiFi screen if it's active
    if (state.sub_screen_active && state.active_sub_screen == SETTINGS_WIFI) {
        char ip_buf[16] = {0};
        wifi_mgr_get_ip_addr(ip_buf, sizeof(ip_buf));
        wifi_screen_update_connection(connected, ssid, ip_buf);
        return;
    }
    
    if (!state.visible || !state.wifi_status_label) {
        return;
    }
    
    if (connected && ssid && ssid[0]) {
        lv_label_set_text(state.wifi_status_label, ssid);
        lv_obj_set_style_text_color(state.wifi_status_label, COLOR_ACCENT, LV_PART_MAIN);
    } else {
        lv_label_set_text(state.wifi_status_label, i18n_get(STR_WIFI_DISCONNECTED));
        lv_obj_set_style_text_color(state.wifi_status_label, COLOR_TEXT_DIM, LV_PART_MAIN);
    }
}
