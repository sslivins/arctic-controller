/*
 * Host stub for freertos/task.h.
 *
 * Deliberately NOT a scheduler. xTaskCreate records the request and reports
 * success without running anything, because the only task in the host-tested
 * sources (log_persist_task) is an infinite loop with a 5-second delay -- a
 * real host scheduler would either hang the suite or turn every assertion into
 * a race.
 *
 * The consequence is stated plainly so nobody mistakes it for coverage: the
 * background task's own decision logic (severity debounce, heartbeat cadence)
 * is NOT exercised by the host tests. What is exercised is everything it
 * calls -- log_persist_flush_now takes the same lock and runs the same
 * flush_locked -- plus the fact that starting it is guarded correctly.
 *
 * task_fake::created() lets a test assert that a task was requested at all,
 * which is how "start is a no-op without a partition" is checked.
 */
#pragma once

#include "FreeRTOS.h"

#include <stdint.h>

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

#ifdef __cplusplus
#include <string>
#include <vector>

namespace task_fake {

struct Request {
    std::string name;
    uint32_t stack_words;
    unsigned priority;
};

// All xTaskCreate calls since reset(), in order.
std::vector<Request> &requests();

inline int created() { return (int)requests().size(); }

void reset();

// Make the next `count` xTaskCreate calls report failure, as they do on a
// device that has run out of internal RAM.
void fail_next(int count);

}  // namespace task_fake
#endif

#ifdef __cplusplus
extern "C" {
#endif

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack_words,
                       void *arg, unsigned priority, TaskHandle_t *out);

void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t task);

#ifdef __cplusplus
}
#endif
