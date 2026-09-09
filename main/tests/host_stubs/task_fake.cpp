/*
 * Task and net_diag stubs for the host build. See freertos/task.h.
 *
 * net_diag walks lwIP's TCP PCB lists, which do not exist here. It is a pure
 * observer -- it logs a line and changes no state that any host-tested source
 * reads -- so a counter is a faithful stand-in.
 */

#include "freertos/task.h"

#include "net_diag.h"

#include <cstring>

namespace {
int g_fail_remaining = 0;
int g_netdiag_calls = 0;
int g_low_heap_watch_starts = 0;
}  // namespace

namespace task_fake {

std::vector<Request> &requests() {
    static std::vector<Request> v;
    return v;
}

void reset() {
    requests().clear();
    g_fail_remaining = 0;
}

void fail_next(int count) { g_fail_remaining = count; }

}  // namespace task_fake

extern "C" BaseType_t xTaskCreate(TaskFunction_t fn, const char *name,
                                  uint32_t stack_words, void *arg,
                                  unsigned priority, TaskHandle_t *out) {
    (void)fn;
    (void)arg;
    if (out != nullptr) {
        *out = nullptr;
    }
    if (g_fail_remaining > 0) {
        --g_fail_remaining;
        return pdFAIL;
    }
    task_fake::requests().push_back(
        {name != nullptr ? name : "", stack_words, priority});
    return pdPASS;
}

extern "C" void vTaskDelay(TickType_t ticks) { (void)ticks; }

extern "C" void vTaskDelete(TaskHandle_t task) { (void)task; }

// --- net_diag -------------------------------------------------------------

extern "C" void net_diag_sample(net_diag_t *out) {
    if (out == nullptr) return;
    std::memset(out, 0, sizeof(*out));
}

extern "C" void net_diag_log_snapshot(void) { ++g_netdiag_calls; }

// Spawns a FreeRTOS watch task on the device. There is no scheduler here, and
// the task is a pure observer, so counting the start is a faithful stand-in.
extern "C" void net_diag_start_low_heap_watch(void) { ++g_low_heap_watch_starts; }

namespace net_diag_fake {
int calls() { return g_netdiag_calls; }
int watch_starts() { return g_low_heap_watch_starts; }
}  // namespace net_diag_fake
