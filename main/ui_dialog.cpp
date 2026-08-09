#include "ui_common.h"

namespace {

struct DialogContext {
    lv_obj_t* actions;
};

void dialog_delete_cb(lv_event_t* event)
{
    auto* context = static_cast<DialogContext*>(lv_event_get_user_data(event));
    lv_free(context);
}

}  // namespace

extern "C" lv_obj_t* ui_dialog_create(const char* title, const char* message)
{
    lv_obj_t* overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_70, LV_PART_MAIN);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* panel = lv_obj_create(overlay);
    lv_obj_set_width(panel, 640);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(panel, UI_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x0f3460), LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 32, LV_PART_MAIN);
    lv_obj_set_style_pad_row(panel, 20, LV_PART_MAIN);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(panel);

    lv_obj_t* title_label = lv_label_create(panel);
    lv_obj_set_width(title_label, LV_PCT(100));
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, UI_FONT_DIALOG_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_color(title_label, UI_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t* message_label = lv_label_create(panel);
    lv_obj_set_width(message_label, LV_PCT(100));
    lv_label_set_long_mode(message_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(message_label, message);
    lv_obj_set_style_text_font(message_label, UI_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(message_label, UI_COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_align(message_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t* actions = lv_obj_create(panel);
    lv_obj_remove_style_all(actions);
    lv_obj_set_size(actions, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(actions, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_top(actions, 8, LV_PART_MAIN);
    lv_obj_remove_flag(actions, LV_OBJ_FLAG_SCROLLABLE);

    auto* context = static_cast<DialogContext*>(lv_malloc(sizeof(DialogContext)));
    if (!context) {
        lv_obj_delete(overlay);
        return nullptr;
    }
    context->actions = actions;
    lv_obj_set_user_data(overlay, context);
    lv_obj_add_event_cb(overlay, dialog_delete_cb, LV_EVENT_DELETE, context);
    return overlay;
}

extern "C" lv_obj_t* ui_dialog_add_action(lv_obj_t* dialog,
                                            const char* label,
                                            const char* tag,
                                            ui_dialog_action_style_t style,
                                            lv_event_cb_t event_cb)
{
    if (!dialog) return nullptr;
    auto* context = static_cast<DialogContext*>(lv_obj_get_user_data(dialog));
    if (!context || !context->actions) return nullptr;

    lv_obj_t* button = lv_button_create(context->actions);
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
