/*
 * Arctic Heat Pump Controller
 * Common UI definitions and utilities
 */
#pragma once

#include <lvgl.h>

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
// Common Fonts
// ============================================================================
#define UI_FONT_TITLE         (&lv_font_montserrat_32)
#define UI_FONT_SUBTITLE      (&lv_font_montserrat_24)
#define UI_FONT_BODY          (&lv_font_montserrat_20)
#define UI_FONT_SMALL         (&lv_font_montserrat_16)
#define UI_FONT_ICON          (&lv_font_montserrat_32)  // Match status bar icons
#define UI_FONT_ICON_LARGE    (&lv_font_montserrat_32)

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
    lv_obj_set_style_bg_color(btn, UI_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, UI_COLOR_BTN_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, UI_CLOSE_BTN_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    
    if (event_cb) {
        lv_obj_add_event_cb(btn, event_cb, LV_EVENT_CLICKED, NULL);
    }
    
    lv_obj_t* icon = lv_label_create(btn);
    lv_label_set_text(icon, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(icon, UI_FONT_ICON, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, UI_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_center(icon);
    
    return btn;
}

#ifdef __cplusplus
}
#endif
