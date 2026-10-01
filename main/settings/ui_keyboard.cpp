#include "ui_keyboard.h"
#include "keyboard_maps.h"
#include "settings_common.h"

lv_obj_t* ui_keyboard_create(lv_obj_t* parent, lv_obj_t* textarea,
                             UiKeyboardKind kind, lv_color_t bg)
{
    lv_obj_t* kb = lv_keyboard_create(parent);
    lv_obj_set_size(kb, LV_PCT(100), LV_PCT(kind == UiKeyboardKind::Number ? 30 : 25));
    lv_obj_set_style_bg_color(kb, bg, LV_PART_MAIN);
    lv_obj_set_style_text_font(kb, FONT_NORMAL, LV_PART_ITEMS);

    if (kind == UiKeyboardKind::Number) {
        lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_NUMBER, kb_map_num, kb_ctrl_num);
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    } else {
        lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, kb_map_lc, kb_ctrl_lc);
        lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, kb_map_uc, kb_ctrl_uc);
        lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_SPECIAL, kb_map_spec, kb_ctrl_spec);
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    }
    // Pop the pressed key up above the finger. The maps carry
    // LV_BUTTONMATRIX_CTRL_POPOVER on character keys; this keeps those flags.
    lv_keyboard_set_popovers(kb, true);

    lv_keyboard_set_textarea(kb, textarea);
    return kb;
}
