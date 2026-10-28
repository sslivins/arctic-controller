/*
 * Arctic Heat Pump Controller
 * NVS persistence for per-run energy summaries (run_ledger.h).
 *
 * Namespace "runs", one blob per local day ("dYYYYMMDD") holding that day's
 * finished runs in start order. A blob is rewritten as each run ends (a few
 * dozen small writes a day) and days older than kRunDaysKept, about as long as
 * the telemetry the run lists come from, are erased.
 */
#pragma once

#include "run_ledger.h"

#include <stddef.h>
#include <stdint.h>

namespace energy {

constexpr int kRunDaysKept = 15;
constexpr size_t kMaxRunsPerDay = 96;

// Add a finished run to its start day. A run already stored (same start) is
// replaced; a full day keeps the runs it has.
bool store_append_run(const RunRecord& run);

// The runs stored for one day, in start order.
size_t store_read_runs(uint32_t date, RunRecord* out, size_t capacity);

// Erase every stored run (test fixtures).
void store_clear_runs();

}  // namespace energy
