// Energy ledger: per-mode electrical/heat integration, day boundaries, clock
// jumps, checkpoints and the telemetry power averager. Pure logic.

#include "energy_ledger.h"

#include <cstdio>

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

static constexpr int64_t MIDNIGHT = 1'800'000'000;  // start of DAY1
static const Day DAY1{20270115, MIDNIGHT, -480};
static const Day DAY2{20270116, MIDNIGHT + 86400, -480};
static const Day DAY3{20270117, MIDNIGHT + 2 * 86400, -480};

static Input standby(uint32_t w = 20) {
    Input in;
    in.connected = true;
    in.power_valid = true;
    in.power_w = w;
    return in;
}

static Input running(Bucket b, uint32_t w, int32_t q, bool q_valid = true) {
    Input in = standby(w);
    in.running = true;
    in.bucket = b;
    in.thermal_valid = q_valid;
    in.thermal_w = q;
    in.source = SOURCE_EXTERNAL;
    in.flow_lpm_x10 = 400;
    in.glycol_pct = 30;
    return in;
}

// Feed `in` every `step` seconds over [t, t + secs].
static int64_t feed(EnergyLedger& l, const Day& d, int64_t t, int64_t secs, const Input& in,
                    int64_t step = 1) {
    for (int64_t s = 0; s <= secs; s += step) l.add(t + s, d, in);
    return t + secs;
}

static void integrates_per_bucket() {
    EnergyLedger l;
    int64_t t = MIDNIGHT + 3600;
    // First reading only sets the reference.
    CHECK_EQ_L(l.add(t, DAY1, standby()), 0);
    CHECK_EQ_L(l.today().elec_wh[BUCKET_STANDBY], 0);

    t = feed(l, DAY1, t, 3600, standby(20));                       // 20 Wh standby
    t = feed(l, DAY1, t, 3600, running(BUCKET_HEATING, 3000, 9000));
    t = feed(l, DAY1, t, 1800, running(BUCKET_HOT_WATER, 4000, 10000));
    t = feed(l, DAY1, t, 1800, running(BUCKET_COOLING, 2000, -6000));
    (void)t;

    const DayRecord r = l.today();
    CHECK_EQ_L(r.date, 20270115);
    CHECK_EQ_L(r.utc_offset_min, -480);
    // Each change is booked from the reading that reported it, so the second
    // that straddles it goes to the previous bucket.
    CHECK_EQ_L(r.elec_wh[BUCKET_STANDBY], 20);
    CHECK_EQ_L(r.elec_wh[BUCKET_HEATING], 3000);
    CHECK_EQ_L(r.elec_wh[BUCKET_HOT_WATER], 2000);
    CHECK_EQ_L(r.elec_wh[BUCKET_COOLING], 1000);
    CHECK_EQ_L(total_elec_wh(r), 6020);
    CHECK_EQ_L(r.thermal_wh[BUCKET_HEATING], 9000);
    CHECK_EQ_L(r.thermal_wh[BUCKET_HOT_WATER], 5000);
    CHECK_EQ_L(r.thermal_wh[BUCKET_COOLING], 3000);  // cooling heat is a magnitude
    CHECK_EQ_L(r.covered_elec_wh[BUCKET_HEATING], 3000);
    CHECK_EQ_L(r.run_s[BUCKET_HEATING], 3600);
    CHECK_EQ_L(r.run_s[BUCKET_HOT_WATER], 1800);
    CHECK_EQ_L(r.run_s[BUCKET_COOLING], 1800);
    CHECK_EQ_L(r.power_s, 10800);
    CHECK_EQ_L(r.thermal_s, 7200);
    CHECK_EQ_L(r.source_s[SOURCE_EXTERNAL], 7200);
    CHECK_EQ_L(r.starts, 1);  // one off->on edge; mode changes while running aren't starts
    CHECK_EQ_L(r.flags & DAY_SETTINGS_SET, DAY_SETTINGS_SET);
    CHECK_EQ_L(r.flags & DAY_MIXED_SETTINGS, 0);
    CHECK_EQ_L(r.flow_lpm_x10, 400);
    CHECK_EQ_L(r.glycol_pct, 30);
}

