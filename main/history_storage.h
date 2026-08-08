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
