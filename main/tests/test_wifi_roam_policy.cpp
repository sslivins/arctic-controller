// Host tests for WifiRoamPolicy: when to look for a stronger access point and
// which one to move to.

#include <cstdio>

#include "wifi_roam_policy.h"
#include "wifi_roam.h"

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__,      \
                         __LINE__, #cond);                                   \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

using P = WifiRoamPolicy;
static constexpr int64_t TICK = 15000;  // supervisor interval

static const uint8_t A[6] = {1, 1, 1, 1, 1, 1};
static const uint8_t B[6] = {2, 2, 2, 2, 2, 2};
static const uint8_t C[6] = {3, 3, 3, 3, 3, 3};

static WifiRoamCandidate cand(const uint8_t* b, int rssi, int ch) {
    WifiRoamCandidate c{};
    memcpy(c.bssid, b, 6);
    c.rssi = (int8_t)rssi;
    c.channel = (uint8_t)ch;
    return c;
}

// Samples every tick from `from` until the policy asks for a scan; returns
// the time it did, or -1 if it never did before `until`.
static int64_t first_scan(P& p, int64_t from, int64_t until, int rssi) {
    for (int64_t t = from; t <= until; t += TICK) {
        if (p.sample(t, true, rssi)) return t;
    }
    return -1;
}

// Runs a fresh connection through its settle period, including the one-off
// check after connecting if it fires (recorded as a scan that found nothing
// better). Returns the time of the first sample past the settle period.
static int64_t past_connect_check(P& p, int rssi) {
    for (int64_t t = 0;; t += TICK) {
        const bool scan = p.sample(t, true, rssi);
        if (t >= P::SETTLE_MS) {
            if (scan) p.pick(t, A, rssi, nullptr, 0);
            return t;
        }
    }
}

static void strong_signal_never_scans() {
    P p;
    CHECK(first_scan(p, 0, 3600000, -55) == -1);
    P q;
    CHECK(first_scan(q, 0, 3600000, P::GOOD_RSSI + 1) == -1);
}

static void mediocre_signal_checks_once_after_connecting() {
    P p;
    const int64_t t = first_scan(p, 0, 3600000, P::GOOD_RSSI);
    CHECK(t >= P::SETTLE_MS);
    CHECK(t < P::SETTLE_MS + TICK);
    p.pick(t, A, P::GOOD_RSSI, nullptr, 0);
    CHECK(first_scan(p, t + TICK, 7200000, P::GOOD_RSSI) == -1);
    CHECK(first_scan(p, 7200000, 14400000, P::WEAK_RSSI + 1) == -1);
}

static void each_new_connection_gets_a_check() {
    P p;
    int64_t t = past_connect_check(p, -65);
    CHECK(first_scan(p, t + TICK, t + 600000, -65) == -1);
    t += 600000;
    CHECK(!p.sample(t, false, 0));
    t += TICK;
    const int64_t back = t;
    const int64_t scan = first_scan(p, t, t + 600000, -65);
    CHECK(scan >= back + P::SETTLE_MS);
    CHECK(scan < back + P::SETTLE_MS + TICK);
}

static void unknown_rssi_never_scans() {
    P p;
    CHECK(first_scan(p, 0, 7200000, 0) == -1);
}

static void weak_signal_scans_after_settle() {
    P p;
    // Weak from the moment we connect: the check after settling covers it.
    const int64_t t = first_scan(p, 0, 3600000, -80);
    CHECK(t >= P::SETTLE_MS);
    CHECK(t < P::SETTLE_MS + TICK);
}

static void weak_needs_a_continuous_minute() {
    P p;
    int64_t t = past_connect_check(p, -50) + P::RESCAN_MS;
    for (int64_t end = t + 300000; t < end; t += TICK) CHECK(!p.sample(t, true, -50));
    // Weak, then one good sample resets the clock.
    for (int i = 0; i < 3; i++, t += TICK) CHECK(!p.sample(t, true, -75));
    CHECK(!p.sample(t, true, -60));
    t += TICK;
    const int64_t weak_from = t;
    const int64_t scan = first_scan(p, t, t + 600000, -75);
    CHECK(scan >= weak_from + P::WEAK_FOR_MS);
    CHECK(scan < weak_from + P::WEAK_FOR_MS + TICK);
}

