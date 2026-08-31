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

#ifdef __cplusplus
}
#endif
