/*
 * Arctic Heat Pump Controller
 * Network diagnostics snapshot — see net_diag.h for rationale.
 */
#include "net_diag.h"

#include <stdio.h>
#include <string.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "lwip/tcpip.h"
#include "lwip/priv/tcp_priv.h"   // tcp_active_pcbs, tcp_tw_pcbs, tcp_bound_pcbs, tcp_listen_pcbs
#include "lwip/stats.h"
#include "lwip/etharp.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"

#include "wifi_manager.h"

static const char* TAG = "netdiag";

// Defined in the vendored esp_hosted transport_drv.c (Arctic diagnostic hooks).
extern "C" void esp_hosted_get_tx_throttle_stats(bool* active, uint32_t* on_count,
                                                 uint32_t* drops, uint32_t* held_ms);
extern "C" void esp_hosted_get_sta_pkt_counts(uint32_t* tx_queued, uint32_t* tx_sent,
                                              uint32_t* rx);

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

void net_diag_format_wifi(char* buf, size_t len)
{
    if (!buf || len == 0) return;
    const unsigned disc   = (unsigned)wifi_mgr_get_disconnect_count();
    const unsigned reason = (unsigned)wifi_mgr_get_last_disconnect_reason();

    if (wifi_mgr_get_state() != WIFI_MGR_STATE_CONNECTED) {
        snprintf(buf, len, "wifi not-connected disc=%u reason=%u", disc, reason);
        return;
    }

    bool thr = false;
    uint32_t thr_on = 0, thr_drops = 0, thr_ms = 0;
    esp_hosted_get_tx_throttle_stats(&thr, &thr_on, &thr_drops, &thr_ms);

    wifi_ap_record_t ap;
    const int64_t t0 = esp_timer_get_time();
    const esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
    const int rpc_ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (err != ESP_OK) {
        snprintf(buf, len, "wifi ap-info FAILED (%s) disc=%u reason=%u rpc=%dms thr=%s%lums/%lu/%lu",
                 esp_err_to_name(err), disc, reason, rpc_ms, thr ? "ON " : "off ",
                 (unsigned long)thr_ms, (unsigned long)thr_on, (unsigned long)thr_drops);
        return;
    }
    snprintf(buf, len, "wifi rssi=%d ch=%u ap=..%02x:%02x disc=%u reason=%u rpc=%dms thr=%s%lums/%lu/%lu",
             (int)ap.rssi, (unsigned)ap.primary, ap.bssid[4], ap.bssid[5],
             disc, reason, rpc_ms, thr ? "ON " : "off ",
             (unsigned long)thr_ms, (unsigned long)thr_on, (unsigned long)thr_drops);
}

// ---------------------------------------------------------------------------
// Outage packet counters (runs on the tcpip thread)
// ---------------------------------------------------------------------------

typedef struct {
    net_diag_counters_t res;
    struct netif* netif;
    ip4_addr_t gw;
} counter_walk_t;

// Static so a callback that runs after the caller gave up never touches a dead
// stack frame. s_counter_busy stays set until the callback finishes; while it
// is set no new walk is queued.
static counter_walk_t s_counter_walk;
static SemaphoreHandle_t s_counter_done;
static volatile bool s_counter_busy;

static void sample_counters_on_tcpip(void* arg)
{
    counter_walk_t* w = (counter_walk_t*)arg;
    net_diag_counters_t* c = &w->res;

#if LWIP_STATS
#if LINK_STATS
    c->link_tx = lwip_stats.link.xmit;
    c->link_rx = lwip_stats.link.recv;
    c->link_drop = lwip_stats.link.drop;
    c->link_err = lwip_stats.link.err;
#endif
#if ETHARP_STATS
    c->arp_tx = lwip_stats.etharp.xmit;
    c->arp_rx = lwip_stats.etharp.recv;
#endif
#if IP_STATS
    c->ip_tx = lwip_stats.ip.xmit;
    c->ip_rx = lwip_stats.ip.recv;
    c->ip_drop = lwip_stats.ip.drop;
    c->ip_rterr = lwip_stats.ip.rterr;
#endif
#if ICMP_STATS
    c->icmp_tx = lwip_stats.icmp.xmit;
    c->icmp_rx = lwip_stats.icmp.recv;
#endif
#endif

    if (w->netif && ip4_addr_get_u32(&w->gw) != 0) {
        for (size_t i = 0; i < ARP_TABLE_SIZE; i++) {
            ip4_addr_t* ip = NULL;
            struct netif* nif = NULL;
            struct eth_addr* eth = NULL;
            if (!etharp_get_entry(i, &ip, &nif, &eth) || nif != w->netif) continue;
            if (ip4_addr_get_u32(ip) == ip4_addr_get_u32(&w->gw)) {
                c->gw_arp = true;
                memcpy(c->gw_mac, eth->addr, sizeof(c->gw_mac));
            } else if (c->peer_ip == 0) {
                c->peer_ip = ip4_addr_get_u32(ip);
            }
        }
    }
    s_counter_busy = false;
    xSemaphoreGive(s_counter_done);
}

