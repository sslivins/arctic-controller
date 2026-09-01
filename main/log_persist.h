/*
 * Arctic Heat Pump Controller
 * Persistent debug log
 *
 * Snapshots the tail of the RAM log ring (log_buffer) into flash so that after
 * the device wedges-and-reboots (or is power-cycled to recover a network wedge)
 * the run-up to the failure survives. The RAM ring alone is lost on reboot; the
 * flash-backed event log only stores structured events, not raw log text.
 *
 * Storage: a small round-robin of fixed-size slots in a dedicated flash region.
 * Each slot holds a CRC-validated text snapshot. On boot we locate the newest
 * valid slot and expose it as the "previous boot" tail via
 * log_persist_get_previous() / GET /api/logs/persisted. The background task
 * re-snapshots on new WARN/ERROR activity (debounced) and on a slow heartbeat.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Mount the flash region and load the newest valid snapshot from the PREVIOUS
// boot into RAM (for log_persist_get_previous). Safe to call once, early, after
// log_buffer_init(). Returns true if the flash region was found.
bool log_persist_init(void);

// Start the low-priority background task that periodically snapshots the log
// tail and emits netdiag lines. Call after WiFi/servers are up.
void log_persist_start(void);

// Force an immediate snapshot of the current log tail to flash.
// @param reason  short code stored in the record header (see LOG_PERSIST_REASON_*).
void log_persist_flush_now(uint8_t reason);

// Copy the previous-boot snapshot (loaded at init) into @p out as a
// NUL-terminated string. Returns the number of text bytes available (may be
// larger than cap-1 if truncated). Returns 0 when no valid snapshot exists.
size_t log_persist_get_previous(char* out, size_t cap);

// Metadata about the previous-boot snapshot (all zero if none).
typedef struct {
    bool     present;
    uint32_t seq;         // monotonic record sequence
    uint32_t boot_id;     // random id of the boot that wrote it
    uint32_t len;         // text length in bytes
    uint8_t  reason;      // why it was written (LOG_PERSIST_REASON_*)
} log_persist_prev_info_t;

void log_persist_get_previous_info(log_persist_prev_info_t* out);

#define LOG_PERSIST_REASON_HEARTBEAT 0
#define LOG_PERSIST_REASON_SEVERITY  1
#define LOG_PERSIST_REASON_MANUAL    2

#ifdef __cplusplus
}
#endif
