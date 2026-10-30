#include <stdio.h>
#include <string.h>

#include "tschart_logic.h"

using namespace tschart;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        g_checks++;                                                          \
        long long _a = (long long)(a), _b = (long long)(b);                  \
        if (_a != _b) {                                                      \
            g_failures++;                                                    \
            printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__,        \
                   __LINE__, #a, #b, _a, _b);                                \
        }                                                                    \
    } while (0)

static void test_nice_axis() {
    // Temps in deci-degrees, 1 degree minimum step.
    Axis a = nice_axis(183, 471, 6, 10);   // 5 deg would need 7 intervals
    CHECK_EQ(a.step, 100);
    CHECK_EQ(a.lo, 100);
    CHECK_EQ(a.hi, 500);
    a = nice_axis(203, 261, 6, 10);   // 1 deg would need 7 intervals
    CHECK_EQ(a.step, 20);
    CHECK_EQ(a.lo, 200);
    CHECK_EQ(a.hi, 280);
    a = nice_axis(183, 351, 6, 10);
    CHECK_EQ(a.step, 50);
    CHECK_EQ(a.lo, 150);
    CHECK_EQ(a.hi, 400);
    // Negative ranges round outward.
    a = nice_axis(-143, -12, 6, 10);
    CHECK_EQ(a.step, 50);
    CHECK_EQ(a.lo, -150);
    CHECK_EQ(a.hi, 0);
    // Never finer than min_step.
    a = nice_axis(201, 203, 6, 10);
    CHECK_EQ(a.step, 10);
    CHECK_EQ(a.lo, 200);
    CHECK_EQ(a.hi, 210);
    // Power in W, 1 kW minimum step.
    a = nice_axis(0, 3150, 5, 1000);
    CHECK_EQ(a.step, 1000);
    CHECK_EQ(a.hi, 4000);
    a = nice_axis(0, 7300, 5, 1000);
    CHECK_EQ(a.step, 2000);
    CHECK_EQ(a.hi, 8000);
    // Flat value still gets one interval.
    a = nice_axis(0, 0, 5, 1000);
    CHECK_EQ(a.lo, 0);
    CHECK_EQ(a.hi, 1000);
    // Swapped args.
    a = nice_axis(50, 10, 4, 1);
    CHECK(a.lo <= 10 && a.hi >= 50);
}

static void test_auto_range() {
    RangeCfg temps = {};
    temps.auto_range = true;
    temps.min_span = 100;
    temps.pad_permille = 100;
    temps.min_pad = 20;
    temps.min_step = 10;
    // Tight data expands to min_span about the centre, then pads.
    Axis a = auto_range(temps, 400, 410, 6);
    CHECK_EQ(a.lo, 300);
    CHECK_EQ(a.hi, 500);
    // No data still gives a usable axis.
    a = auto_range(temps, 1, 0, 6);
    CHECK(a.hi > a.lo);

    RangeCfg power = {};
    power.auto_range = true;
    power.include_zero = true;
    power.min_span = 1000;
    power.pad_permille = 50;
    power.min_step = 1000;
    a = auto_range(power, 0, 3150, 5);
    CHECK_EQ(a.lo, 0);
    CHECK_EQ(a.hi, 4000);
    // Peak exactly on a step gets the 5 % headroom.
    a = auto_range(power, 0, 4000, 5);
    CHECK_EQ(a.lo, 0);
    CHECK_EQ(a.hi, 5000);
    // Idle: minimum 1 kW.
    a = auto_range(power, 0, 0, 5);
    CHECK_EQ(a.lo, 0);
    CHECK_EQ(a.hi, 1000);
    a = auto_range(power, 50, 300, 5);
    CHECK_EQ(a.lo, 0);
    CHECK_EQ(a.hi, 1000);
    a = auto_range(power, 0, 960, 5);   // 960 + 5 % > 1 kW
    CHECK_EQ(a.hi, 2000);

    // Hard limits in auto mode widen the range to include them.
    RangeCfg lim = temps;
    lim.has_min = true;
    lim.min = 0;
    a = auto_range(lim, 400, 410, 6);
    CHECK_EQ(a.lo, 0);

    // Fixed range keeps the configured bounds.
    RangeCfg fixed = {};
    fixed.has_min = fixed.has_max = true;
    fixed.min = 0;
    fixed.max = 60;
    fixed.min_step = 1;
    a = auto_range(fixed, 3, 7, 4);
    CHECK_EQ(a.lo, 0);
    CHECK_EQ(a.hi, 60);
    CHECK_EQ(a.step, 20);
}

