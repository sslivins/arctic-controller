// Supply/return/tank direction arrows: 15 s bucket means, least-squares slope,
// hysteresis, hold time, and window resets on gaps.
//
// Readings are modelled the way the unit produces them: about one per second,
// true temperature plus slowly-wandering sensor noise (AR(1), ~3 s memory,
// sigma 0.3 °C), rounded to whole degrees.  That reproduces the A-B-A dither
// seen in the Sun Peaks history.

#include "temp_trend.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <string_view>

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__,      \
                         __LINE__, #cond);                                   \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static constexpr uint32_t STEP = TempTrendTracker::SAMPLE_INTERVAL_MS;

struct Sensor {
    std::mt19937 rng;
    std::normal_distribution<double> innov{0.0, 0.3 * std::sqrt(1 - 0.7 * 0.7)};
    double noise = 0.0;
    explicit Sensor(unsigned seed) : rng(seed) {}
    int16_t read(double true_c) {
        noise = 0.7 * noise + innov(rng);
        return (int16_t)std::lround(true_c + noise);
    }
};

struct Stats {
    int rising = 0, falling = 0, steady = 0, changes = 0;
    uint32_t first_rising_ms = 0, first_falling_ms = 0;
};

// Feed one reading per second for `seconds`; f maps minutes-since-*t0 to the
// true temperature.  Stats count once per second.
static Stats run(TempTrendTracker& tr, Sensor& s, uint32_t* t, int seconds,
                 const std::function<double(double)>& f) {
    Stats st;
    const uint32_t t0 = *t;
    TempTrend prev = tr.trend();
    for (int i = 0; i < seconds; i++) {
        tr.update(*t, s.read(f((*t - t0) / 60000.0)), true);
        TempTrend now = tr.trend();
        if (now == TempTrend::RISING && !st.rising++) st.first_rising_ms = *t - t0;
        if (now == TempTrend::FALLING && !st.falling++) st.first_falling_ms = *t - t0;
        if (now == TempTrend::STEADY) st.steady++;
        if (now != prev) st.changes++;
        prev = now;
        *t += 1000;
    }
    return st;
}

static void test_unknown_until_window_full() {
    TempTrendTracker tr = make_water_trend_tracker();
    Sensor s(1);
    uint32_t t = 1000;
    CHECK(tr.trend() == TempTrend::UNKNOWN);
    run(tr, s, &t, tr.window() * 15 - 10, [](double) { return 40.0; });
    CHECK(tr.trend() == TempTrend::UNKNOWN);
    run(tr, s, &t, 20, [](double) { return 40.0; });
    CHECK(tr.trend() == TempTrend::STEADY);
}

// The worst case: the true temperature sits exactly on a rounding boundary so
// readings are a coin flip between 40 and 41 all day.  No arrow, ever.
static void test_boundary_dither_stays_steady() {
    for (unsigned seed = 1; seed <= 4; seed++) {
        TempTrendTracker water = make_water_trend_tracker();
        TempTrendTracker tank = make_tank_trend_tracker();
        Sensor s(seed), s2(seed + 100);
        uint32_t t = 0, t2 = 0;
        run(water, s, &t, 20 * 60, [](double) { return 40.5; });
        run(tank, s2, &t2, 20 * 60, [](double) { return 40.5; });
        Stats w = run(water, s, &t, 12 * 3600, [](double) { return 40.5; });
        Stats k = run(tank, s2, &t2, 12 * 3600, [](double) { return 40.5; });
        CHECK(w.changes == 0);
        CHECK(k.changes == 0);
        if (w.changes || k.changes)
            std::fprintf(stderr, "  seed %u: water %d changes, tank %d changes\n", seed,
                         w.changes, k.changes);
    }
}

// Supply during a run: +0.2 °C/min.  The arrow comes on within a few minutes
// and then stays on, despite the readings only stepping every ~5 min.
static void test_heating_ramp() {
    TempTrendTracker tr = make_water_trend_tracker();
    Sensor s(7);
    uint32_t t = 0;
    run(tr, s, &t, 10 * 60, [](double) { return 30.3; });
    CHECK(tr.trend() == TempTrend::STEADY);
    Stats st = run(tr, s, &t, 30 * 60, [](double m) { return 30.3 + 0.2 * m; });
    CHECK(tr.trend() == TempTrend::RISING);
    CHECK(st.falling == 0);
    CHECK(st.changes == 1);
    CHECK(st.rising > 0 && st.first_rising_ms <= 5 * 60000);
}

static void test_cooling_ramp() {
    TempTrendTracker tr = make_water_trend_tracker();
    Sensor s(8);
    uint32_t t = 0;
    run(tr, s, &t, 10 * 60, [](double) { return 45.2; });
    Stats st = run(tr, s, &t, 30 * 60, [](double m) { return 45.2 - 0.25 * m; });
    CHECK(tr.trend() == TempTrend::FALLING);
    CHECK(st.rising == 0);
    CHECK(st.changes == 1);
}

