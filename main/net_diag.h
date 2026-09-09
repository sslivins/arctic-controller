/*
 * Arctic Heat Pump Controller
 * Network diagnostics snapshot
 *
 * Emits a single ESP_LOGI("netdiag", ...) line summarising heap health and the
 * live lwIP TCP PCB population (active / TIME_WAIT / bound / listening). The PCB
 * counts are the key signal for the load-induced network wedge: if the device
 * stops answering while these climb toward their configured ceilings
 * (LWIP_MAX_ACTIVE_TCP, the TIME_WAIT backlog under LWIP_TCP_MSL), it is PCB
 * exhaustion rather than a WiFi/association drop.
 *
 * Because CONFIG_LWIP_TCPIP_CORE_LOCKING is disabled, the PCB lists are walked
 * on the tcpip thread via tcpip_callback() rather than raced from the caller.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Heap (bytes)
    unsigned free_internal;
    unsigned min_free_internal;   // low-water since boot
    unsigned largest_free_internal; // biggest single block; distinguishes
                                    // fragmentation from true exhaustion
    unsigned free_psram;
    // lwIP TCP PCB population
    int tcp_active;               // established / in-progress connections
    int tcp_time_wait;            // sockets lingering in TIME_WAIT
    int tcp_bound;                // bound but not listening/connected
    int tcp_listen;               // listening sockets (our HTTPS servers)
    bool pcb_valid;               // false if the tcpip callback timed out
} net_diag_t;

// Gather a snapshot (blocks briefly on the tcpip thread for the PCB walk).
void net_diag_sample(net_diag_t* out);

// Gather + emit an ESP_LOGI("netdiag", ...) line. Convenience wrapper.
void net_diag_log_snapshot(void);

// Threshold for treating internal heap as critically low. Healthy runs sit at
// 30-40 KB free internal; observed excursions collapse to double digits. 12 KB
// is low enough not to fire in normal operation but high enough to catch the
// descent rather than only the floor.
#define NET_DIAG_LOW_HEAP_BYTES 12288

// Start a lightweight task that polls internal free heap every
// NET_DIAG_WATCH_PERIOD_MS and profiles any excursion below
// NET_DIAG_LOW_HEAP_BYTES: when it began, how deep it went, how long it
// lasted, and the largest free block at the floor.
//
// Deliberately does NOT attribute the memory to a task. The obvious tool for
// that, heap_caps_get_per_task_info() under CONFIG_HEAP_TASK_TRACKING, walks
// every block in every heap while holding the heap spinlock with interrupts
// disabled. With ~30 MB of PSRAM attached that walk is long enough to trip the
// interrupt watchdog: calling it from this poller panicked the device with
// "Interrupt wdt timeout on CPU1" during ESP-Hosted SDIO bring-up and left it
// in a boot loop that OTA could not recover (see #234). The heap dips exactly
// when the system is busiest, so a low-heap trigger is the worst possible
// moment to start a long critical section.
//
// Profiling the excursion is safe -- it is only counter reads -- and still
// narrows the search, because the timestamps can be lined up against what the
// device was doing.
//
// The periodic snapshot cadence is far too slow to see the event: in the
// post-#249 run every 30s sample read ~37 KB free, yet
// heap_caps_get_minimum_free_size reported an all-time low of 16 bytes -- the
// collapse and the recovery both fell between two samples.
//
// Safe to call more than once; subsequent calls are ignored.
void net_diag_start_low_heap_watch(void);

// 100ms is a deliberate compromise. The dips coincide with multi-second
// screenshot/PSRAM activity, so they are very unlikely to be shorter than
// this, while the poll itself is only a counter read per heap.
#define NET_DIAG_WATCH_PERIOD_MS 100

#ifdef __cplusplus
}
#endif
