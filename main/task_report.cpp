/*
 * Arctic Heat Pump Controller
 * Task snapshot for stall detectors (see task_report.h).
 */
#include "task_report.h"

#include <inttypes.h>
#include <stdio.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/freertos_debug.h>
#include <sdkconfig.h>

#if CONFIG_IDF_TARGET_ARCH_RISCV
#include <riscv/rvruntime-frames.h>
#endif

extern "C" {
extern int _text_start;
extern int _text_end;
extern int _iram_text_start;
extern int _iram_text_end;
}

namespace {

constexpr TickType_t kSampleWindow = pdMS_TO_TICKS(1000);
constexpr int kTopTasks = 8;
constexpr int kMaxLocks = 12;
constexpr int kMaxStackAddrs = 24;

struct NamedLock {
    const char* name;
    SemaphoreHandle_t lock;
};
NamedLock s_locks[kMaxLocks];
int s_lock_count = 0;
portMUX_TYPE s_locks_mux = portMUX_INITIALIZER_UNLOCKED;

struct Snapshot {
    TaskStatus_t* tasks = nullptr;
    UBaseType_t count = 0;
    int64_t at_us = 0;
};

bool take(Snapshot& s)
{
    const UBaseType_t capacity = uxTaskGetNumberOfTasks() + 4;
    s.tasks = (TaskStatus_t*)heap_caps_calloc(capacity, sizeof(TaskStatus_t),
                                              MALLOC_CAP_SPIRAM);
    if (s.tasks == nullptr) {
        return false;
    }
    s.count = uxTaskGetSystemState(s.tasks, capacity, nullptr);
    s.at_us = esp_timer_get_time();
    return true;
}

char state_char(eTaskState state)
{
    switch (state) {
    case eRunning:   return 'X';
    case eReady:     return 'R';
    case eBlocked:   return 'B';
    case eSuspended: return 'S';
    case eDeleted:   return 'D';
    default:         return '?';
    }
}

void log_task(const char* tag, const TaskStatus_t& t, uint64_t delta, int64_t window_us,
              const char* mark)
{
    char core[12] = "any";
    if (t.xCoreID != tskNO_AFFINITY) snprintf(core, sizeof(core), "%d", (int)t.xCoreID);
    // A mutex holder runs at the priority of the highest task waiting for it.
    char boosted[24] = "";
    if (t.uxCurrentPriority != t.uxBasePriority) {
        snprintf(boosted, sizeof(boosted), " (base %u)", (unsigned)t.uxBasePriority);
    }
    ESP_LOGW(tag, "  %-16s %c prio %2u%s core %-3s cpu %3u%%%s%s",
             t.pcTaskName ? t.pcTaskName : "?", state_char(t.eCurrentState),
             (unsigned)t.uxCurrentPriority, boosted, core,
             (unsigned)(window_us > 0 ? delta * 100 / (uint64_t)window_us : 0),
             mark ? "  " : "", mark ? mark : "");
}

// One line: "'LVGL' by httpd, 'event log' by arctic_demo_sync", or "".
void format_lock_holders(char* out, size_t size)
{
    NamedLock locks[kMaxLocks];
    int count;
    taskENTER_CRITICAL(&s_locks_mux);
    count = s_lock_count;
    for (int i = 0; i < count; i++) locks[i] = s_locks[i];
    taskEXIT_CRITICAL(&s_locks_mux);

    size_t len = 0;
    out[0] = '\0';
    for (int i = 0; i < count && len < size; i++) {
        TaskHandle_t holder = xSemaphoreGetMutexHolder(locks[i].lock);
        if (holder != nullptr) {
            len += snprintf(out + len, size - len, "%s'%s' by %s", len ? ", " : "",
                            locks[i].name, pcTaskGetName(holder));
        }
    }
}

bool is_code(uint32_t v)
{
    return (v >= (uint32_t)&_text_start && v < (uint32_t)&_text_end) ||
           (v >= (uint32_t)&_iram_text_start && v < (uint32_t)&_iram_text_end);
}

// A task that is not running has its registers saved on its own stack: the
// program counter, the return address, and above the stack pointer the return
// addresses of the calls it is inside (plus some stale ones). Returns how
// many of out[0..] were filled (pc, ra, then stack code addresses); 0 for a
// running task.
int read_stack(uint32_t* out, int max, TaskHandle_t task)
{
#if CONFIG_IDF_TARGET_ARCH_RISCV
    TaskStatus_t status;
    vTaskGetInfo(task, &status, pdFALSE, eInvalid);
    if (status.eCurrentState == eRunning) {
        return 0;
    }
    TaskSnapshot_t snap;
    if (vTaskGetSnapshot(task, &snap) != pdTRUE || snap.pxTopOfStack == nullptr ||
        snap.pxEndOfStack == nullptr) {
        return 0;
    }
    const auto* frame = (const RvExcFrame*)snap.pxTopOfStack;
    const uint32_t* end = (const uint32_t*)snap.pxEndOfStack;
    const uint32_t* p = (const uint32_t*)frame->sp;
    if (p < (const uint32_t*)(frame + 1) || p >= end) {
        p = (const uint32_t*)(frame + 1);
    }
    int n = 0;
    out[n++] = (uint32_t)frame->mepc;
    out[n++] = (uint32_t)frame->ra;
    for (; p < end && n < max; p++) {
        const uint32_t v = *p;
        if (is_code(v) && (v & 1) == 0) {
            out[n++] = v;
        }
    }
    return n;
#else
    (void)out;
    (void)max;
    (void)task;
    return 0;
#endif
}

// Short lines: /api/logs keeps 192 characters per entry.
void log_stack(const char* tag, const char* name, const uint32_t* addrs, int count)
{
    if (count < 2) {
        return;
    }
    ESP_LOGW(tag, "  %s pc 0x%08" PRIx32 " ra 0x%08" PRIx32 "; stack code addresses:", name,
             addrs[0], addrs[1]);
    constexpr int kPerLine = 12;
    for (int i = 2; i < count; i += kPerLine) {
        char line[kPerLine * 11 + 1];
        int len = 0;
        for (int j = i; j < count && j < i + kPerLine; j++) {
            len += snprintf(line + len, sizeof(line) - len, " 0x%08" PRIx32, addrs[j]);
        }
        ESP_LOGW(tag, "   %s", line);
    }
}

}  // namespace