void net_diag_sample_counters(net_diag_counters_t* out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    uint32_t q = 0, sent = 0, rx = 0;
    esp_hosted_get_sta_pkt_counts(&q, &sent, &rx);

    if (!s_counter_done) {
        s_counter_done = xSemaphoreCreateBinary();
    }
    if (s_counter_done && !s_counter_busy) {
        counter_walk_t* w = &s_counter_walk;
        memset(w, 0, sizeof(*w));
        esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta) {
            esp_netif_ip_info_t info;
            if (esp_netif_get_ip_info(sta, &info) == ESP_OK) {
                ip4_addr_set_u32(&w->gw, info.gw.addr);
            }
            w->netif = (struct netif*)esp_netif_get_netif_impl(sta);
        }
        xSemaphoreTake(s_counter_done, 0);  // clear a stale give from a late callback
        s_counter_busy = true;
        if (tcpip_callback(sample_counters_on_tcpip, w) != ERR_OK) {
            s_counter_busy = false;
        } else if (xSemaphoreTake(s_counter_done, pdMS_TO_TICKS(1000)) == pdTRUE) {
            *out = w->res;
            out->valid = true;
        }
    }
    out->hosted_q = q;
    out->hosted_sent = sent;
    out->hosted_rx = rx;
}

void net_diag_format_counters(char* buf, size_t len, const net_diag_counters_t* base)
{
    if (!buf || len == 0) return;
    net_diag_counters_t now;
    net_diag_sample_counters(&now);
    if (!now.valid) {
        snprintf(buf, len, "counters TIMED OUT (tcpip thread unresponsive) hosted q=%u sent=%u rx=%u",
                 now.hosted_q, now.hosted_sent, now.hosted_rx);
        return;
    }
    net_diag_counters_t zero;
    memset(&zero, 0, sizeof(zero));
    const net_diag_counters_t* b = (base && base->valid) ? base : &zero;
    char arp[24];
    if (now.gw_arp) {
        snprintf(arp, sizeof(arp), "..:%02x:%02x:%02x", now.gw_mac[3], now.gw_mac[4], now.gw_mac[5]);
    } else {
        snprintf(arp, sizeof(arp), "none");
    }
    snprintf(buf, len,
             "link tx+%u rx+%u drop+%u err+%u | arp tx+%u rx+%u | ip tx+%u rx+%u drop+%u rterr+%u | "
             "icmp tx+%u rx+%u | hosted q+%u sent+%u rx+%u | gw_arp=%s",
             now.link_tx - b->link_tx, now.link_rx - b->link_rx,
             now.link_drop - b->link_drop, now.link_err - b->link_err,
             now.arp_tx - b->arp_tx, now.arp_rx - b->arp_rx,
             now.ip_tx - b->ip_tx, now.ip_rx - b->ip_rx,
             now.ip_drop - b->ip_drop, now.ip_rterr - b->ip_rterr,
             now.icmp_tx - b->icmp_tx, now.icmp_rx - b->icmp_rx,
             now.hosted_q - b->hosted_q, now.hosted_sent - b->hosted_sent,
             now.hosted_rx - b->hosted_rx, arp);
}

void net_diag_log_snapshot(void)
{
    net_diag_t d;
    net_diag_sample(&d);

    char wifi[112];
    net_diag_format_wifi(wifi, sizeof(wifi));

    if (d.pcb_valid) {
        ESP_LOGI(TAG,
                 "heap int=%u (min %u, largest %u) psram=%u | tcp active=%d tw=%d bound=%d listen=%d | %s",
                 d.free_internal, d.min_free_internal, d.largest_free_internal,
                 d.free_psram,
                 d.tcp_active, d.tcp_time_wait, d.tcp_bound, d.tcp_listen, wifi);
    } else {
        // A timed-out PCB walk is itself a strong wedge signal: the tcpip thread
        // did not service our callback within 1s.
        ESP_LOGW(TAG,
                 "heap int=%u (min %u, largest %u) psram=%u | tcp PCB walk TIMED OUT (tcpip thread unresponsive) | %s",
                 d.free_internal, d.min_free_internal, d.largest_free_internal,
                 d.free_psram, wifi);
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
