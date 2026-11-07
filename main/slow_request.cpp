/*
 * Arctic Heat Pump Controller
 * Worker task for slow HTTP handlers (see slow_request.h).
 */
#include "slow_request.h"

#include <atomic>
#include <sys/socket.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "bsp/m5stack_tab5.h"

static const char* TAG = "slow_req";

namespace {

struct Job {
    httpd_req_t* req;
    slow_request_handler_t handler;
};

// A few requests may wait; beyond that the client is told to retry rather
// than holding a socket (and an async request copy) for a long time.
constexpr UBaseType_t kQueueDepth = 3;
// PSRAM stack: the handlers render the screen and stream it, and never touch
// flash, so the internal RAM a server-sized stack would cost is not needed.
constexpr uint32_t kStackSize = 16384;
// Below the server tasks (priority 5), so the worker never delays them.
constexpr UBaseType_t kPriority = 4;
// The stalled-client case is bounded by the socket send timeout; this only
// guards against something unforeseen holding the worker.
constexpr int64_t kDrainTimeoutUs = 20 * 1000 * 1000;

QueueHandle_t s_queue = nullptr;
TaskHandle_t s_task = nullptr;
SemaphoreHandle_t s_mutex = nullptr;
// Queued plus running jobs. Guarded by s_mutex for increments, so the drain
// cannot miss a job that is being queued as it starts.
std::atomic<int> s_pending{0};
std::atomic<bool> s_abort{false};
std::atomic<int> s_current_fd{-1};

void worker(void*)
{
    for (;;) {
        Job job;
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        const int fd = httpd_req_to_sockfd(job.req);
        httpd_handle_t handle = job.req->handle;
        esp_err_t ret = ESP_FAIL;
        if (!s_abort.load()) {
            s_current_fd.store(fd);
            ret = job.handler(job.req);
            s_current_fd.store(-1);
        }
        httpd_req_async_handler_complete(job.req);
        if (ret != ESP_OK) {
            // Same as returning an error on the server thread: drop the
            // connection, since a partial response cannot be recovered.
            httpd_sess_trigger_close(handle, fd);
        }
        s_pending.fetch_sub(1);
    }
}

bool ensure_worker(void)
{
    if (s_task != nullptr) {
        return true;
    }
    if (s_mutex == nullptr) {
        s_mutex = xSemaphoreCreateMutex();
    }
    if (s_queue == nullptr) {
        s_queue = xQueueCreate(kQueueDepth, sizeof(Job));
    }
    if (s_mutex == nullptr || s_queue == nullptr) {
        return false;
    }
    if (xTaskCreateWithCaps(worker, "slow_req", kStackSize, nullptr, kPriority,
                            &s_task, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_task = nullptr;
        return false;
    }
    return true;
}

esp_err_t send_busy(httpd_req_t* req, const char* why)
{
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Retry-After", "2");
    char body[96];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", why);
    return httpd_resp_sendstr(req, body);
}

}  // namespace

bool slow_request_on_worker(void)
{
    return s_task != nullptr && xTaskGetCurrentTaskHandle() == s_task;
}

bool slow_request_aborting(void)
{
    return s_abort.load();
}

bool slow_request_display_lock(void)
{
    while (!bsp_display_lock(100)) {
        if (s_abort.load()) {
            return false;
        }
    }
    return true;
}

esp_err_t slow_request_defer(httpd_req_t* req, slow_request_handler_t handler)
{
    // Only the server threads call this, and they start the worker lazily
    // so a controller nobody takes screenshots of never pays for it.
    if (!ensure_worker()) {
        ESP_LOGE(TAG, "Cannot start the slow-request worker");
        return send_busy(req, "Out of memory");
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_abort.load()) {
        xSemaphoreGive(s_mutex);
        return send_busy(req, "Server restarting");
    }
    if (uxQueueSpacesAvailable(s_queue) == 0) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "Busy: %s rejected", req->uri);
        return send_busy(req, "Busy, try again shortly");
    }
    httpd_req_t* copy = nullptr;
    if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
        xSemaphoreGive(s_mutex);
        return send_busy(req, "Out of memory");
    }
    s_pending.fetch_add(1);
    Job job = {copy, handler};
    // Cannot fail: space was checked under the mutex and the worker only
    // ever removes items.
    xQueueSend(s_queue, &job, 0);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void slow_request_drain(void)
{
    if (s_task == nullptr) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_abort.store(true);
    xSemaphoreGive(s_mutex);
    if (s_pending.load() == 0) {
        return;
    }
    ESP_LOGI(TAG, "Waiting for %d slow request(s) to stop", s_pending.load());
    // Unblock a send to a client that stopped reading.
    const int fd = s_current_fd.load();
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
    }
    const int64_t deadline = esp_timer_get_time() + kDrainTimeoutUs;
    while (s_pending.load() > 0 && esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_pending.load() > 0) {
        ESP_LOGE(TAG, "%d slow request(s) still running at server stop",
                 s_pending.load());
    }
}

void slow_request_resume(void)
{
    s_abort.store(false);
}
