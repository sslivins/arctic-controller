/*
 * Arctic Heat Pump Controller
 * NVS persistence for the daily energy ledger (energy_ledger.h).
 *
 * Keys in the "energy" namespace:
 *   "today"    checkpoint of the open day (written every 10 min and on a
 *              planned restart, so an unplanned reset loses at most that).
 *   "mYYYYMM"  one blob per month: up to 31 closed days plus the lifetime
 *              totals through the last of them. Day and totals share a blob
 *              so closing a day is a single nvs_set_blob + commit: either
 *              both land or neither does. Kept for kMonthsKept months.
 *
 * Lifetime totals count from the first day the ledger closed after install or
 * a factory reset (which erases NVS), tagged with a random reset_id so a
 * consumer such as Home Assistant can tell the counter started over.
 */
#pragma once

#include "energy_ledger.h"

#include <stddef.h>
#include <stdint.h>

namespace energy {

constexpr int kMonthsKept = 13;

struct Lifetime {
    uint32_t reset_id;
    uint32_t since_date;    // first day counted (YYYYMMDD)
    uint32_t through_date;  // last closed day counted
    uint32_t days;
    uint32_t elec_wh;
    uint32_t thermal_wh[kModes];
    uint32_t covered_elec_wh[kModes];
    uint32_t power_s;
    uint32_t thermal_s;
    uint32_t reserved;
};
static_assert(sizeof(Lifetime) == 56, "Lifetime is persisted; keep its layout");

// Add a closed day to the lifetime totals (in memory only).
void fold(Lifetime* life, const DayRecord& d);

bool store_load_today(DayAccum* out);
bool store_save_today(const DayAccum& accum);

// Store a closed day in its month and fold it into the lifetime totals. A day
// already counted (date <= through_date) is left alone, so committing twice
// after a crash is harmless. `life` receives the updated totals.
bool store_commit_day(const DayRecord& day, Lifetime* life);

// Lifetime totals from the newest month blob at or before `date`'s month.
// False when no day has closed yet.
bool store_load_lifetime(uint32_t date, Lifetime* out);

// Closed days in [from_date, to_date] (YYYYMMDD), oldest first.
size_t store_read_days(uint32_t from_date, uint32_t to_date, DayRecord* out, size_t capacity);

// Previous calendar month of a YYYYMM value.
inline uint32_t prev_month(uint32_t yyyymm) {
    return (yyyymm % 100) <= 1 ? (yyyymm / 100 - 1) * 100 + 12 : yyyymm - 1;
}
inline uint32_t next_month(uint32_t yyyymm) {
    return (yyyymm % 100) >= 12 ? (yyyymm / 100 + 1) * 100 + 1 : yyyymm + 1;
}

}  // namespace energy
