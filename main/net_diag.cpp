/*
 * Arctic Heat Pump Controller
 * Network diagnostics snapshot — see net_diag.h for rationale.
 */
#include "net_diag.h"

#include <string.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

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
    out->free_psram        = (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    out->pcb_valid = sample_pcbs(out);
}

void net_diag_log_snapshot(void)
{
    net_diag_t d;
    net_diag_sample(&d);

    if (d.pcb_valid) {
        ESP_LOGI(TAG,
                 "heap int=%u (min %u) psram=%u | tcp active=%d tw=%d bound=%d listen=%d",
                 d.free_internal, d.min_free_internal, d.free_psram,
                 d.tcp_active, d.tcp_time_wait, d.tcp_bound, d.tcp_listen);
    } else {
        // A timed-out PCB walk is itself a strong wedge signal: the tcpip thread
        // did not service our callback within 1s.
        ESP_LOGW(TAG,
                 "heap int=%u (min %u) psram=%u | tcp PCB walk TIMED OUT (tcpip thread unresponsive)",
                 d.free_internal, d.min_free_internal, d.free_psram);
    }
}
