/*
 * Arctic Heat Pump Controller
 * Network diagnostics snapshot — see net_diag.h for rationale.
 */
#include "net_diag.h"

#include <string.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "lwip/tcpip.h"
#include "lwip/priv/tcp_priv.h"   // tcp_active_pcbs, tcp_tw_pcbs, tcp_bound_pcbs, tcp_listen_pcbs

static const char* TAG = "netdiag";

// ---------------------------------------------------------------------------
// PCB counting (runs on the tcpip thread)
// ---------------------------------------------------------------------------

typedef struct {
    int active;
    int time_wait;
    int bound;
    int listen;
    SemaphoreHandle_t done;
} pcb_walk_t;

static void count_pcbs_on_tcpip(void* arg)
{
    pcb_walk_t* w = (pcb_walk_t*)arg;
    int a = 0, t = 0, b = 0, l = 0;

    for (struct tcp_pcb* p = tcp_active_pcbs; p != NULL; p = p->next) a++;
    for (struct tcp_pcb* p = tcp_tw_pcbs; p != NULL; p = p->next) t++;
    for (struct tcp_pcb* p = tcp_bound_pcbs; p != NULL; p = p->next) b++;
    for (struct tcp_pcb_listen* p = tcp_listen_pcbs.listen_pcbs; p != NULL; p = p->next) l++;

    w->active = a;
    w->time_wait = t;
    w->bound = b;
    w->listen = l;
    xSemaphoreGive(w->done);
}

static bool sample_pcbs(net_diag_t* out)
{
    pcb_walk_t w;
    memset(&w, 0, sizeof(w));
    w.done = xSemaphoreCreateBinary();
    if (!w.done) return false;

    bool ok = false;
    if (tcpip_callback(count_pcbs_on_tcpip, &w) == ERR_OK) {
        // The tcpip thread is normally very responsive; 1s is a generous cap so
        // a wedged stack can't block the diagnostic task indefinitely.
        if (xSemaphoreTake(w.done, pdMS_TO_TICKS(1000)) == pdTRUE) {
            out->tcp_active    = w.active;
            out->tcp_time_wait = w.time_wait;
            out->tcp_bound     = w.bound;
            out->tcp_listen    = w.listen;
            ok = true;
        }
    }
    vSemaphoreDelete(w.done);
    return ok;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void net_diag_sample(net_diag_t* out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    out->free_internal     = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->min_free_internal = (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    out->largest_free_internal =
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    out->free_psram        = (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    out->pcb_valid = sample_pcbs(out);
}

void net_diag_log_snapshot(void)
{
    net_diag_t d;
    net_diag_sample(&d);

    if (d.pcb_valid) {
        ESP_LOGI(TAG,
                 "heap int=%u (min %u, largest %u) psram=%u | tcp active=%d tw=%d bound=%d listen=%d",
                 d.free_internal, d.min_free_internal, d.largest_free_internal,
                 d.free_psram,
                 d.tcp_active, d.tcp_time_wait, d.tcp_bound, d.tcp_listen);
    } else {
        // A timed-out PCB walk is itself a strong wedge signal: the tcpip thread
        // did not service our callback within 1s.
        ESP_LOGW(TAG,
                 "heap int=%u (min %u, largest %u) psram=%u | tcp PCB walk TIMED OUT (tcpip thread unresponsive)",
                 d.free_internal, d.min_free_internal, d.largest_free_internal,
                 d.free_psram);
    }
}

// ---------------------------------------------------------------------------
// Fast low-heap watch
// ---------------------------------------------------------------------------

static void net_diag_low_heap_watch_task(void* arg)
{
    (void)arg;

    // Everything here must stay cheap. This task runs 10x/sec and fires
    // precisely when the system is under its heaviest memory pressure, so it
    // must not allocate, must not take the heap lock for long, and must not
    // walk the heap. See the header for the interrupt-watchdog panic that
    // resulted from doing exactly that.
    bool     in_dip     = false;
    unsigned dip_floor  = 0;
    unsigned dip_largest = 0;
    int64_t  dip_start_us = 0;
    unsigned last_min   = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(NET_DIAG_WATCH_PERIOD_MS));

        const unsigned freeb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

        if (!in_dip && freeb < NET_DIAG_LOW_HEAP_BYTES) {
            in_dip       = true;
            dip_start_us = esp_timer_get_time();
            dip_floor    = freeb;
            dip_largest  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
            ESP_LOGW(TAG, "LOW HEAP entered: int=%u largest=%u psram=%u",
                     freeb, dip_largest,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        } else if (in_dip) {
            if (freeb < dip_floor) {
                dip_floor   = freeb;
                dip_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
            }
            // Require a clear recovery before closing the excursion, so a
            // value oscillating around the threshold reports once rather than
            // repeatedly.
            if (freeb > (NET_DIAG_LOW_HEAP_BYTES * 2)) {
                const int64_t dur_ms = (esp_timer_get_time() - dip_start_us) / 1000;
                ESP_LOGW(TAG,
                         "LOW HEAP recovered: floor=%u largest_at_floor=%u duration=%lldms now=%u",
                         dip_floor, dip_largest, (long long)dur_ms, freeb);
                in_dip = false;
            }
        }

        // Backstop. If the all-time low-water mark moves without this poller
        // having seen a dip, the excursion was shorter than the poll interval.
        // Saying so explicitly matters: otherwise a quiet log is ambiguous
        // between "no dip occurred" and "the dip was too fast to observe".
        const unsigned now_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        if (now_min < last_min) {
            if (now_min < NET_DIAG_LOW_HEAP_BYTES && !in_dip) {
                ESP_LOGW(TAG,
                         "MISSED dip: low-water fell %u -> %u between polls (shorter than %dms)",
                         last_min, now_min, NET_DIAG_WATCH_PERIOD_MS);
            }
            last_min = now_min;
        }
    }
}

void net_diag_start_low_heap_watch(void)
{
    static bool started = false;
    if (started) {
        return;
    }
    started = true;

    // Small stack: this task only reads counters and calls the attribution
    // dump, which uses static storage. Low priority so it never preempts the
    // network path it is observing.
    BaseType_t ok = xTaskCreate(net_diag_low_heap_watch_task, "netdiag_lowheap",
                                3072, NULL, 1, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to start low-heap watch task");
        started = false;
    }
}