static void test_mapping() {
    CHECK_EQ(map_x(1000, 1000, 2000, 10, 110), 10);
    CHECK_EQ(map_x(1500, 1000, 2000, 10, 110), 60);
    CHECK_EQ(map_x(2500, 1000, 2000, 10, 110), 110);
    CHECK_EQ(map_x(500, 1000, 2000, 10, 110), 10);
    CHECK_EQ(map_y(0, 0, 100, 0, 200), 200);
    CHECK_EQ(map_y(100, 0, 100, 0, 200), 0);
    CHECK_EQ(map_y(50, 0, 100, 0, 200), 100);
    CHECK_EQ(map_y(-50, -100, 0, 0, 200), 100);
    CHECK_EQ(time_at(60, 1000, 2000, 10, 110), 1500);
    CHECK_EQ(time_at(0, 1000, 2000, 10, 110), 1000);
    CHECK_EQ(time_at(200, 1000, 2000, 10, 110), 2000);
}

static void test_interpolate() {
    // Staircase 20, 20, 21, 21 at 60 s spacing.
    const uint32_t t[] = {0, 60, 120, 180};
    const int32_t v[] = {200, 200, 210, 210};
    int32_t out[4];
    interpolate_plateaus(t, v, 4, 300, out);
    // Plateau midpoints are 30 s (200) and 150 s (210).
    CHECK_EQ(out[0], 200);
    CHECK_EQ(out[1], 202);   // 1/4 of the way from 30 to 150
    CHECK_EQ(out[2], 207);
    CHECK_EQ(out[3], 210);

    // Gaps and NONE break the line; values are kept exactly at edges.
    const uint32_t t2[] = {0, 60, 1000, 1060, 1120};
    const int32_t v2[] = {100, 110, 300, NONE, 320};
    int32_t out2[5];
    interpolate_plateaus(t2, v2, 5, 300, out2);
    CHECK_EQ(out2[0], 100);
    CHECK_EQ(out2[1], 110);
    CHECK_EQ(out2[2], 300);
    CHECK_EQ(out2[3], NONE);
    CHECK_EQ(out2[4], 320);

    // A single long plateau stays flat.
    const uint32_t t3[] = {0, 60, 120};
    const int32_t v3[] = {50, 50, 50};
    int32_t out3[3];
    interpolate_plateaus(t3, v3, 3, 300, out3);
    CHECK_EQ(out3[0], 50);
    CHECK_EQ(out3[1], 50);
    CHECK_EQ(out3[2], 50);
}

static void test_decimate() {
    // Six samples in one column collapse to first/min/max/last.
    const int32_t xs[] = {5, 5, 5, 5, 5, 5, 6};
    const uint32_t t[] = {0, 1, 2, 3, 4, 5, 6};
    const int32_t v[] = {10, 3, 8, 20, 7, 9, 11};
    Point out[7];
    size_t n = decimate(xs, t, v, 7, 100, out);
    CHECK_EQ(n, 5);
    CHECK_EQ(out[0].v, 10);
    CHECK(out[0].brk);
    CHECK_EQ(out[1].v, 3);
    CHECK_EQ(out[2].v, 20);
    CHECK_EQ(out[3].v, 9);
    CHECK_EQ(out[4].v, 11);
    CHECK(!out[4].brk);

    // Gaps and NONE start new segments, never bridged in a column.
    const int32_t xs2[] = {1, 1, 1, 2, 3};
    const uint32_t t2[] = {0, 10, 500, 510, 520};
    const int32_t v2[] = {1, 2, 3, NONE, 5};
    Point out2[5];
    n = decimate(xs2, t2, v2, 5, 100, out2);
    CHECK_EQ(n, 4);
    CHECK(out2[0].brk);
    CHECK(!out2[1].brk);
    CHECK(out2[2].brk);
    CHECK_EQ(out2[2].v, 3);
    CHECK(out2[3].brk);
    CHECK_EQ(out2[3].v, 5);
}

static void test_split_panes() {
    const uint16_t w[] = {60, 40};
    const uint16_t mh[] = {0, 0};
    bool vis[] = {true, true};
    PaneSlot s[2];
    split_panes(0, 209, 10, w, mh, vis, 2, s);
    CHECK_EQ(s[0].y1, 0);
    CHECK_EQ(s[0].y2, 119);
    CHECK_EQ(s[1].y1, 130);
    CHECK_EQ(s[1].y2, 209);

    vis[0] = false;
    split_panes(0, 209, 10, w, mh, vis, 2, s);
    CHECK(s[0].y2 < s[0].y1);
    CHECK_EQ(s[1].y1, 0);
    CHECK_EQ(s[1].y2, 209);

    // Minimum heights win over a small weight.
    const uint16_t w3[] = {90, 10};
    const uint16_t mh3[] = {0, 60};
    bool vis3[] = {true, true};
    split_panes(0, 209, 10, w3, mh3, vis3, 2, s);
    CHECK_EQ(s[1].y2 - s[1].y1 + 1, 60);
    CHECK_EQ(s[0].y2, s[1].y1 - 11);

    CHECK_EQ(ticks_for_height(200, 6, 40), 5);
    CHECK_EQ(ticks_for_height(400, 6, 40), 6);
    CHECK_EQ(ticks_for_height(20, 6, 40), 1);
}

