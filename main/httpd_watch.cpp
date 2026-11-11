/*
 * Arctic Heat Pump Controller
 * Stall detector for the web UI/API server thread (see httpd_watch.h).
 */
#include "httpd_watch.h"
#include "task_report.h"

#include <atomic>
#include <inttypes.h>

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
            task_report(TAG, "Web server thread", stuck_us, s_server_task.load(), "<- web server");
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