void task_report_register_lock(const char* name, SemaphoreHandle_t lock)
{
    if (lock == nullptr) {
        return;
    }
    taskENTER_CRITICAL(&s_locks_mux);
    if (s_lock_count < kMaxLocks) {
        s_locks[s_lock_count++] = {name, lock};
    }
    taskEXIT_CRITICAL(&s_locks_mux);
}

void task_report(const char* tag, const char* what, int64_t stuck_us,
                 TaskHandle_t watched, const char* mark)
{
    Snapshot a;
    Snapshot b;
    // Locks and stack as of now, while it is stuck; CPU use over the next second.
    char holders[256];
    uint32_t stack[2 + kMaxStackAddrs];
    format_lock_holders(holders, sizeof(holders));
    const int stack_count =
        watched != nullptr ? read_stack(stack, (int)(sizeof(stack) / sizeof(stack[0])), watched) : 0;
    if (!take(a)) {
        ESP_LOGW(tag, "%s stuck for %" PRId64 " ms (no memory for a task snapshot)",
                 what, stuck_us / 1000);
        return;
    }
    vTaskDelay(kSampleWindow);
    if (!take(b)) {
        heap_caps_free(a.tasks);
        return;
    }
    const int64_t window_us = b.at_us - a.at_us;

    // Run-time delta per task in snapshot b (0 for tasks created meanwhile).
    uint64_t* delta = (uint64_t*)heap_caps_calloc(b.count, sizeof(uint64_t), MALLOC_CAP_SPIRAM);
    if (delta != nullptr) {
        for (UBaseType_t i = 0; i < b.count; i++) {
            for (UBaseType_t j = 0; j < a.count; j++) {
                if (a.tasks[j].xHandle == b.tasks[i].xHandle) {
                    delta[i] = (uint64_t)(b.tasks[i].ulRunTimeCounter - a.tasks[j].ulRunTimeCounter);
                    break;
                }
            }
        }
    }

    ESP_LOGW(tag, "%s stuck for %" PRId64 " ms; tasks over the next %" PRId64
                  " ms (X running, R ready, B blocked, S suspended):",
             what, (stuck_us + window_us) / 1000, window_us / 1000);
    bool watched_logged = false;
    if (delta != nullptr) {
        for (int n = 0; n < kTopTasks; n++) {
            int best = -1;
            for (UBaseType_t i = 0; i < b.count; i++) {
                if (delta[i] != UINT64_MAX && (best < 0 || delta[i] > delta[best])) {
                    best = (int)i;
                }
            }
            if (best < 0) {
                break;
            }
            const bool is_watched = b.tasks[best].xHandle == watched;
            watched_logged |= is_watched;
            log_task(tag, b.tasks[best], delta[best], window_us, is_watched ? mark : nullptr);
            delta[best] = UINT64_MAX;
        }
    }
    if (!watched_logged) {
        for (UBaseType_t i = 0; i < b.count; i++) {
            if (b.tasks[i].xHandle == watched) {
                log_task(tag, b.tasks[i],
                         delta != nullptr && delta[i] != UINT64_MAX ? delta[i] : 0,
                         window_us, mark);
            }
        }
    }
    if (holders[0] != '\0') {
        ESP_LOGW(tag, "  locks held: %s", holders);
    }
    log_stack(tag, pcTaskGetName(watched), stack, stack_count);
    ESP_LOGW(tag, "  internal heap free %u, largest %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    heap_caps_free(delta);
    heap_caps_free(a.tasks);
    heap_caps_free(b.tasks);
}
