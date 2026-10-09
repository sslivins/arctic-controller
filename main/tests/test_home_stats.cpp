// Home screen stats: compressor runs and starts per hour from telemetry
// samples, the tank trend series, and today's energy integration.

#include "daily_energy.h"
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

static void daily_energy_integrates_and_resets() {
    DailyEnergy e;
    CHECK_EQ_L(e.valid(), 0);
    e.add(1000, 1, 3600);  // first reading only sets the reference
    CHECK_EQ_L(e.valid(), 1);
    CHECK_EQ_L(e.wh(), 0);
    for (int i = 1; i <= 120; i++) e.add(1000 + i * 30, 1, 3600);  // 1 h at 3.6 kW
    CHECK_EQ_L(e.wh(), 3600);

    e.add(1000 + 120 * 30 + 600, 1, 9999);  // a 10 min gap isn't integrated
    CHECK_EQ_L(e.wh(), 3600);
    e.add(1000 + 120 * 30 + 630, 1, 1200);
    CHECK_EQ_L(e.wh(), 3610);

    e.pause();
    e.add(50000, 1, 5000);
    CHECK_EQ_L(e.wh(), 3610);

    e.add(50030, 2, 5000);  // new day
    CHECK_EQ_L(e.wh(), 0);
    e.add(50060, 2, 7200);
    CHECK_EQ_L(e.wh(), 60);
}

int main() {
    finished_and_current_runs();
    first_sample_running_is_not_a_start();
    gap_ends_run_and_hides_start();
    stale_open_run_is_not_current();
    starts_only_count_the_last_hour();
    setpoint_and_mode();
    unknown_mode_is_not_a_run();
    tank_series_averages_buckets();
    daily_energy_integrates_and_resets();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("home_stats: all checks passed\n");
    return 0;
}
