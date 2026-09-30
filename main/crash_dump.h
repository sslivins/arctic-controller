/*
 * Arctic Heat Pump Controller
 * Core dump access
 *
 * On a panic or task-watchdog abort, ESP-IDF writes an ELF core dump (every
 * task's registers and stack) to the `coredump` flash partition. This module
 * reports a saved dump at boot and lets GET/DELETE /api/logs/coredump read and
 * clear it, so a crash can be decoded against the firmware's ELF afterwards.
 *
 * Devices still on the partition table from before #208 have no `coredump`
 * partition. For them every call reports "no dump" and nothing else changes.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

// Log whether the previous boot left a core dump, and if so which task
// crashed and where. Call once at boot, after the log buffer is up.
void crash_dump_report_at_boot(void);

// Size in bytes of the saved core dump, or 0 if there is none (or it is
// corrupt, or the device has no coredump partition).
size_t crash_dump_size(void);

// Read @p len bytes of the saved dump starting at @p offset.
esp_err_t crash_dump_read(size_t offset, void* buf, size_t len);

// Erase the saved dump. Succeeds when there is nothing to erase.
esp_err_t crash_dump_erase(void);

#ifdef __cplusplus
}
#endif
