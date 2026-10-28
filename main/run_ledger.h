/*
 * Arctic Heat Pump Controller
 * Per-run energy summary: estimated COP and outdoor temperature for each
 * compressor run.
 *
 * The 30 s telemetry samples that the run lists are built from carry no heat
 * output or outdoor reading, so RunTracker sums them live from the same
 * readings as the energy ledger and with the same rules (energy_ledger.h):
 * heat is only counted over intervals with a valid estimate, together with
 * the electricity of those intervals, and the outdoor average leaves out
 * defrost and the couple of minutes after it.
 *
 * A run starts when the compressor does and ends when it stops, the unit
 * disconnects, or the readings have a gap. Run lists still come from the
 * telemetry; RunTotals attaches the summaries that overlap each one.
 *
 * Pure logic (no IDF, no clock): run_store.cpp persists it.
 */
#pragma once

#include "energy_ledger.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace energy {

// YYYYMMDD of a day count from days_from_date().
inline uint32_t date_from_days(int64_t z) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const int64_t m = mp + (mp < 10 ? 3 : -9);
    const int64_t y = yoe + era * 400 + (m <= 2);
    return (uint32_t)(y * 10000 + m * 100 + d);
}

inline uint32_t date_add_days(uint32_t date, int days) {
    return date_from_days(days_from_date(date) + days);
}

enum : uint8_t {
    RUN_OPEN = 1 << 0,  // still running (only ever in memory)
};

// A run's COP needs this much covered electricity: a run shorter than the
// settling time after a start has no estimate at all.
constexpr uint32_t kRunCopMinCoveredWh = 100;

// One compressor run. Persisted in NVS day blobs: only ever append fields.
struct RunRecord {
    uint32_t date;             // local YYYYMMDD the run started on
    uint32_t start_s;          // epoch
    uint32_t end_s;
    uint32_t elec_wh;
    uint32_t thermal_wh;       // heat in the mode's direction, while estimated
    uint32_t covered_elec_wh;  // electricity over those same intervals
    uint32_t outdoor_s;        // seconds in the outdoor average
    int16_t outdoor_avg_deci;
    uint8_t bucket;            // the mode it ran in longest
    uint8_t flags;
};
static_assert(sizeof(RunRecord) == 32, "RunRecord is persisted; keep its layout");

class RunTracker {
public:
    static constexpr int64_t kMaxStepSeconds = 120;
    static constexpr int64_t kPostDefrostSeconds = 120;

    // Feed one reading, with the local date it falls on. Returns true when it
    // ended a run; the run is then available from take_finished() until the
    // next add().
    bool add(int64_t now_s, uint32_t date, const Input& in) {
        has_finished_ = false;
        const bool in_step = has_prev_ && now_s >= prev_s_ && now_s - prev_s_ <= kMaxStepSeconds;
        if (open_) {
            if (in_step) {
                book(prev_s_, now_s, prev_);
                if (!in.running || !in.connected) finish();
            } else {
                finish();
            }
        }
        if (has_prev_ && prev_.defrost && !in.defrost) defrost_end_s_ = now_s;
        if (!open_ && in.running && in.connected) start(now_s, date, in);
        prev_ = in;
        prev_s_ = now_s;
        has_prev_ = true;
        return has_finished_;
    }

    // Forget the reference point (e.g. the clock isn't set). An open run ends
    // at its last booked reading on the next add().
    void pause() { has_prev_ = false; }

    bool take_finished(RunRecord* out) const {
        if (!has_finished_) return false;
        *out = finished_;
        return true;
    }

    // The run in progress, through the last reading.
    bool current(RunRecord* out) const {
        if (!open_) return false;
        *out = record();
        out->flags |= RUN_OPEN;
        return true;
    }

private:
    void start(int64_t now_s, uint32_t date, const Input& in) {
        open_ = true;
        date_ = date;
        start_s_ = now_s;
        end_s_ = now_s;
        first_bucket_ = in.bucket;
        elec_ws_ = thermal_ws_ = covered_ws_ = 0;
        outdoor_sum_deci_s_ = 0;
        outdoor_s_ = 0;
        memset(run_s_, 0, sizeof(run_s_));
    }

    void finish() {
        open_ = false;
        // A run with nothing booked (one reading before a gap) isn't a run.
        if (end_s_ <= start_s_) return;
        finished_ = record();
        has_finished_ = true;
    }