static void gaps_and_disconnects_are_not_booked() {
    EnergyLedger l;
    int64_t t = MIDNIGHT + 1000;
    t = feed(l, DAY1, t, 100, standby(3600));  // 100 Wh
    CHECK_EQ_L(l.today().elec_wh[BUCKET_STANDBY], 100);

    // A gap longer than kMaxStepSeconds books nothing.
    t += EnergyLedger::kMaxStepSeconds + 1;
    l.add(t, DAY1, standby(3600));
    CHECK_EQ_L(l.today().elec_wh[BUCKET_STANDBY], 100);
    // Exactly kMaxStepSeconds still counts.
    t += EnergyLedger::kMaxStepSeconds;
    l.add(t, DAY1, standby(3600));
    CHECK_EQ_L(l.today().elec_wh[BUCKET_STANDBY], 220);

    // Disconnected time isn't booked, and reconnecting isn't a start.
    Input off = running(BUCKET_HEATING, 3600, 0);
    off.connected = false;
    t = feed(l, DAY1, t, 100, off);
    CHECK_EQ_L(total_elec_wh(l.today()), 220);
    t = feed(l, DAY1, t, 100, running(BUCKET_HEATING, 3600, 0, false));
    CHECK_EQ_L(l.today().starts, 0);
    CHECK_EQ_L(l.today().elec_wh[BUCKET_HEATING], 100);
    CHECK_EQ_L(l.today().thermal_s, 0);  // no valid estimate

    // A pause drops the reference: the next reading only sets it.
    l.pause();
    l.add(t + 60, DAY1, standby(3600));
    CHECK_EQ_L(total_elec_wh(l.today()), 320);

    // Time going backwards within the day resets the reference.
    l.add(t - 500, DAY1, standby(3600));
    l.add(t - 470, DAY1, standby(3600));
    CHECK_EQ_L(total_elec_wh(l.today()), 350);
}

static void splits_at_midnight() {
    EnergyLedger l;
    l.add(DAY2.start_s - 30, DAY1, standby(3600));
    DayRecord fin{};
    CHECK_EQ_L(l.take_finished(&fin), 0);
    // One 60 s interval across midnight: 30 s to each day.
    CHECK_EQ_L(l.add(DAY2.start_s + 30, DAY2, standby(3600)), 1);
    CHECK_EQ_L(l.take_finished(&fin), 1);
    CHECK_EQ_L(fin.date, 20270115);
    CHECK_EQ_L(fin.elec_wh[BUCKET_STANDBY], 30);
    CHECK_EQ_L(l.today().date, 20270116);
    CHECK_EQ_L(l.today().elec_wh[BUCKET_STANDBY], 30);
    // The finished record is only offered until the next reading.
    l.add(DAY2.start_s + 60, DAY2, standby(3600));
    CHECK_EQ_L(l.take_finished(&fin), 0);

    // A gap across midnight closes the day but books nothing.
    l.add(DAY3.start_s + 3600, DAY3, standby(3600));
    CHECK_EQ_L(l.take_finished(&fin), 1);
    CHECK_EQ_L(fin.date, 20270116);
    CHECK_EQ_L(fin.elec_wh[BUCKET_STANDBY], 60);
    CHECK_EQ_L(total_elec_wh(l.today()), 0);
}

static void clock_going_back_never_reopens_a_day() {
    EnergyLedger l;
    feed(l, DAY2, DAY2.start_s + 100, 100, standby(3600));
    CHECK_EQ_L(l.today().elec_wh[BUCKET_STANDBY], 100);
    // Back into yesterday: ignored, nothing booked, no day closed.
    CHECK_EQ_L(l.add(DAY2.start_s - 100, DAY1, standby(3600)), 0);
    l.add(DAY2.start_s - 70, DAY1, standby(3600));
    CHECK_EQ_L(l.today().date, 20270116);
    CHECK_EQ_L(l.today().elec_wh[BUCKET_STANDBY], 100);
    // Caught up: the first reading back only sets the reference.
    l.add(DAY2.start_s + 1000, DAY2, standby(3600));
    l.add(DAY2.start_s + 1030, DAY2, standby(3600));
    CHECK_EQ_L(l.today().elec_wh[BUCKET_STANDBY], 130);
}

