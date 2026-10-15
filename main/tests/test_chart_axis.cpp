// Host tests for chart_axis: cycle-history y-axis gridlines on round degrees.

#include <cstdio>
#include <cstdlib>

#include "chart_axis.h"

static int g_failures = 0;

#define CHECK(cond)                                                   \
    do {                                                              \
        if (!(cond)) {                                                \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, \
                         __LINE__, #cond);                            \
            g_failures++;                                             \
        }                                                             \
    } while (0)

static void check_axis(int32_t lo, int32_t hi, int32_t want_lo, int32_t want_hi,
                       int32_t want_step) {
    const ChartAxis a = chart_axis_nice(lo, hi);
    if (a.lo != want_lo || a.hi != want_hi || a.step != want_step) {
        std::fprintf(stderr, "nice(%d, %d) = {%d, %d, %d}, want {%d, %d, %d}\n",
                     (int)lo, (int)hi, (int)a.lo, (int)a.hi, (int)a.step,
                     (int)want_lo, (int)want_hi, (int)want_step);
        g_failures++;
    }
}

// The range from the bug report: labels 4.6°, 14.4°, 24.1°... become 0°,
// 10°, 20°...
static void test_reported_range() { check_axis(46, 506, 0, 600, 100); }

static void test_small_ranges_use_small_steps() {
    check_axis(305, 335, 300, 340, 10);   // 30.5-33.5 -> 30..34 by 1
    check_axis(250, 350, 240, 360, 20);   // 10° span -> 2° steps
}

static void test_bounds_are_widened_not_clipped() {
    for (int32_t lo = -300; lo <= 600; lo += 37) {
        for (int32_t span = 1; span <= 900; span += 53) {
            const ChartAxis a = chart_axis_nice(lo, lo + span);
            CHECK(a.lo <= lo);
            CHECK(a.hi >= lo + span);
            CHECK(a.lo % a.step == 0);
            CHECK(a.hi % a.step == 0);
            CHECK(a.step % 10 == 0);  // whole degrees
            CHECK((a.hi - a.lo) / a.step <= 6);
            CHECK(a.hi > a.lo);
        }
    }
}

static void test_negative_temperatures() { check_axis(-73, 47, -100, 50, 50); }

static void test_fahrenheit_heating_range() {
    // 40-122 °F in tenths.
    check_axis(400, 1220, 400, 1400, 200);
}

static void test_flat_and_reversed_input() {
    check_axis(300, 300, 300, 310, 10);
    check_axis(500, 100, 100, 500, 100);
}

int main() {
    test_reported_range();
    test_small_ranges_use_small_steps();
    test_bounds_are_widened_not_clipped();
    test_negative_temperatures();
    test_fahrenheit_heating_range();
    test_flat_and_reversed_input();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::puts("chart_axis: all tests passed");
    return EXIT_SUCCESS;
}
