// Home screen stats: compressor runs and starts per hour from telemetry
// samples and the tank trend series.

#include "home_stats.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK_EQ_L(actual, expected)                                         \
    do {                                                                     \
        long a_ = (long)(actual);                                            \
        long e_ = (long)(expected);                                          \
        if (a_ != e_) {                                                      \
            std::fprintf(stderr, "%s:%d: %s == %ld, expected %ld\n",         \
                         __FILE__, __LINE__, #actual, a_, e_);               \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static constexpr uint32_t T0 = 1'800'000'000;
static constexpr uint32_t STEP = HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC;

// One sample per character from T0: 'H' heating run, 'C' cooling run,
// 'U' compressor running with an unknown mode, '.' stopped, ' ' no sample.
static std::vector<history_telemetry_sample_t> timeline(const char* s,
                                                        uint32_t t0 = T0) {
    std::vector<history_telemetry_sample_t> v;
    for (uint32_t i = 0; s[i]; i++) {
        if (s[i] == ' ') continue;
        history_telemetry_sample_t x = {};
        x.timestamp = t0 + i * STEP;
        x.flags = HISTORY_TELEMETRY_COMPRESSOR_VALID;
        if (s[i] == 'H' || s[i] == 'C' || s[i] == 'U') {
            x.flags |= HISTORY_TELEMETRY_COMPRESSOR_RUNNING;
            x.mode = s[i] == 'H'   ? HISTORY_TELEMETRY_MODE_HEATING
                     : s[i] == 'C' ? HISTORY_TELEMETRY_MODE_COOLING
                                   : HISTORY_TELEMETRY_MODE_UNKNOWN;
        }
        v.push_back(x);
    }
    return v;
}

static size_t runs(const std::vector<history_telemetry_sample_t>& v,
                   uint32_t now, home_run_t* out, size_t cap,
                   home_run_summary_t* sum) {
    return home_stats_runs(v.data(), v.size(), now, out, cap, sum);
}

static void drop_runs_before_keeps_overlapping_and_current() {
    // Finished before, ending exactly at, overlapping, inside, and current.
    home_run_t r[5] = {{100, 150, 1, true}, {160, 200, 1, true}, {180, 260, 2, true},
                       {300, 400, 1, true}, {500, 0, 1, true}};
    const size_t kept = home_stats_drop_runs_before(r, 5, 200);
    CHECK_EQ_L(kept, 3);
    CHECK_EQ_L(r[0].start, 180);
    CHECK_EQ_L(r[0].mode, 2);
    CHECK_EQ_L(r[1].start, 300);
    CHECK_EQ_L(r[2].start, 500);
    CHECK_EQ_L(r[2].end, 0);
}

static void finished_and_current_runs() {
    auto v = timeline("..HHH...HH");
    home_run_t out[8];
    home_run_summary_t sum;
    const uint32_t now = T0 + 9 * STEP + 5;
    CHECK_EQ_L(runs(v, now, out, 8, &sum), 2);
    CHECK_EQ_L(out[0].start, T0 + 2 * STEP);
    CHECK_EQ_L(out[0].end, T0 + 5 * STEP);
    CHECK_EQ_L(out[0].mode, HISTORY_TELEMETRY_MODE_HEATING);
    CHECK_EQ_L(sum.last.start, T0 + 2 * STEP);
    CHECK_EQ_L(sum.current.start, T0 + 8 * STEP);
    CHECK_EQ_L(sum.current.end, 0);
    CHECK_EQ_L(sum.starts_last_hour, 2);
}

static void first_sample_running_is_not_a_start() {
    auto v = timeline("HHH..");
    home_run_summary_t sum;
    home_run_t out[4];
    CHECK_EQ_L(runs(v, T0 + 5 * STEP, out, 4, &sum), 1);
    CHECK_EQ_L(sum.starts_last_hour, 0);
    CHECK_EQ_L(sum.current.start, 0);
    CHECK_EQ_L(sum.last.end, T0 + 3 * STEP);
}

static void gap_ends_run_and_hides_start() {
    // Running, then 5 missing samples, then running again: the outage closes
    // the first run, and the second one's start wasn't observed.
    auto v = timeline(".HH     HH");
    home_run_t out[4];
    home_run_summary_t sum;
    CHECK_EQ_L(runs(v, T0 + 9 * STEP, out, 4, &sum), 2);
    CHECK_EQ_L(out[0].end, T0 + 3 * STEP);
    CHECK_EQ_L(out[1].start, T0 + 8 * STEP);
    CHECK_EQ_L(sum.starts_last_hour, 1);
}

static void stale_open_run_is_not_current() {
    auto v = timeline("..HH");
    home_run_summary_t sum;
    home_run_t out[4];
    runs(v, T0 + 3 * STEP + 3600, out, 4, &sum);
    CHECK_EQ_L(sum.current.start, 0);
    CHECK_EQ_L(sum.last.start, T0 + 2 * STEP);
    CHECK_EQ_L(sum.last.end, T0 + 4 * STEP);
}

static void starts_only_count_the_last_hour() {
    // Short cycling: 1 on, 1 off for 3 hours = 180 starts in total.
    std::string s;
    for (int i = 0; i < 180; i++) s += "H.";
    auto v = timeline(s.c_str());
    home_run_t out[4];
    home_run_summary_t sum;
    const uint32_t now = T0 + 360 * STEP;
    CHECK_EQ_L(runs(v, now, out, 4, &sum), 4);
    // Starts at even sample indices in [now - 3600, now): 60 of them, minus
    // the first sample's run which isn't one (not in range anyway).
    CHECK_EQ_L(sum.starts_last_hour, 60);
    CHECK_EQ_L(out[3].start, T0 + 358 * STEP);
}

static void setpoint_and_mode() {
    auto v = timeline(".CC.");
    v[1].flags |= HISTORY_TELEMETRY_SETPOINT_VALID;
    v[1].setpoint_deci_c = 120;
    home_run_t out[2];
    home_run_summary_t sum;
    runs(v, T0 + 4 * STEP, out, 2, &sum);
    CHECK_EQ_L(sum.setpoint_valid, 1);
    CHECK_EQ_L(sum.setpoint_deci_c, 120);
    CHECK_EQ_L(sum.last.mode, HISTORY_TELEMETRY_MODE_COOLING);
}

static void unknown_mode_is_not_a_run() {
    // Matches the cycle-history chart, which ignores unknown-mode samples.
    auto v = timeline("UUU.UUHH.");
    home_run_t out[4];
    home_run_summary_t sum;
    CHECK_EQ_L(runs(v, T0 + 9 * STEP, out, 4, &sum), 1);
    CHECK_EQ_L(out[0].start, T0 + 6 * STEP);
    CHECK_EQ_L(out[0].mode, HISTORY_TELEMETRY_MODE_HEATING);
}

static void tank_series_averages_buckets() {
    auto v = timeline("........");
    for (size_t i = 0; i < v.size(); i++) {
        if (i == 5) continue;  // no tank reading
        v[i].flags |= HISTORY_TELEMETRY_TANK_VALID;
        v[i].tank_deci_c = (int16_t)(400 + i * 10);
    }
    int16_t t[4];
    bool ok[4];
    home_stats_tank_series(v.data(), v.size(), T0, T0 + 8 * STEP, t, ok, 4);
    CHECK_EQ_L(ok[0], 1);
    CHECK_EQ_L(t[0], 405);
    CHECK_EQ_L(t[2], 440);  // sample 4 only
    CHECK_EQ_L(t[3], 465);

    // Outside the window, or no readings at all.
    home_stats_tank_series(v.data(), v.size(), T0 + 1000, T0 + 2000, t, ok, 4);
    CHECK_EQ_L(ok[0] || ok[1] || ok[2] || ok[3], 0);
}

static void older_run_found_when_window_has_none() {
    // Idle for the whole window; the last run sits in the older samples.
    auto older = timeline("..HHH....");
    auto window = timeline("....", T0 + 100 * STEP);
    home_run_summary_t sum;
    runs(window, T0 + 104 * STEP, nullptr, 0, &sum);
    CHECK_EQ_L(home_stats_needs_older(&sum, window.data(), window.size()), 1);
    home_stats_merge_older(&sum, window.data(), window.size(), older.data(), older.size());
    CHECK_EQ_L(sum.last.start, T0 + 2 * STEP);
    CHECK_EQ_L(sum.last.end, T0 + 5 * STEP);
    CHECK_EQ_L(sum.last.mode, HISTORY_TELEMETRY_MODE_HEATING);

    // Nothing older either.
    auto idle = timeline("......");
    runs(window, T0 + 104 * STEP, nullptr, 0, &sum);
    home_stats_merge_older(&sum, window.data(), window.size(), idle.data(), idle.size());
    CHECK_EQ_L(sum.last.start, 0);
}

static void run_cut_off_at_window_start_gets_real_start() {
    // A cooling run started before the window and stopped inside it.
    auto older = timeline("..CCCC");
    auto window = timeline("CC...", T0 + 6 * STEP);
    home_run_summary_t sum;
    runs(window, T0 + 11 * STEP, nullptr, 0, &sum);
    CHECK_EQ_L(sum.last.start, T0 + 6 * STEP);
    CHECK_EQ_L(home_stats_needs_older(&sum, window.data(), window.size()), 1);
    home_stats_merge_older(&sum, window.data(), window.size(), older.data(), older.size());
    CHECK_EQ_L(sum.last.start, T0 + 2 * STEP);
    CHECK_EQ_L(sum.last.end, T0 + 8 * STEP);
    CHECK_EQ_L(sum.last.mode, HISTORY_TELEMETRY_MODE_COOLING);

    // After an outage at the window start it really did start there.
    window = timeline("CC...", T0 + 20 * STEP);
    runs(window, T0 + 25 * STEP, nullptr, 0, &sum);
    home_stats_merge_older(&sum, window.data(), window.size(), older.data(), older.size());
    CHECK_EQ_L(sum.last.start, T0 + 20 * STEP);
}

static void run_ending_at_window_start_ends_at_first_sample() {
    auto older = timeline("..HHH");
    // One sample went missing at the boundary; still the same stretch.
    auto window = timeline("....", T0 + 6 * STEP);
    home_run_summary_t sum;
    runs(window, T0 + 10 * STEP, nullptr, 0, &sum);
    home_stats_merge_older(&sum, window.data(), window.size(), older.data(), older.size());
    CHECK_EQ_L(sum.last.start, T0 + 2 * STEP);
    CHECK_EQ_L(sum.last.end, T0 + 6 * STEP);
}

static void window_with_its_own_run_needs_nothing_older() {
    auto window = timeline("..HH..");
    home_run_summary_t sum;
    runs(window, T0 + 6 * STEP, nullptr, 0, &sum);
    CHECK_EQ_L(home_stats_needs_older(&sum, window.data(), window.size()), 0);
    // Nor does a run in progress.
    window = timeline("...HH");
    runs(window, T0 + 5 * STEP, nullptr, 0, &sum);
    CHECK_EQ_L(home_stats_needs_older(&sum, window.data(), window.size()), 0);
}

int main() {
    finished_and_current_runs();
    drop_runs_before_keeps_overlapping_and_current();
    first_sample_running_is_not_a_start();
    gap_ends_run_and_hides_start();
    stale_open_run_is_not_current();
    starts_only_count_the_last_hour();
    setpoint_and_mode();
    unknown_mode_is_not_a_run();
    tank_series_averages_buckets();
    older_run_found_when_window_has_none();
    run_cut_off_at_window_start_gets_real_start();
    run_ending_at_window_start_ends_at_first_sample();
    window_with_its_own_run_needs_nothing_older();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("home_stats: all checks passed\n");
    return 0;
}
