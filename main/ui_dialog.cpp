#include "ui_common.h"

namespace {

// The dialog is a fixed structure:
//
//   overlay (returned handle, tagged for the test API)
//     └── panel
//           ├── title label
//           ├── message label
//           ├── [extra body labels...]
//           └── actions row (always the last child)
//
// Nothing is stashed in lv_obj user_data: that slot belongs to the test API's
// widget tags, and a struct pointer parked there can be misread as a tag
// string by the tag heuristic in test_endpoints.cpp.

lv_obj_t* dialog_panel(lv_obj_t* dialog)
{
    if (!dialog || lv_obj_get_child_count(dialog) == 0) return nullptr;
    return lv_obj_get_child(dialog, 0);
}

lv_obj_t* dialog_actions(lv_obj_t* dialog)
{
    lv_obj_t* panel = dialog_panel(dialog);
    if (!panel) return nullptr;
    uint32_t count = lv_obj_get_child_count(panel);
    if (count == 0) return nullptr;
    return lv_obj_get_child(panel, count - 1);
}

lv_obj_t* create_body_label(lv_obj_t* panel, const char* text)
{
    lv_obj_t* label = lv_label_create(panel);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_style_text_font(label, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    return label;
}

void sheet_slide_exec_cb(void* obj, int32_t value)
{
    lv_obj_align(static_cast<lv_obj_t*>(obj), LV_ALIGN_BOTTOM_MID, 0, value);
}

}  // namespace

extern "C" lv_obj_t* ui_dialog_create(const char* title, const char* message)
{
    return ui_dialog_create_ex(
        title, message, UI_DIALOG_LAYOUT_CENTER, nullptr, nullptr);
}

extern "C" lv_obj_t* ui_dialog_create_ex(const char* title,
                                         const char* message,
                                         ui_dialog_layout_t layout,
                                         const char* overlay_tag,
                                         const char* panel_tag)
{
    lv_obj_t* overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_70, LV_PART_MAIN);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(overlay, const_cast<char*>(overlay_tag));

    lv_obj_t* panel = lv_obj_create(overlay);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(panel, UI_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 32, LV_PART_MAIN);
    lv_obj_set_style_pad_row(panel, 20, LV_PART_MAIN);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(panel, const_cast<char*>(panel_tag));

    if (layout == UI_DIALOG_LAYOUT_SHEET) {
        lv_obj_set_width(panel, LV_PCT(100));
        lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_bottom(panel, 48, LV_PART_MAIN);
        lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, 300);  // start off-screen
    } else {
        lv_obj_set_width(panel, 640);
        lv_obj_set_style_border_width(panel, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(panel, lv_color_hex(0x0f3460), LV_PART_MAIN);
        lv_obj_center(panel);
    }

    lv_obj_t* title_label = lv_label_create(panel);
    lv_obj_set_width(title_label, LV_PCT(100));
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(title_label, title ? title : "");
    lv_obj_set_style_text_font(title_label, UI_FONT_DIALOG_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(title_label, UI_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    create_body_label(panel, message);

    lv_obj_t* actions = lv_obj_create(panel);
    lv_obj_remove_style_all(actions);
    lv_obj_set_size(actions, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(actions, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_top(actions, 8, LV_PART_MAIN);
    lv_obj_remove_flag(actions, LV_OBJ_FLAG_SCROLLABLE);

    if (layout == UI_DIALOG_LAYOUT_SHEET) {
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, panel);
        lv_anim_set_values(&anim, 300, 0);
        lv_anim_set_duration(&anim, 300);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&anim, sheet_slide_exec_cb);
        lv_anim_start(&anim);
    }
    return overlay;
}

extern "C" lv_obj_t* ui_dialog_add_text(lv_obj_t* dialog, const char* text)
{
    lv_obj_t* panel = dialog_panel(dialog);
    lv_obj_t* actions = dialog_actions(dialog);
    if (!panel || !actions) return nullptr;

    lv_obj_t* label = create_body_label(panel, text);
    // Keep the action row last so buttons stay below every body line.
    lv_obj_move_to_index(actions, lv_obj_get_child_count(panel) - 1);
    return label;
}

extern "C" void ui_dialog_set_title_color(lv_obj_t* dialog, lv_color_t color)
{
    lv_obj_t* panel = dialog_panel(dialog);
    if (!panel || lv_obj_get_child_count(panel) == 0) return;
    lv_obj_set_style_text_color(lv_obj_get_child(panel, 0), color, LV_PART_MAIN);
}

extern "C" lv_obj_t* ui_dialog_add_action(lv_obj_t* dialog,
                                          const char* label,
                                          const char* tag,
                                          ui_dialog_action_style_t style,
                                          lv_event_cb_t event_cb)
{
    lv_obj_t* actions = dialog_actions(dialog);
    if (!actions) return nullptr;

    lv_obj_t* button = lv_button_create(actions);
    lv_obj_set_size(button, 220, 68);
    lv_obj_set_style_radius(button, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button,
        style == UI_DIALOG_ACTION_DANGER ? UI_COLOR_ERROR :
        style == UI_DIALOG_ACTION_PRIMARY ? UI_COLOR_ACCENT :
                                            lv_color_hex(0x3d4f6f),
        LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, UI_COLOR_BTN_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_user_data(button, const_cast<char*>(tag));
    if (event_cb) {
        lv_obj_add_event_cb(button, event_cb, LV_EVENT_CLICKED, dialog);
    }

    lv_obj_t* button_label = lv_label_create(button);
    lv_label_set_text(button_label, label);
    lv_obj_set_style_text_font(button_label, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(
        button_label,
        style == UI_DIALOG_ACTION_PRIMARY ? lv_color_hex(0x000000) : UI_COLOR_TEXT,
        LV_PART_MAIN);
    lv_obj_center(button_label);
    return button;
}

extern "C" void ui_dialog_dismiss_cb(lv_event_t* event)
{
    lv_obj_t* dialog = static_cast<lv_obj_t*>(lv_event_get_user_data(event));
    if (dialog) lv_obj_delete(dialog);
}
