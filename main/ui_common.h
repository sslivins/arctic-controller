/*
 * Arctic Heat Pump Controller
 * Common UI definitions and utilities
 */
#pragma once

#include <lvgl.h>
#include "fonts/fonts.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Common Colors
// ============================================================================
#define UI_COLOR_BG           lv_color_hex(0x1a1a2e)
#define UI_COLOR_PANEL        lv_color_hex(0x16213e)
#define UI_COLOR_TEXT         lv_color_hex(0xeaeaea)
#define UI_COLOR_TEXT_DIM     lv_color_hex(0x888888)
#define UI_COLOR_ACCENT       lv_color_hex(0x00d4ff)
#define UI_COLOR_SUCCESS      lv_color_hex(0x4ade80)
#define UI_COLOR_WARNING      lv_color_hex(0xfbbf24)
#define UI_COLOR_ERROR        lv_color_hex(0xef4444)
#define UI_COLOR_BTN_PRESSED  lv_color_hex(0x2a3a5e)

// ============================================================================
// Common Sizes
// ============================================================================
#define UI_CLOSE_BTN_SIZE     60
#define UI_CLOSE_BTN_RADIUS   30
#define UI_HEADER_HEIGHT      70

// ============================================================================
// Common Fonts (using custom Latin-extended fonts for i18n support)
// ============================================================================
#define UI_FONT_HEADER        (&montserrat_32_latin)  // Screen titles/headers
#define UI_FONT_TITLE         (&montserrat_40_latin)
#define UI_FONT_SUBTITLE      (&montserrat_24_latin)
#define UI_FONT_BODY          (&montserrat_32_latin)
#define UI_FONT_SMALL         (&montserrat_24_latin)
#define UI_FONT_ICON          (&montserrat_32_latin)  // Match status bar icons
#define UI_FONT_ICON_LARGE    (&montserrat_32_latin)

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Create a standard close button (X icon)
 * @param parent Parent object (usually header)
 * @param event_cb Event callback for when button is clicked
 * @return The created button object
 */
static inline lv_obj_t* ui_create_close_button(lv_obj_t* parent, lv_event_cb_t event_cb)
{
    lv_obj_t* btn = lv_btn_create(parent);
    lv_obj_set_size(btn, UI_CLOSE_BTN_SIZE, UI_CLOSE_BTN_SIZE);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x3d4f6f), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, UI_COLOR_BTN_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, UI_CLOSE_BTN_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, UI_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_border_opa(btn, LV_OPA_50, LV_PART_MAIN);
    
    if (event_cb) {
        lv_obj_add_event_cb(btn, event_cb, LV_EVENT_CLICKED, NULL);
    }
    
    lv_obj_t* icon = lv_label_create(btn);
    lv_label_set_text(icon, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(icon, UI_FONT_ICON, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, UI_COLOR_ACCENT, LV_PART_MAIN);
    lv_obj_center(icon);
    
    return btn;
}

#ifdef __cplusplus
}
#endif
