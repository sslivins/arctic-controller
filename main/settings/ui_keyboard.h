/**
 * Shared on-screen keyboard. Every screen that needs typing should create its
 * keyboard here so the layout, key pop-ups and font stay the same everywhere.
 */
#pragma once

#include "lvgl.h"

enum class UiKeyboardKind {
    Text,    // letters / symbols (WiFi password, names, search, host)
    Number,  // digits only, with an OK key that fires LV_EVENT_READY
};

/**
 * Create a keyboard bound to `textarea`, full width at the standard height.
 * `bg` should match the dialog's card colour. Callers only position it
 * (lv_obj_align) and hook LV_EVENT_READY if they need it.
 */
lv_obj_t* ui_keyboard_create(lv_obj_t* parent, lv_obj_t* textarea,
                             UiKeyboardKind kind, lv_color_t bg);
