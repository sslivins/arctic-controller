/*
 * Arctic Heat Pump Controller
 * Stall detector for the web UI/API server thread (see httpd_watch.h).
 */
#include "httpd_watch.h"

#include <atomic>
#include <inttypes.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

static const char* TAG = "httpd_watch";

namespace {

constexpr TickType_t kBeatPeriod = pdMS_TO_TICKS(1000);
// Normal requests take well under a second; a TLS handshake ~0.3 s.
constexpr int64_t kStallUs = 3 * 1000 * 1000;
// CPU use is measured over this window once a stall is seen.
constexpr TickType_t kSampleWindow = pdMS_TO_TICKS(1000);
constexpr int kTopTasks = 8;

SemaphoreHandle_t s_mutex = nullptr;
TaskHandle_t s_task = nullptr;
httpd_handle_t s_server = nullptr;
std::atomic<int> s_busy{0};

// Heartbeat bookkeeping. s_sent is written by the watch task only; s_acked
// by the server thread only.
uint32_t s_sent = 0;
std::atomic<uint32_t> s_acked{0};
std::atomic<int64_t> s_acked_us{0};
std::atomic<TaskHandle_t> s_server_task{nullptr};

void beat(void* arg)
{
    s_server_task.store(xTaskGetCurrentTaskHandle());
    s_acked_us.store(esp_timer_get_time());
    s_acked.store((uint32_t)(uintptr_t)arg);
}

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

void log_task(const TaskStatus_t& t, uint64_t delta, int64_t window_us, bool server)
{
    char core[12] = "any";
    if (t.xCoreID != tskNO_AFFINITY) snprintf(core, sizeof(core), "%d", (int)t.xCoreID);
    ESP_LOGW(TAG, "  %-16s %c prio %2u core %-3s cpu %3u%%%s",
             t.pcTaskName ? t.pcTaskName : "?", state_char(t.eCurrentState),
             (unsigned)t.uxCurrentPriority, core,
             (unsigned)(window_us > 0 ? delta * 100 / (uint64_t)window_us : 0),
             server ? "  <- web server" : "");
}

// Log every task's state and its share of one core over kSampleWindow, the
// busiest first, plus the web server thread itself.
void report(int64_t stuck_us)
{
    Snapshot a;
    Snapshot b;
    if (!take(a)) {
        ESP_LOGW(TAG, "Web server thread stuck for %" PRId64 " ms (no memory for a task snapshot)",
                 stuck_us / 1000);
        return;
    }
    vTaskDelay(kSampleWindow);
    if (!take(b)) {
        heap_caps_free(a.tasks);
        return;
    }
    const int64_t window_us = b.at_us - a.at_us;
    const TaskHandle_t server_task = s_server_task.load();

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

    ESP_LOGW(TAG, "Web server thread stuck for %" PRId64 " ms; tasks over the next %" PRId64
                  " ms (X running, R ready, B blocked, S suspended):",
             (stuck_us + window_us) / 1000, window_us / 1000);
    bool server_logged = false;
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
            const bool is_server = b.tasks[best].xHandle == server_task;
            server_logged |= is_server;
            log_task(b.tasks[best], delta[best], window_us, is_server);
            delta[best] = UINT64_MAX;
        }
    }
    if (!server_logged) {
        for (UBaseType_t i = 0; i < b.count; i++) {
            if (b.tasks[i].xHandle == server_task) {
                log_task(b.tasks[i], delta != nullptr && delta[i] != UINT64_MAX ? delta[i] : 0,
                         window_us, true);
            }
        }
    }
    ESP_LOGW(TAG, "  internal heap free %u, largest %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    heap_caps_free(delta);
    heap_caps_free(a.tasks);
    heap_caps_free(b.tasks);
}

void watch_task(void*)
{
    bool waiting = false;
    bool reported = false;
    int64_t sent_us = 0;
    httpd_handle_t watched = nullptr;
    for (;;) {
        vTaskDelay(kBeatPeriod);

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s_server != watched) {
            // New or no server: forget a heartbeat sent to the old one.
            watched = s_server;
            waiting = false;
            reported = false;
        }
        if (watched != nullptr && !waiting) {
            const uint32_t seq = ++s_sent;
            if (httpd_queue_work(watched, beat, (void*)(uintptr_t)seq) == ESP_OK) {
                waiting = true;
                sent_us = esp_timer_get_time();
            }
        }
        xSemaphoreGive(s_mutex);

        if (!waiting) {
            continue;
        }
        if (s_acked.load() == s_sent) {
            if (reported) {
                ESP_LOGW(TAG, "Web server thread answering again after %" PRId64 " ms",
                         (s_acked_us.load() - sent_us) / 1000);
            }
            waiting = false;
            reported = false;
            continue;
        }
        const int64_t stuck_us = esp_timer_get_time() - sent_us;
        if (!reported && stuck_us >= kStallUs && s_busy.load() == 0) {
            reported = true;
            report(stuck_us);
        }
    }
}

}  // namespace

void httpd_watch_attach(httpd_handle_t server)
{
    if (s_mutex == nullptr) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == nullptr) {
            return;
        }
    }
    // PSRAM stack: it only logs and reads task state. Above the server tasks
    // (5) so it still runs when they are the ones hogging the CPU.
    if (s_task == nullptr &&
        xTaskCreateWithCaps(watch_task, "httpd_watch", 4096, nullptr, 6, &s_task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        s_task = nullptr;
        ESP_LOGW(TAG, "Cannot start the web server stall detector");
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_server = server;
    xSemaphoreGive(s_mutex);
}

void httpd_watch_detach(void)
{
    if (s_mutex == nullptr) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_server = nullptr;
    xSemaphoreGive(s_mutex);
}

void httpd_watch_busy_begin(void)
{
    s_busy.fetch_add(1);
}

void httpd_watch_busy_end(void)
{
    s_busy.fetch_sub(1);
}