    RunRecord record() const {
        RunRecord r;
        memset(&r, 0, sizeof(r));
        r.date = date_;
        r.start_s = (uint32_t)start_s_;
        r.end_s = (uint32_t)end_s_;
        r.elec_wh = ws_to_wh(elec_ws_);
        r.thermal_wh = ws_to_wh(thermal_ws_);
        r.covered_elec_wh = ws_to_wh(covered_ws_);
        r.outdoor_s = outdoor_s_;
        if (outdoor_s_ > 0) r.outdoor_avg_deci = (int16_t)(outdoor_sum_deci_s_ / (int64_t)outdoor_s_);
        r.bucket = first_bucket_ < kModes ? first_bucket_ : (uint8_t)BUCKET_HEATING;
        uint32_t best = 0;
        for (int m = 0; m < kModes; m++) {
            if (run_s_[m] > best) {
                best = run_s_[m];
                r.bucket = (uint8_t)m;
            }
        }
        return r;
    }

    // Same booking rules as EnergyLedger::book().
    void book(int64_t a, int64_t b, const Input& p) {
        end_s_ = b;
        if (!p.connected) return;
        const uint32_t dt = (uint32_t)(b - a);
        const bool mode = p.bucket < kModes;
        if (p.power_valid) elec_ws_ += (uint64_t)p.power_w * dt;
        if (p.running && mode) run_s_[p.bucket] += dt;
        if (p.thermal_valid && p.power_valid && mode) {
            const int32_t q = p.bucket == BUCKET_COOLING ? -p.thermal_w : p.thermal_w;
            if (q > 0) thermal_ws_ += (uint64_t)q * dt;
            covered_ws_ += (uint64_t)p.power_w * dt;
        }
        if (p.outdoor_valid && p.running && mode && !p.defrost) {
            const int64_t from = a > defrost_end_s_ + kPostDefrostSeconds
                                     ? a
                                     : defrost_end_s_ + kPostDefrostSeconds;
            if (b > from) {
                const uint32_t run_dt = (uint32_t)(b - from);
                outdoor_sum_deci_s_ += (int64_t)(p.outdoor_c * 10) * run_dt;
                outdoor_s_ += run_dt;
            }
        }
    }

    bool open_ = false;
    uint32_t date_ = 0;
    int64_t start_s_ = 0;
    int64_t end_s_ = 0;
    uint8_t first_bucket_ = BUCKET_STANDBY;
    uint64_t elec_ws_ = 0;
    uint64_t thermal_ws_ = 0;
    uint64_t covered_ws_ = 0;
    int64_t outdoor_sum_deci_s_ = 0;
    uint32_t outdoor_s_ = 0;
    uint32_t run_s_[kModes] = {};
    Input prev_{};
    int64_t prev_s_ = 0;
    bool has_prev_ = false;
    int64_t defrost_end_s_ = INT64_MIN / 2;
    RunRecord finished_{};
    bool has_finished_ = false;
};

// The run in [starts[i], ends[i]] (padded by `pad` seconds each side, the
// telemetry's sample spacing) that `r` overlaps most, or -1. Each summary goes
// to one run only, so a stop the 30 s samples missed doesn't count it twice.
inline int best_run(const RunRecord& r, const uint32_t* starts, const uint32_t* ends, size_t n,
                    uint32_t pad) {
    int best = -1;
    int64_t best_overlap = 0;
    for (size_t i = 0; i < n; i++) {
        const int64_t lo = (int64_t)starts[i] - pad, hi = (int64_t)ends[i] + pad;
        const int64_t a = r.start_s > lo ? (int64_t)r.start_s : lo;
        const int64_t b = r.end_s < hi ? (int64_t)r.end_s : hi;
        if (b - a > best_overlap) {
            best_overlap = b - a;
            best = (int)i;
        }
    }
    return best;
}

// The summaries attached to one run, combined.
struct RunTotals {
    uint32_t thermal_wh = 0;
    uint32_t covered_elec_wh = 0;
    int64_t outdoor_sum_deci_s = 0;
    uint32_t outdoor_s = 0;
    bool any = false;

    void add(const RunRecord& r) {
        any = true;
        thermal_wh += r.thermal_wh;
        covered_elec_wh += r.covered_elec_wh;
        outdoor_sum_deci_s += (int64_t)r.outdoor_avg_deci * r.outdoor_s;
        outdoor_s += r.outdoor_s;
    }
    // COP x10, or -1 when too little was covered to say.
    int32_t cop_x10() const {
        if (covered_elec_wh < kRunCopMinCoveredWh) return -1;
        return (int32_t)(((uint64_t)thermal_wh * 10 + covered_elec_wh / 2) / covered_elec_wh);
    }
    bool outdoor(int16_t* deci) const {
        if (outdoor_s == 0) return false;
        *deci = (int16_t)(outdoor_sum_deci_s / (int64_t)outdoor_s);
        return true;
    }
};

}  // namespace energy