static void restore_continues_or_closes() {
    EnergyLedger first;
    feed(first, DAY1, MIDNIGHT + 100, 100, standby(3600));
    const DayAccum saved = first.accum();

    // Same day: carries on; the downtime isn't booked.
    EnergyLedger same;
    same.restore(saved);
    CHECK_EQ_L(same.save_due(0, true), 0);  // a restored checkpoint is already saved
    same.add(MIDNIGHT + 5000, DAY1, standby(3600));
    CHECK_EQ_L(same.today().elec_wh[BUCKET_STANDBY], 100);
    same.add(MIDNIGHT + 5010, DAY1, standby(3600));
    CHECK_EQ_L(same.today().elec_wh[BUCKET_STANDBY], 110);
    // Restoring once tracking has started is ignored.
    same.restore(DayAccum{});
    CHECK_EQ_L(same.today().elec_wh[BUCKET_STANDBY], 110);

    // A checkpoint from an earlier day is closed by the first reading.
    EnergyLedger later;
    later.restore(saved);
    CHECK_EQ_L(later.add(DAY2.start_s + 100, DAY2, standby(3600)), 1);
    DayRecord fin{};
    CHECK_EQ_L(later.take_finished(&fin), 1);
    CHECK_EQ_L(fin.date, 20270115);
    CHECK_EQ_L(fin.elec_wh[BUCKET_STANDBY], 100);
    CHECK_EQ_L(later.today().date, 20270116);
    CHECK_EQ_L(total_elec_wh(later.today()), 0);

    // Legacy energy is reported separately and counts in the total.
    EnergyLedger legacy;
    legacy.add(MIDNIGHT + 10, DAY1, standby());
    legacy.add_unattributed(3600ull * 250);
    CHECK_EQ_L(legacy.today().elec_unattributed_wh, 250);
    CHECK_EQ_L(total_elec_wh(legacy.today()), 250);
}

static void save_schedule() {
    const int64_t I = EnergyLedger::kSaveIntervalSeconds;
    EnergyLedger l;
    CHECK_EQ_L(l.save_due(MIDNIGHT), 0);       // nothing open yet
    l.add(MIDNIGHT + 10, DAY1, standby());
    CHECK_EQ_L(l.save_due(MIDNIGHT + 10), 1);  // never saved: at once
    l.mark_saved(MIDNIGHT + 10);
    CHECK_EQ_L(l.save_due(MIDNIGHT + 10 + I), 0);  // unchanged: no write
    l.add(MIDNIGHT + 20, DAY1, standby());
    CHECK_EQ_L(l.save_due(MIDNIGHT + 20), 0);        // changed, interval not up
    CHECK_EQ_L(l.save_due(MIDNIGHT + 20, true), 1);  // planned restart
    CHECK_EQ_L(l.save_due(MIDNIGHT + 10 + I), 1);
    l.mark_saved(MIDNIGHT + 10 + I);
    // A new day saves at once.
    l.add(DAY2.start_s + 5, DAY2, standby());
    CHECK_EQ_L(l.save_due(DAY2.start_s + 5), 1);
}

static void thermal_only_counts_the_expected_direction() {
    EnergyLedger l;
    int64_t t = MIDNIGHT + 100;
    // Heating that reads as cooling adds no heat but keeps its electricity.
    t = feed(l, DAY1, t, 3600, running(BUCKET_HEATING, 1000, -500));
    t = feed(l, DAY1, t, 3600, running(BUCKET_HEATING, 1000, 3000));
    // A thermal estimate without power isn't covered at all.
    Input no_p = running(BUCKET_HEATING, 0, 3000);
    no_p.power_valid = false;
    t = feed(l, DAY1, t, 3600, no_p);
    (void)t;
    const DayRecord r = l.today();
    CHECK_EQ_L(r.thermal_wh[BUCKET_HEATING], 3000);
    CHECK_EQ_L(r.covered_elec_wh[BUCKET_HEATING], 2000);
    CHECK_EQ_L(r.thermal_s, 7200);
    CHECK_EQ_L(r.power_s, 7200);
    CHECK_EQ_L(r.run_s[BUCKET_HEATING], 3 * 3600);

    // Standby never gets thermal, even with an estimate.
    EnergyLedger s;
    Input idle = standby(50);
    idle.thermal_valid = true;
    idle.thermal_w = 800;
    feed(s, DAY1, MIDNIGHT + 100, 3600, idle);
    CHECK_EQ_L(s.today().thermal_s, 0);
    CHECK_EQ_L(s.today().elec_wh[BUCKET_STANDBY], 50);
}