// When a run ends the arrow returns to steady without swinging the other way.
static void test_ramp_then_plateau() {
    TempTrendTracker tr = make_water_trend_tracker();
    Sensor s(9);
    uint32_t t = 0;
    run(tr, s, &t, 10 * 60, [](double) { return 30.0; });
    run(tr, s, &t, 20 * 60, [](double m) { return 30.0 + 0.3 * m; });
    CHECK(tr.trend() == TempTrend::RISING);
    Stats st = run(tr, s, &t, 30 * 60, [](double) { return 36.0; });
    CHECK(tr.trend() == TempTrend::STEADY);
    CHECK(st.falling == 0);
    CHECK(st.changes == 1);
}

// The tank warms ~0.08 °C/min: under the supply/return threshold, but enough
// for the tank's longer, more sensitive window.
static void test_tank_slow_ramp() {
    TempTrendTracker tank = make_tank_trend_tracker();
    Sensor s(10);
    uint32_t t = 0;
    run(tank, s, &t, 20 * 60, [](double) { return 45.4; });
    CHECK(tank.trend() == TempTrend::STEADY);
    Stats st = run(tank, s, &t, 40 * 60, [](double m) { return 45.4 + 0.08 * m; });
    CHECK(tank.trend() == TempTrend::RISING);
    CHECK(st.falling == 0);
    CHECK(st.changes == 1);
}

// Readings every 15 s still work: one per bucket.
static void test_sparse_readings() {
    TempTrendTracker tr = make_water_trend_tracker();
    uint32_t t = 0;
    for (int i = 0; i <= tr.window(); i++, t += STEP) tr.update(t, 40, true);
    CHECK(tr.trend() == TempTrend::STEADY);
    for (int i = 0; i < 40; i++, t += STEP) tr.update(t, (int16_t)(40 + i / 4), true);
    CHECK(tr.trend() == TempTrend::RISING);
}

static void test_gap_and_invalid_reset() {
    TempTrendTracker tr = make_water_trend_tracker();
    Sensor s(11);
    uint32_t t = 0;
    run(tr, s, &t, tr.window() * 15 + 30, [](double) { return 40.0; });
    CHECK(tr.trend() == TempTrend::STEADY);
    t += TempTrendTracker::MAX_GAP_MS + 1000;
    tr.update(t, 40, true);
    CHECK(tr.trend() == TempTrend::UNKNOWN);

    run(tr, s, &t, tr.window() * 15 + 30, [](double) { return 40.0; });
    CHECK(tr.trend() == TempTrend::STEADY);
    tr.update(t, 0, false);
    CHECK(tr.trend() == TempTrend::UNKNOWN);
}

// Even a violent swing can't change the arrow more than once a minute.
static void test_min_hold() {
    TempTrendTracker tr = make_water_trend_tracker();
    uint32_t t = 0;
    for (int i = 0; i < tr.window() * 15 + 30; i++, t += 1000) tr.update(t, 30, true);
    int changes = 0;
    TempTrend prev = tr.trend();
    uint32_t last_change = 0;
    for (int i = 0; i < 3600; i++, t += 1000) {
        tr.update(t, (i / 90) % 2 ? 40 : 30, true);  // 90 s half-period
        if (tr.trend() != prev) {
            if (changes) CHECK(t - last_change >= TempTrendTracker::MIN_HOLD_SAMPLES * STEP);
            last_change = t;
            changes++;
            prev = tr.trend();
        }
    }
    CHECK(changes > 0);
}

static void test_wraparound_clock() {
    TempTrendTracker tr = make_water_trend_tracker();
    uint32_t t = 0xFFFFFFFFu - 60000;
    for (int i = 0; i < tr.window() * 15 + 30; i++, t += 1000) tr.update(t, 40, true);
    CHECK(tr.trend() == TempTrend::STEADY);
}

static void test_strings() {
    CHECK(std::string_view(temp_trend_to_string(TempTrend::RISING)) == "rising");
    CHECK(std::string_view(temp_trend_to_string(TempTrend::FALLING)) == "falling");
    CHECK(std::string_view(temp_trend_to_string(TempTrend::STEADY)) == "steady");
    CHECK(std::string_view(temp_trend_to_string(TempTrend::UNKNOWN)) == "unknown");
}

int main() {
    test_unknown_until_window_full();
    test_boundary_dither_stays_steady();
    test_heating_ramp();
    test_cooling_ramp();
    test_ramp_then_plateau();
    test_tank_slow_ramp();
    test_sparse_readings();
    test_gap_and_invalid_reset();
    test_min_hold();
    test_wraparound_clock();
    test_strings();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("temp_trend: all tests passed\n");
    return 0;
}
