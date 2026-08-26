#pragma once

#include "event_log.h"
#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>

/**
 * The first 256 KB of the history partition is a dual-bank event journal.
 * The rest is reserved for the future telemetry timeline.
 */
#define HISTORY_EVENT_REGION_SIZE (256 * 1024)
#define HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC 30
#define HISTORY_TELEMETRY_RETENTION_DAYS 14
#define HISTORY_TELEMETRY_PAGE_CAPACITY 1024

typedef enum {
    HISTORY_TELEMETRY_MODE_UNKNOWN = 0,
    HISTORY_TELEMETRY_MODE_HEATING = 1,
    HISTORY_TELEMETRY_MODE_COOLING = 2,
    HISTORY_TELEMETRY_MODE_HOT_WATER = 3,
} history_telemetry_mode_t;

enum {
    HISTORY_TELEMETRY_CONNECTED = 1 << 0,
    HISTORY_TELEMETRY_COMPRESSOR_VALID = 1 << 1,
    HISTORY_TELEMETRY_COMPRESSOR_RUNNING = 1 << 2,
    HISTORY_TELEMETRY_INLET_VALID = 1 << 3,
    HISTORY_TELEMETRY_OUTLET_VALID = 1 << 4,
    HISTORY_TELEMETRY_SETPOINT_VALID = 1 << 5,
};

typedef struct {
    uint32_t timestamp;
    uint32_t sequence;
    int16_t inlet_deci_c;
    int16_t outlet_deci_c;
    int16_t setpoint_deci_c;
    uint8_t mode;
    uint8_t flags;
} history_telemetry_sample_t;

esp_err_t history_storage_init(void);

/**
 * Load events oldest-first into contiguous output arrays.
 */
esp_err_t history_storage_load_events(event_entry_t* entries,
                                      uint32_t* event_ids,
                                      size_t capacity,
                                      size_t* count,
                                      uint32_t* next_event_id);

/**
 * Append a new version of an event. Updating an existing event uses the same
 * event ID, which preserves timestamp backfills without rewriting flash.
 */
esp_err_t history_storage_append_event(uint32_t event_id, const event_entry_t* entry);

/**
 * Atomically compact a ring buffer into the inactive bank. The old committed
 * bank remains valid until the replacement bank is fully written and committed.
 */
esp_err_t history_storage_replace_events(const event_entry_t* entries,
                                         const uint32_t* event_ids,
                                         size_t capacity,
                                         size_t head,
                                         size_t count);

esp_err_t history_storage_append_telemetry(
    const history_telemetry_sample_t* sample);

esp_err_t history_storage_query_telemetry(
    uint32_t start_timestamp,
    uint32_t end_timestamp,
    history_telemetry_sample_t* samples,
    size_t capacity,
    size_t* count);

esp_err_t history_storage_latest_telemetry_timestamp(uint32_t* timestamp);

/**
 * Block new telemetry operations and wait for an in-flight operation before
 * the history partition is erased.
 */
void history_storage_prepare_factory_reset(void);

/**
 * Permanently stop accepting new event/telemetry flash writes and wait for any
 * in-flight write to complete. Call immediately before rebooting so that no
 * esp_partition operation is running when esp_restart_noos() stalls the other
 * core and runs Cache_WriteBack_All() (an in-flight SPI-flash op would make the
 * cache write-back fault with a Store access fault in ROM). Not reversible.
 */
void history_storage_begin_reboot(void);

#ifdef CONFIG_TEST_ENDPOINTS
esp_err_t history_storage_seed_telemetry_for_test(
    const history_telemetry_sample_t* samples,
    size_t count);
#endif
