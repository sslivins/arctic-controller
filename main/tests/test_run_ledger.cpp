// Per-run energy summaries: the live tracker, attaching summaries to the
// telemetry runs, and the NVS day store over the NVS fake.

#include "run_ledger.h"
#include "run_store.h"
#include "nvs_fake.h"

#include <cstdio>
#include <vector>

using namespace energy;

static int g_failures = 0;

#define CHECK_EQ_L(actual, expected)                                         \
    do {                                                                     \
        long long a_ = (long long)(actual);                                  \
        long long e_ = (long long)(expected);                                \
        if (a_ != e_) {                                                      \
            std::fprintf(stderr, "%s:%d: %s == %lld, expected %lld\n",       \
                         __FILE__, __LINE__, #actual, a_, e_);               \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static constexpr uint32_t DATE = 20270115;
static constexpr int64_t T0 = 1800000000;

static Input running(int32_t thermal_w, uint32_t power_w, int16_t outdoor_c) {
    Input in;
    in.connected = true;
    in.running = true;
    in.bucket = BUCKET_HEATING;
    in.power_valid = true;
    in.power_w = power_w;
    in.thermal_valid = true;
    in.thermal_w = thermal_w;
    in.outdoor_valid = true;
    in.outdoor_c = outdoor_c;
    return in;
}

static Input stopped() {
    Input in;
    in.connected = true;
    in.power_valid = true;
    in.power_w = 20;
    return in;
}

// Feed `secs` one-second readings of `in` from t; returns the next t.
static int64_t feed(RunTracker& r, int64_t t, int64_t secs, const Input& in) {
    for (int64_t i = 0; i < secs; i++, t++) r.add(t, DATE, in);
    return t;
}

static void run_cop_and_outdoor() {
    RunTracker r;
    RunRecord rec{};
    int64_t t = feed(r, T0, 10, stopped());
    // 180 s settling with no estimate, then an hour at 6 kW heat for 2 kW.
    Input settling = running(0, 2000, -5);
    settling.thermal_valid = false;
    t = feed(r, t, 180, settling);
    t = feed(r, t, 3600, running(6000, 2000, -5));
    CHECK_EQ_L(r.current(&rec), 1);
    CHECK_EQ_L(rec.flags & RUN_OPEN, RUN_OPEN);
    CHECK_EQ_L(rec.start_s, T0 + 10);
    CHECK_EQ_L(r.add(t, DATE, stopped()), 1);
    CHECK_EQ_L(r.take_finished(&rec), 1);
    CHECK_EQ_L(rec.date, DATE);
    CHECK_EQ_L(rec.start_s, T0 + 10);
    CHECK_EQ_L(rec.end_s, t);
    CHECK_EQ_L(rec.flags, 0);
    CHECK_EQ_L(rec.bucket, BUCKET_HEATING);
    CHECK_EQ_L(rec.elec_wh, 2100);  // 3780 s at 2 kW
    CHECK_EQ_L(rec.thermal_wh, 6000);
    CHECK_EQ_L(rec.covered_elec_wh, 2000);
    CHECK_EQ_L(rec.outdoor_avg_deci, -50);
    CHECK_EQ_L(rec.outdoor_s, 3780);
    CHECK_EQ_L(r.current(&rec), 0);

    RunTotals tot;
    tot.add(rec);
    CHECK_EQ_L(tot.cop_x10(), 30);
    int16_t deci = 0;
    CHECK_EQ_L(tot.outdoor(&deci), 1);
    CHECK_EQ_L(deci, -50);
}

static void defrost_left_out_of_outdoor() {
    RunTracker r;
    RunRecord rec{};
    int64_t t = feed(r, T0, 600, running(6000, 2000, -10));
    Input df = running(-2000, 1500, 5);
    df.defrost = true;
    t = feed(r, t, 300, df);
    // Still warm from the defrost: left out of the average.
    t = feed(r, t, RunTracker::kPostDefrostSeconds, running(6000, 2000, 8));
    t = feed(r, t, 600, running(6000, 2000, -10));
    r.add(t, DATE, stopped());
    CHECK_EQ_L(r.take_finished(&rec), 1);
    CHECK_EQ_L(rec.outdoor_avg_deci, -100);
    CHECK_EQ_L(rec.outdoor_s, 1200);
    // Defrost heat runs backwards: nothing booked, its electricity still counts.
    CHECK_EQ_L(rec.covered_elec_wh, ws_to_wh(1320ull * 2000 + 300ull * 1500));
}

static void gap_disconnect_and_cooling() {
    RunTracker r;
    RunRecord rec{};
    int64_t t = feed(r, T0, 300, running(6000, 2000, 0));
    // A gap ends the run at its last reading; the next reading starts another.
    t += RunTracker::kMaxStepSeconds + 1;
    CHECK_EQ_L(r.add(t, DATE, running(6000, 2000, 0)), 1);
    CHECK_EQ_L(r.take_finished(&rec), 1);
    CHECK_EQ_L(rec.end_s, T0 + 299);
    CHECK_EQ_L(r.current(&rec), 1);
    CHECK_EQ_L(rec.start_s, t);

    Input off = running(6000, 2000, 0);
    off.connected = false;
    t = feed(r, t + 1, 60, running(6000, 2000, 0));
    CHECK_EQ_L(r.add(t, DATE, off), 1);
    CHECK_EQ_L(r.current(&rec), 0);

    // Cooling counts heat removed, and a run of one reading isn't a run.
    RunTracker c;
    Input cool = running(-5000, 1000, 30);
    cool.bucket = BUCKET_COOLING;
    t = feed(c, T0, 3600, cool);
    c.add(t, DATE, stopped());
    CHECK_EQ_L(c.take_finished(&rec), 1);
    CHECK_EQ_L(rec.bucket, BUCKET_COOLING);
    CHECK_EQ_L(rec.thermal_wh, 5000);

    RunTracker one;
    one.add(T0, DATE, running(6000, 2000, 0));
    CHECK_EQ_L(one.add(T0 + 500, DATE, stopped()), 0);
}

static void attach_to_telemetry_runs() {
    // Two telemetry runs; the tracker saw the first as two pieces (a stop the
    // 30 s samples missed) and the second straddles its padded edge.
    const uint32_t starts[] = {1000, 5000};
    const uint32_t ends[] = {2000, 6000};
    RunRecord a{}, b{}, c{}, far{};
    a.start_s = 990; a.end_s = 1500; a.thermal_wh = 300; a.covered_elec_wh = 100;
    a.outdoor_s = 500; a.outdoor_avg_deci = -20;
    b.start_s = 1510; b.end_s = 2010; b.thermal_wh = 300; b.covered_elec_wh = 100;
    b.outdoor_s = 500; b.outdoor_avg_deci = -40;
    c.start_s = 4980; c.end_s = 6000;
    far.start_s = 3000; far.end_s = 3500;
    CHECK_EQ_L(best_run(a, starts, ends, 2, 30), 0);
    CHECK_EQ_L(best_run(b, starts, ends, 2, 30), 0);
    CHECK_EQ_L(best_run(c, starts, ends, 2, 30), 1);
    CHECK_EQ_L(best_run(far, starts, ends, 2, 30), -1);

    RunTotals tot;
    tot.add(a);
    tot.add(b);
    CHECK_EQ_L(tot.cop_x10(), 30);
    int16_t deci = 0;
    CHECK_EQ_L(tot.outdoor(&deci), 1);
    CHECK_EQ_L(deci, -30);

    // Too little covered to say, and no outdoor time at all.
    RunTotals small;
    RunRecord s{};
    s.covered_elec_wh = kRunCopMinCoveredWh - 1;
    s.thermal_wh = 300;
    small.add(s);
    CHECK_EQ_L(small.cop_x10(), -1);
    CHECK_EQ_L(small.outdoor(&deci), 0);
}

static void dates() {
    CHECK_EQ_L(date_add_days(20270101, -1), 20261231);
    CHECK_EQ_L(date_add_days(20280228, 1), 20280229);
    CHECK_EQ_L(date_add_days(20270301, -1), 20270228);
    CHECK_EQ_L(date_from_days(days_from_date(19700101)), 19700101);
}

static RunRecord stored(uint32_t date, uint32_t start, uint32_t heat = 100) {
    RunRecord r{};
    r.date = date;
    r.start_s = start;
    r.end_s = start + 600;
    r.thermal_wh = heat;
    return r;
}

static void store_round_trip_order_and_prune() {
    nvs_fake::reset();
    RunRecord out[kMaxRunsPerDay + 4];
    CHECK_EQ_L(store_read_runs(DATE, out, 4), 0);

    CHECK_EQ_L(store_append_run(stored(DATE, 3000)), 1);
    CHECK_EQ_L(store_append_run(stored(DATE, 1000)), 1);
    RunRecord open = stored(DATE, 2000);
    open.flags = RUN_OPEN;
    CHECK_EQ_L(store_append_run(open), 1);
    // The same run again replaces it.
    CHECK_EQ_L(store_append_run(stored(DATE, 1000, 555)), 1);
    CHECK_EQ_L(store_read_runs(DATE, out, 8), 3);
    CHECK_EQ_L(out[0].start_s, 1000);
    CHECK_EQ_L(out[0].thermal_wh, 555);
    CHECK_EQ_L(out[1].start_s, 2000);
    CHECK_EQ_L(out[1].flags, 0);
    CHECK_EQ_L(out[2].start_s, 3000);
    CHECK_EQ_L(store_read_runs(DATE, out, 2), 2);

    // A flipped bit fails the CRC.
    std::vector<uint8_t> blob;
    CHECK_EQ_L(nvs_fake::peek_blob("runs", "d20270115", &blob), 1);
    blob[30] ^= 1;
    nvs_fake::seed_blob("runs", "d20270115", blob);
    CHECK_EQ_L(store_read_runs(DATE, out, 8), 0);

    // A full day keeps what it has.
    nvs_fake::reset();
    for (uint32_t i = 0; i < kMaxRunsPerDay + 2; i++) {
        CHECK_EQ_L(store_append_run(stored(DATE, 1000 + i * 700)), 1);
    }
    CHECK_EQ_L(store_read_runs(DATE, out, kMaxRunsPerDay + 4), kMaxRunsPerDay);

    // A new day erases the days that aged out, even across a gap.
    nvs_fake::reset();
    const uint32_t old_day = date_add_days(DATE, -40);
    const uint32_t kept = date_add_days(DATE, -(kRunDaysKept - 1));
    const uint32_t aged = date_add_days(DATE, -kRunDaysKept);
    CHECK_EQ_L(store_append_run(stored(old_day, 10)), 1);
    CHECK_EQ_L(store_append_run(stored(kept, 10)), 1);
    CHECK_EQ_L(store_append_run(stored(aged, 10)), 1);
    CHECK_EQ_L(store_append_run(stored(DATE, 10)), 1);
    CHECK_EQ_L(store_read_runs(old_day, out, 4), 0);
    CHECK_EQ_L(store_read_runs(aged, out, 4), 0);
    CHECK_EQ_L(store_read_runs(kept, out, 4), 1);
    CHECK_EQ_L(store_read_runs(DATE, out, 4), 1);
}

int main() {
    run_cop_and_outdoor();
    defrost_left_out_of_outdoor();
    gap_disconnect_and_cooling();
    attach_to_telemetry_runs();
    dates();
    store_round_trip_order_and_prune();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("run_ledger: all passed\n");
    return 0;
}
