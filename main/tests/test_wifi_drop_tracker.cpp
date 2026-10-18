// Host tests for WifiDropTracker: which WiFi outages reach the event log.

#include <cstdio>

#include "wifi_drop_tracker.h"

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__,      \
                         __LINE__, #cond);                                   \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static constexpr int64_t L = WifiDropTracker::LONG_MS;
static constexpr int64_t W = WifiDropTracker::UNSTABLE_WINDOW_MS;

struct Polled {
    WifiDropEvent ev[8];
    int n;
};

static Polled poll(WifiDropTracker& t, int64_t now) {
    Polled p{};
    p.n = t.poll(now, p.ev, 8);
    return p;
}

static void short_drop(WifiDropTracker& t, int64_t at) {
    t.on_down(at, 200, false);
    t.on_up(at + 5000);
}

static void ignored_before_first_connect() {
    WifiDropTracker t;
    t.on_down(1000, 201, false);
    CHECK(poll(t, 1000 + 2 * L).n == 0);
    t.on_up(1000 + 2 * L);
    CHECK(poll(t, 1000 + 2 * L).n == 0);
    CHECK(t.stats(1000 + 2 * L).drops == 0);
}

static void local_disconnects_are_not_drops() {
    WifiDropTracker t;
    t.on_up(0);
    t.on_down(1000, 8, true);
    CHECK(!t.stats(1000 + 2 * L).down);
    CHECK(poll(t, 1000 + 2 * L).n == 0);
    t.on_up(1000 + 2 * L);
    CHECK(poll(t, 1000 + 2 * L).n == 0);
    const WifiDropStats s = t.stats(1000 + 2 * L);
    CHECK(s.drops == 0 && s.short_drops == 0 && s.long_drops == 0);
}

static void short_drop_is_counted_not_logged() {
    WifiDropTracker t;
    t.on_up(0);
    t.on_down(1000, 200, false);
    CHECK(t.stats(11000).down);
    CHECK(t.stats(11000).down_s == 10);
    t.on_up(31000);
    CHECK(poll(t, 31000).n == 0);
    const WifiDropStats s = t.stats(31000);
    CHECK(s.drops == 1 && s.short_drops == 1 && s.long_drops == 0);
    CHECK(s.last_reason == 200);
    CHECK(!s.down && s.down_s == 0);
}

static void retries_during_an_outage_are_one_outage() {
    WifiDropTracker t;
    t.on_up(0);
    t.on_down(1000, 200, false);
    t.on_down(5000, 201, false);
    t.on_down(9000, 201, false);
    t.on_up(12000);
    CHECK(t.stats(12000).drops == 1);
    CHECK(t.stats(12000).last_reason == 200);
}

static void long_outage_logs_lost_then_restored() {
    WifiDropTracker t;
    t.on_up(0);
    t.on_down(1000, 200, false);
    CHECK(poll(t, 1000 + L - 1).n == 0);
    Polled p = poll(t, 1000 + L);
    CHECK(p.n == 1);
    CHECK(p.ev[0].action == WifiDropAction::LOST && p.ev[0].payload == 200);
    CHECK(t.stats(1000 + L).long_drops == 1);
    CHECK(poll(t, 1000 + 3 * L).n == 0);  // LOST only once

    t.on_up(1000 + 252000);
    p = poll(t, 1000 + 252000);
    CHECK(p.n == 1);
    CHECK(p.ev[0].action == WifiDropAction::RESTORED && p.ev[0].payload == 252);
    const WifiDropStats s = t.stats(1000 + 252000);
    CHECK(s.drops == 1 && s.long_drops == 1 && s.short_drops == 0);
}

static void long_outage_ending_between_polls_logs_both() {
    WifiDropTracker t;
    t.on_up(0);
    t.on_down(1000, 4, false);
    t.on_up(1000 + L + 5000);
    Polled p = poll(t, 1000 + L + 6000);
    CHECK(p.n == 2);
    CHECK(p.ev[0].action == WifiDropAction::LOST && p.ev[0].payload == 4);
    CHECK(p.ev[1].action == WifiDropAction::RESTORED && p.ev[1].payload == 65);
    CHECK(t.stats(1000 + L + 6000).long_drops == 1);
}

static void three_short_drops_in_an_hour_log_unstable_once() {
    WifiDropTracker t;
    t.on_up(0);
    short_drop(t, 60000);
    short_drop(t, 600000);
    CHECK(poll(t, 700000).n == 0);
    short_drop(t, 1200000);
    Polled p = poll(t, 1300000);
    CHECK(p.n == 1);
    CHECK(p.ev[0].action == WifiDropAction::UNSTABLE && p.ev[0].payload == 3);

    // Two more during the quiet hour: not enough for another report.
    short_drop(t, 1500000);
    short_drop(t, 2000000);
    CHECK(poll(t, 2100000).n == 0);
    CHECK(poll(t, 1300000 + W).n == 0);
    // And they don't seed the next window either.
    short_drop(t, 1300000 + W + 1000);
    CHECK(poll(t, 1300000 + W + 10000).n == 0);
    CHECK(t.stats(1300000 + W + 10000).short_drops == 6);
}

static void drops_spread_over_more_than_an_hour_are_not_unstable() {
    WifiDropTracker t;
    t.on_up(0);
    short_drop(t, 0);
    short_drop(t, W / 2);
    short_drop(t, W + 10000);
    CHECK(poll(t, W + 20000).n == 0);
}

static void a_flapping_link_reports_once_per_window() {
    WifiDropTracker t;
    t.on_up(0);
    int64_t now = 0;
    for (int i = 0; i < 3; i++) short_drop(t, now += 60000);
    Polled p = poll(t, now + 1000);
    CHECK(p.n == 1 && p.ev[0].payload == 3);
    const int64_t quiet_end = now + 1000 + W;
    // Ten more drops through the quiet hour: one report when it ends.
    int unstable = 0;
    for (int i = 0; i < 10; i++) {
        short_drop(t, now += 300000);
        unstable += poll(t, now + 6000).n;
    }
    CHECK(unstable == 0);
    p = poll(t, quiet_end);
    CHECK(p.n == 1);
    CHECK(p.ev[0].action == WifiDropAction::UNSTABLE && p.ev[0].payload == 10);
}

static void long_drops_do_not_count_toward_unstable() {
    WifiDropTracker t;
    t.on_up(0);
    int64_t now = 0;
    int unstable = 0;
    for (int i = 0; i < 3; i++) {
        t.on_down(now += 100000, 200, false);
        t.on_up(now += L + 1000);
        Polled p = poll(t, now);
        CHECK(p.n == 2);
        for (int j = 0; j < p.n; j++) {
            if (p.ev[j].action == WifiDropAction::UNSTABLE) unstable++;
        }
    }
    CHECK(unstable == 0);
}

int main() {
    ignored_before_first_connect();
    local_disconnects_are_not_drops();
    short_drop_is_counted_not_logged();
    retries_during_an_outage_are_one_outage();
    long_outage_logs_lost_then_restored();
    long_outage_ending_between_polls_logs_both();
    three_short_drops_in_an_hour_log_unstable_once();
    drops_spread_over_more_than_an_hour_are_not_unstable();
    a_flapping_link_reports_once_per_window();
    long_drops_do_not_count_toward_unstable();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("wifi_drop_tracker: all tests passed\n");
    return 0;
}
