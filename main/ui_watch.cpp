/*
 * Arctic Heat Pump Controller
 * Stall detector for the LVGL (UI) thread (see ui_watch.h).
 */
#include "ui_watch.h"
#include "task_report.h"

#include <atomic>
#include <inttypes.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <lvgl.h>

static const char* TAG = "ui_watch";

namespace {

constexpr uint32_t kBeatMs = 250;
constexpr TickType_t kCheckPeriod = pdMS_TO_TICKS(250);
// UI work is a few ms per frame; a screen rebuild well under a second.
constexpr int64_t kStallUs = 2 * 1000 * 1000;

std::atomic<int64_t> s_beat_us{0};
std::atomic<TaskHandle_t> s_ui_task{nullptr};
TaskHandle_t s_task = nullptr;

void beat(lv_timer_t*)
{
    s_beat_us.store(esp_timer_get_time());
}

void watch_task(void*)
{
    bool reported = false;
    int64_t stalled_since_us = 0;
    for (;;) {
        vTaskDelay(kCheckPeriod);
        const int64_t last = s_beat_us.load();
        const int64_t now = esp_timer_get_time();
        // A beat is due every kBeatMs; count the stall from when it was due.
        const int64_t stuck_us = now - last - (int64_t)kBeatMs * 1000;
        if (stuck_us < kStallUs) {
            if (reported) {
                ESP_LOGW(TAG, "UI thread running again after %" PRId64 " ms",
                         (last - stalled_since_us) / 1000);
                reported = false;
            }
            continue;
        }
        if (!reported) {
            reported = true;
            stalled_since_us = last + (int64_t)kBeatMs * 1000;
            task_report(TAG, "UI thread", stuck_us, s_ui_task.load(), "<- UI thread");
        }
    }
}

}  // namespace

void ui_watch_start(void)
{
    if (s_task != nullptr) {
        return;
    }
    task_report_register_lock("LVGL", lvgl_port_get_lock());
    s_ui_task.store(xTaskGetHandle("taskLVGL"));
    s_beat_us.store(esp_timer_get_time());
    lv_timer_create(beat, kBeatMs, nullptr);
    // PSRAM stack: it only logs and reads task state. Above taskLVGL (5) so it
    // still runs when the UI thread is the one hogging the CPU.
    if (xTaskCreateWithCaps(watch_task, "ui_watch", 4096, nullptr, 6, &s_task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        s_task = nullptr;
        ESP_LOGW(TAG, "Cannot start the UI thread stall detector");
    }
}
