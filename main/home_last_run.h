#pragma once

#include "home_stats.h"

/**
 * When the summary window (samples from window_start) can't settle the last
 * run, look back through the rest of the retained history (up to `end`
 * minus the retention period) for it. Reads all of the telemetry flash and
 * briefly holds a ~800 KB PSRAM buffer: call from a worker or httpd task.
 */
void home_last_run_resolve(home_run_summary_t* sum,
                           const history_telemetry_sample_t* window, size_t n,
                           uint32_t window_start, uint32_t end);
