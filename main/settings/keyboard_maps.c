/**
 * Custom keyboard maps for WiFi password dialog
 * Removes OK and keyboard-toggle buttons (header has those)
 * 
 * This is in a .c file because LVGL uses implicit int-to-enum
 * conversions that don't work in C++
 */

#include "lvgl.h"

// Helper macro for button width with popover
#define KB_BTN(w) (LV_BUTTONMATRIX_CTRL_POPOVER | (w))

// Lowercase keyboard - removed LV_SYMBOL_KEYBOARD and LV_SYMBOL_OK from bottom row
const char* const kb_map_lc[] = {
    "1#", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
    "ABC", "a", "s", "d", "f", "g", "h", "j", "k", "l", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "z", "x", "c", "v", "b", "n", "m", ".", ",", ":", "\n",
    LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, ""
};

const lv_buttonmatrix_ctrl_t kb_ctrl_lc[] = {
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | 5, KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), LV_BUTTONMATRIX_CTRL_CHECKED | 7,
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | 6, KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3), LV_BUTTONMATRIX_CTRL_CHECKED | 7,
    LV_BUTTONMATRIX_CTRL_CHECKED | KB_BTN(1), LV_BUTTONMATRIX_CTRL_CHECKED | KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), LV_BUTTONMATRIX_CTRL_CHECKED | KB_BTN(1), LV_BUTTONMATRIX_CTRL_CHECKED | KB_BTN(1), LV_BUTTONMATRIX_CTRL_CHECKED | KB_BTN(1),
    LV_BUTTONMATRIX_CTRL_CHECKED | 2, 6, LV_BUTTONMATRIX_CTRL_CHECKED | 2
};

// Uppercase keyboard
const char* const kb_map_uc[] = {
    "1#", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "A", "S", "D", "F", "G", "H", "J", "K", "L", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "Z", "X", "C", "V", "B", "N", "M", ".", ",", ":", "\n",
    LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, ""
};

const lv_buttonmatrix_ctrl_t kb_ctrl_uc[] = {
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | 5, KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), KB_BTN(4), LV_BUTTONMATRIX_CTRL_CHECKED | 7,
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | 6, KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3), KB_BTN(3), LV_BUTTONMATRIX_CTRL_CHECKED | 7,
    LV_BUTTONMATRIX_CTRL_CHECKED | KB_BTN(1), LV_BUTTONMATRIX_CTRL_CHECKED | KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), LV_BUTTONMATRIX_CTRL_CHECKED | KB_BTN(1), LV_BUTTONMATRIX_CTRL_CHECKED | KB_BTN(1), LV_BUTTONMATRIX_CTRL_CHECKED | KB_BTN(1),
    LV_BUTTONMATRIX_CTRL_CHECKED | 2, 6, LV_BUTTONMATRIX_CTRL_CHECKED | 2
};

// Special characters keyboard
const char* const kb_map_spec[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "+", "&", "/", "*", "=", "%", "!", "?", "#", "<", ">", "\n",
    "\\", "@", "$", "(", ")", "{", "}", "[", "]", ";", "\"", "'", "\n",
    LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, ""
};

const lv_buttonmatrix_ctrl_t kb_ctrl_spec[] = {
    KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), LV_BUTTONMATRIX_CTRL_CHECKED | 2,
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2, KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1),
    KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1), KB_BTN(1),
    LV_BUTTONMATRIX_CTRL_CHECKED | 2, 6, LV_BUTTONMATRIX_CTRL_CHECKED | 2
};