static void test_place_tags() {
    int32_t y[] = {100, 102, 50};
    size_t order[3];
    place_tags(y, 3, 20, 2, 0, 300, order);
    CHECK_EQ(order[0], 2);
    CHECK_EQ(order[1], 0);
    CHECK_EQ(order[2], 1);
    CHECK_EQ(y[2], 50);
    CHECK_EQ(y[0], 100);
    CHECK_EQ(y[1], 122);

    // Crowded at the bottom: shifted up, still ordered, in bounds.
    int32_t y2[] = {295, 290, 298};
    place_tags(y2, 3, 20, 2, 0, 300, order);
    CHECK_EQ(y2[order[2]], 290);
    CHECK_EQ(y2[order[1]], 268);
    CHECK_EQ(y2[order[0]], 246);

    // Too many for the space: top wins (clamped), order kept.
    int32_t y3[] = {5, 5, 5};
    place_tags(y3, 3, 20, 2, 0, 40, order);
    CHECK_EQ(y3[order[0]], 10);
    CHECK(y3[order[1]] > y3[order[0]]);
    CHECK(y3[order[2]] > y3[order[1]]);
}

static void test_pan() {
    // 1 h window ending at 10000 over 600 px, data 0 .. 10000.
    PanLimits lim = pan_limits(6400, 10000, 0, 10000, 600);
    CHECK_EQ(lim.min_dx, 0);
    CHECK_EQ(lim.max_dx, 1066);
    CHECK_EQ(rubber_band(-30, lim), -10);
    CHECK_EQ(rubber_band(1096, lim), 1076);
    CHECK_EQ(rubber_band(50, lim), 50);

    uint32_t e = 0;
    // 300 px right = 30 min back; 15 min snaps.
    CHECK(pan_target(300, 6400, 10000, 0, 10000, 600, 900, &e));
    CHECK_EQ(e, 8200);
    // Tiny drags spring back.
    CHECK(!pan_target(20, 6400, 10000, 0, 10000, 600, 900, &e));
    // Can't pass latest.
    CHECK(!pan_target(-300, 6400, 10000, 0, 10000, 600, 900, &e));
    CHECK(pan_target(-300, 2800, 6400, 0, 10000, 600, 900, &e));
    CHECK_EQ(e, 8200);
    // Pulled past the earliest end: whole snaps only, as today.
    CHECK(pan_target(5000, 2800, 6400, 0, 10000, 600, 900, &e));
    CHECK_EQ(e, 3700);

    CHECK_EQ(uncovered_permille(0, 1000, 500, 1500), 750);
    CHECK_EQ(uncovered_permille(500, 1500, 0, 1000), 250);
    CHECK_EQ(uncovered_permille(0, 1000, 5000, 6000), 500);
    CHECK_EQ(uncovered_permille(0, 1000, 0, 2000), 500);
}

static void test_time_axis() {
    CHECK_EQ(time_tick_step(3600, 4), 900);
    CHECK_EQ(time_tick_step(6 * 3600, 4), 7200);
    CHECK_EQ(time_tick_step(24 * 3600, 4), 21600);
    CHECK_EQ(time_tick_step(7 * 86400, 4), 86400);
    CHECK_EQ(first_tick(1000, 900, 1000), 1800);
    CHECK_EQ(first_tick(1800, 900, 1800), 1800);
    CHECK_EQ(first_tick(5000, 3600, 3600 * 5 + 200), 8400);
}

static void test_cursor() {
    const int32_t xs[] = {10, 20, 40, 80};
    const bool ok[] = {true, false, true, true};
    CHECK_EQ(nearest_x(xs, ok, 4, 22, 30), 0);
    CHECK_EQ(nearest_x(xs, nullptr, 4, 22, 30), 1);
    CHECK_EQ(nearest_x(xs, ok, 4, 200, 30), -1);
    const uint32_t t[] = {100, 200, 300};
    CHECK_EQ(nearest_time(t, 3, 240, 60), 1);
    CHECK_EQ(nearest_time(t, 3, 260, 60), 2);
    CHECK_EQ(nearest_time(t, 3, 400, 60), -1);
    CHECK_EQ(nearest_time(t, 3, 0, 100), 0);
    CHECK_EQ(nearest_time(t, 0, 0, 100), -1);
}

static void test_notes() {
    const int32_t xs[] = {10, 30, 200, 220, 400};
    size_t g[5];
    CHECK_EQ(group_notes(xs, 5, 50, g), 3);
    CHECK_EQ(g[0], 0);
    CHECK_EQ(g[1], 0);
    CHECK_EQ(g[2], 2);
    CHECK_EQ(g[3], 2);
    CHECK_EQ(g[4], 4);
}

int main() {
    test_nice_axis();
    test_auto_range();
    test_mapping();
    test_interpolate();
    test_decimate();
    test_split_panes();
    test_place_tags();
    test_pan();
    test_time_axis();
    test_cursor();
    test_notes();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
