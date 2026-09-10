/*
 * Host implementation of the FreeRTOS event-group stub. See
 * freertos/event_groups.h.
 */

#include "freertos/event_groups.h"

#include <cstddef>
#include <vector>

struct EventGroupDef_t {
    EventBits_t bits = 0;
};

namespace {
std::vector<EventGroupDef_t *> g_groups;
bool g_fail_next_create = false;
}  // namespace

extern "C" {

EventGroupHandle_t xEventGroupCreate(void) {
    if (g_fail_next_create) {
        g_fail_next_create = false;
        return nullptr;
    }
    EventGroupDef_t *g = new EventGroupDef_t();
    g_groups.push_back(g);
    return g;
}

void vEventGroupDelete(EventGroupHandle_t group) {
    if (!group) return;
    for (size_t i = 0; i < g_groups.size(); ++i) {
        if (g_groups[i] == group) {
            g_groups.erase(g_groups.begin() + (long)i);
            break;
        }
    }
    delete group;
}

EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits) {
    if (!group) return 0;
    group->bits |= bits;
    return group->bits;
}

EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits) {
    if (!group) return 0;
    EventBits_t before = group->bits;
    group->bits &= ~bits;
    return before;
}

EventBits_t xEventGroupGetBits(EventGroupHandle_t group) {
    return group ? group->bits : 0;
}

}  // extern "C"

namespace event_group_fake {

void fail_next_create() { g_fail_next_create = true; }

void reset() {
    for (EventGroupDef_t *g : g_groups) {
        delete g;
    }
    g_groups.clear();
    g_fail_next_create = false;
}

int created() { return (int)g_groups.size(); }

}  // namespace event_group_fake
