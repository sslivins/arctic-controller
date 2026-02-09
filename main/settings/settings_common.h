/*
 * Arctic Heat Pump Controller
 * Settings Screen - Common Definitions
 * 
 * Shared colors, constants, and helpers used across all settings screens.
 */
#pragma once

#include <lvgl.h>
#include <esp_log.h>
#include "fonts/fonts.h"
#include "settings_types.h"

// ============================================================================
// Custom Fonts with Latin Extended Support
// ============================================================================

#define FONT_NORMAL   &montserrat_24_latin
#define FONT_LARGE    &montserrat_24_latin

// ============================================================================
// Colors and Styles
// ============================================================================

#define COLOR_BG            lv_color_hex(0x1a1a2e)
#define COLOR_CARD          lv_color_hex(0x1e2a4a)
#define COLOR_HEADER        lv_color_hex(0x16213e)
#define COLOR_SIDEBAR       lv_color_hex(0x0f1a2e)
#define COLOR_SIDEBAR_BTN   lv_color_hex(0x1a2a4e)
#define COLOR_SIDEBAR_SEL   lv_color_hex(0x00d4ff)
#define COLOR_ACCENT        lv_color_hex(0x00d4ff)
#define COLOR_TEXT          lv_color_hex(0xffffff)
#define COLOR_TEXT_DIM      lv_color_hex(0x888888)
#define COLOR_SUCCESS       lv_color_hex(0x4caf50)
#define COLOR_WARNING       lv_color_hex(0xff9800)
#define COLOR_ERROR         lv_color_hex(0xf44336)
#define COLOR_DISCONNECT    lv_color_hex(0xe74c3c)

// ============================================================================
// Layout Constants
// ============================================================================

#define SIDEBAR_WIDTH_PCT   23      // Sidebar takes ~23% of screen width
#define HEADER_HEIGHT_PCT   8       // Header takes ~8% of screen height
#define SCAN_INTERVAL_MS    10000

// ============================================================================
// Firmware Update State
// ============================================================================

typedef enum {
    UPDATE_STATE_IDLE,
    UPDATE_STATE_CHECKING,
    UPDATE_STATE_UPDATE_AVAILABLE,
    UPDATE_STATE_NO_UPDATE,
    UPDATE_STATE_DOWNLOADING,
    UPDATE_STATE_READY_TO_REBOOT,
    UPDATE_STATE_FAILED
} update_ui_state_t;

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Fully disable scrolling and hide scrollbars on an object
 */
static inline void disable_scrolling(lv_obj_t* obj) {
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

/**
 * @brief Sanitize SSID for display (handle special characters)
 */
void sanitize_ssid_for_display(char* dest, const char* src, size_t dest_size);

/**
 * @brief Get WiFi signal icon based on RSSI
 */
const char* get_signal_icon(int8_t rssi);

