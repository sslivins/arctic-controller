#pragma once

#include "history_storage.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Compressor runs and the tank trend for the home screen, derived from the
 * telemetry history rather than tracked separately.
 */
typedef struct {
    uint32_t start;   // Unix seconds of the first running sample
    uint32_t end;     // Unix seconds the run was seen stopped; 0 = still running
    uint8_t mode;     // history_telemetry_mode_t
    bool seen_start;  // the sample before it was a stopped one (not a gap)
} home_run_t;

typedef struct {
    home_run_t current;   // in progress at the newest sample; start == 0 if none
    home_run_t last;      // most recent finished run; start == 0 if none
    uint16_t starts_last_hour;
    bool setpoint_valid;
    int16_t setpoint_deci_c;  // newest valid setpoint
} home_run_summary_t;

/**
 * Split samples (oldest first) into compressor runs. Samples further apart
 * than three sample intervals are a gap: a run open across one ends at the
 * last sample before it. A run still open at the newest sample is "current"
 * only if that sample is recent relative to `now`.
 *
 * Writes the newest runs (finished and current) to out, oldest first.
 * @return Number of runs written (at most cap).
 */
size_t home_stats_runs(const history_telemetry_sample_t* samples, size_t n,
                       uint32_t now, home_run_t* out, size_t cap,
                       home_run_summary_t* summary);

#define HOME_STATS_MAX_BUCKETS 240

/**
 * Average the tank reading into `buckets` equal slices of [start, end).
 * A slice with no tank reading has valid[i] == false. At most
 * HOME_STATS_MAX_BUCKETS slices are filled.
 */
void home_stats_tank_series(const history_telemetry_sample_t* samples, size_t n,
                            uint32_t start, uint32_t end, int16_t* deci_c,
                            bool* valid, size_t buckets);
