/*
 * Steerable wall clock, overriding libc's time().
 *
 * event_log.cpp stamps every entry with time(), and treats anything before
 * 2024-01-01 as "the clock is not set yet" -- which is the entire reason
 * event_log_time_synced() exists. On the host, time() always returns the
 * present, so the unsynced branch and the backfill that repairs it can never
 * be reached. Tests would silently cover only the happy path.
 *
 * Overriding time() rather than adding a seam to event_log.cpp keeps the
 * production source exactly as it ships. This translation unit is linked only
 * into test_event_log, so no other suite sees a doctored clock.
 *
 * Pair with timer_fake (esp_timer.h) when a test cares about the relationship
 * between wall time and uptime: the backfill derives one from the other.
 */
#pragma once

#include <time.h>

namespace time_fake {

// 2025-01-01T00:00:00Z. Comfortably past event_log's "clock is set" threshold.
constexpr time_t DEFAULT_NOW = 1735689600;

// Anything below this reads as "time not synced" to event_log.cpp.
constexpr time_t SYNC_THRESHOLD = 1704067200;

void set(time_t now);
time_t get();

// Back to DEFAULT_NOW.
void reset();

}  // namespace time_fake
