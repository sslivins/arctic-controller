#include "ui_overlay.h"

#include <stdlib.h>

namespace {

struct CoverLink {
    lv_obj_t* covered;
};

void covered_deleted_cb(lv_event_t* event)
{
    auto* link = static_cast<CoverLink*>(lv_event_get_user_data(event));
    link->covered = nullptr;
}

void cover_overlay_deleted_cb(lv_event_t* event)
{
    auto* link = static_cast<CoverLink*>(lv_event_get_user_data(event));
    if (link->covered) {
        lv_obj_remove_event_cb_with_user_data(link->covered, covered_deleted_cb, link);
        lv_obj_remove_flag(link->covered, LV_OBJ_FLAG_HIDDEN);
    }
    free(link);
}

void track_overlay_deleted_cb(lv_event_t* event)
{
    auto** handle = static_cast<lv_obj_t**>(lv_event_get_user_data(event));
    if (*handle == lv_event_get_target(event)) *handle = nullptr;
}

}  // namespace

extern "C" void ui_overlay_cover(lv_obj_t* overlay, lv_obj_t* covered)
{
    if (!overlay || !covered) return;
    auto* link = static_cast<CoverLink*>(malloc(sizeof(CoverLink)));
    if (!link) return;  // leave `covered` visible rather than risk stranding it
    link->covered = covered;
    lv_obj_add_event_cb(covered, covered_deleted_cb, LV_EVENT_DELETE, link);
    lv_obj_add_event_cb(overlay, cover_overlay_deleted_cb, LV_EVENT_DELETE, link);
    lv_obj_add_flag(covered, LV_OBJ_FLAG_HIDDEN);
}

extern "C" void ui_overlay_track(lv_obj_t* overlay, lv_obj_t** handle)
{
    if (!overlay || !handle) return;
    lv_obj_add_event_cb(overlay, track_overlay_deleted_cb, LV_EVENT_DELETE, handle);
}