static void defrost_is_counted() {
    EnergyLedger l;
    int64_t t = MIDNIGHT + 100;
    t = feed(l, DAY1, t, 600, running(BUCKET_HEATING, 3000, 9000));
    Input d = running(BUCKET_HEATING, 2000, 0, false);
    d.defrost = true;
    d.defrost_thermal_valid = true;
    d.defrost_thermal_w = -7200;
    t = feed(l, DAY1, t, 300, d);
    t = feed(l, DAY1, t, 600, running(BUCKET_HEATING, 3000, 9000));
    t = feed(l, DAY1, t, 300, d);
    (void)t;
    const DayRecord r = l.today();
    CHECK_EQ_L(r.defrosts, 2);
    CHECK_EQ_L(r.defrost_s, 600);
    CHECK_EQ_L(r.defrost_thermal_wh, -1200);
    CHECK_EQ_L(r.starts, 0);  // already running at the first reading
    // Defrost electricity stays in the mode it interrupts, but outside the
    // covered (COP) intervals; nor does it count towards any temperature source.
    CHECK_EQ_L(r.run_s[BUCKET_HEATING], 1800);
    CHECK_EQ_L(r.source_s[SOURCE_EXTERNAL], 1200);
    CHECK_EQ_L(r.covered_elec_wh[BUCKET_HEATING], 1000);
}

static void outdoor_and_settings() {
    EnergyLedger l;
    int64_t t = MIDNIGHT + 100;
    Input in = running(BUCKET_HEATING, 1000, 3000);
    in.outdoor_valid = true;
    in.outdoor_c = -5;
    t = feed(l, DAY1, t, 3600, in, 30);
    in.outdoor_c = 3;
    in.flow_lpm_x10 = 350;
    t = feed(l, DAY1, t, 3600, in, 30);
    (void)t;
    const DayRecord r = l.today();
    CHECK_EQ_L(r.flags & DAY_OUTDOOR_SET, DAY_OUTDOOR_SET);
    CHECK_EQ_L(r.outdoor_min_deci, -50);
    CHECK_EQ_L(r.outdoor_max_deci, 30);
    CHECK_EQ_L(r.outdoor_avg_deci, -10);
    CHECK_EQ_L(r.flow_lpm_x10, 400);  // the day's first
    CHECK_EQ_L(r.flags & DAY_MIXED_SETTINGS, DAY_MIXED_SETTINGS);

    EnergyLedger none;
    feed(none, DAY1, MIDNIGHT + 100, 100, standby());
    CHECK_EQ_L(none.today().flags & (DAY_OUTDOOR_SET | DAY_SETTINGS_SET), 0);
    CHECK_EQ_L(none.today().outdoor_avg_deci, 0);
}

static void power_averager() {
    PowerAverager p;
    uint16_t w = 0;
    CHECK_EQ_L(p.take(0, &w), 0);  // nothing yet

    p.add(1000, true, 1000);
    CHECK_EQ_L(p.take(1000, &w), 1);  // a lone reading counts
    CHECK_EQ_L(w, 1000);

    p.add(1000, true, 1000);
    p.add(11000, true, 3000);  // 10 s at 1000 W
    CHECK_EQ_L(p.take(31000, &w), 1);  // + 20 s at 3000 W
    CHECK_EQ_L(w, 2333);

    // Invalid stretches don't count; all-invalid gives nothing.
    p.add(31000, false, 0);
    CHECK_EQ_L(p.take(61000, &w), 0);
    p.add(61000, true, 500);
    p.add(71000, false, 0);
    CHECK_EQ_L(p.take(91000, &w), 1);
    CHECK_EQ_L(w, 500);

    // Saturates below the "no reading" marker.
    PowerAverager big;
    big.add(0, true, 100000);
    CHECK_EQ_L(big.take(30000, &w), 1);
    CHECK_EQ_L(w, 0xFFFE);
}

int main() {
    integrates_per_bucket();
    gaps_and_disconnects_are_not_booked();
    splits_at_midnight();
    clock_going_back_never_reopens_a_day();
    restore_continues_or_closes();
    save_schedule();
    thermal_only_counts_the_expected_direction();
    defrost_is_counted();
    outdoor_and_settings();
    power_averager();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("energy_ledger: all checks passed\n");
    return 0;
}