static void weak_soon_after_connect_check_waits_for_rescan() {
    P p;
    const int64_t t1 = past_connect_check(p, -65);
    const int64_t t2 = first_scan(p, t1 + TICK, t1 + 7200000, -80);
    CHECK(t2 >= t1 + P::RESCAN_MS);
    CHECK(t2 < t1 + P::RESCAN_MS + TICK);
}

static void disconnect_restarts_settle() {
    P p;
    int64_t t = past_connect_check(p, -50);
    for (; t < 600000; t += TICK) p.sample(t, true, -50);
    CHECK(!p.sample(t, false, 0));
    t += TICK;
    const int64_t back = t;
    const int64_t scan = first_scan(p, t, t + 600000, -80);
    CHECK(scan >= back + P::SETTLE_MS);
}

static void rescan_is_rate_limited() {
    P p;
    const int64_t t1 = first_scan(p, 0, 3600000, -80);
    CHECK(t1 > 0);
    CHECK(p.pick(t1, A, -80, nullptr, 0) == -1);  // nothing better
    const int64_t t2 = first_scan(p, t1 + TICK, t1 + 7200000, -80);
    CHECK(t2 >= t1 + P::RESCAN_MS);
    CHECK(t2 < t1 + P::RESCAN_MS + TICK);
}

static void picks_strongest_clearly_better_other_ap() {
    P p;
    WifiRoamCandidate c[] = {cand(A, -50, 1), cand(B, -68, 6), cand(C, -62, 11)};
    // Current AP A at -78 appears strong in the scan list (stale); ignored.
    CHECK(p.pick(0, A, -78, c, 3) == 2);
}

static void stays_unless_gain_is_large_enough() {
    P p;
    WifiRoamCandidate c[] = {cand(A, -75, 1), cand(B, -75 + P::MIN_GAIN_DB - 1, 6)};
    CHECK(p.pick(0, A, -75, c, 2) == -1);
    c[1].rssi = (int8_t)(-75 + P::MIN_GAIN_DB);
    CHECK(p.pick(0, A, -75, c, 2) == 1);
}

static void only_ap_is_current_means_stay() {
    P p;
    WifiRoamCandidate c[] = {cand(A, -80, 1)};
    CHECK(p.pick(0, A, -80, c, 1) == -1);
    CHECK(p.pick(0, A, -80, c, 1, true) == -1);
}

static void any_takes_strongest_other_even_if_worse() {
    P p;
    WifiRoamCandidate c[] = {cand(A, -50, 1), cand(B, -70, 6), cand(C, -65, 11)};
    CHECK(p.pick(0, A, -50, c, 3, true) == 2);
}

static void after_roam_settles_and_backs_off() {
    P p;
    const int64_t t1 = first_scan(p, 0, 3600000, -80);
    p.on_roam(t1);
    // Even still weak after the move, no new scan for the backoff.
    const int64_t t2 = first_scan(p, t1 + TICK, t1 + 7200000, -80);
    CHECK(t2 >= t1 + P::RESCAN_MS);
}

static void event_payload_round_trips() {
    const uint32_t v = wifi_roam_pack(-78, -52, 11);
    CHECK(wifi_roam_from_rssi(v) == -78);
    CHECK(wifi_roam_to_rssi(v) == -52);
    CHECK(wifi_roam_channel(v) == 11);
    const uint32_t w = wifi_roam_pack(-100, -1, 165);
    CHECK(wifi_roam_from_rssi(w) == -100);
    CHECK(wifi_roam_to_rssi(w) == -1);
    CHECK(wifi_roam_channel(w) == 165);
}

int main() {
    strong_signal_never_scans();
    mediocre_signal_checks_once_after_connecting();
    each_new_connection_gets_a_check();
    unknown_rssi_never_scans();
    weak_signal_scans_after_settle();
    weak_needs_a_continuous_minute();
    weak_soon_after_connect_check_waits_for_rescan();
    disconnect_restarts_settle();
    rescan_is_rate_limited();
    picks_strongest_clearly_better_other_ap();
    stays_unless_gain_is_large_enough();
    only_ap_is_current_means_stay();
    any_takes_strongest_other_even_if_worse();
    after_roam_settles_and_backs_off();
    event_payload_round_trips();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("wifi_roam_policy: all tests passed\n");
    return 0;
}
