// Fault spans for the history charts: error_appeared/error_cleared pairing,
// window clipping, reboots mid-fault and still-active faults.

#include "fault_history.h"

#include <cstdio>
#include <vector>

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,    \
                         __LINE__, #cond);                                 \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

#define CHECK_EQ_U(actual, expected)                                         \
    do {                                                                     \
        unsigned long a_ = (unsigned long)(actual);                          \
        unsigned long e_ = (unsigned long)(expected);                        \
        if (a_ != e_) {                                                      \
            std::fprintf(stderr, "%s:%d: %s == %lu, expected %lu\n",         \
                         __FILE__, __LINE__, #actual, a_, e_);               \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static constexpr uint32_t BOOT_A = 0xA;
static constexpr uint32_t BOOT_B = 0xB;
static constexpr uint32_t T0 = 1'800'000'000;

static event_entry_t ev(uint32_t ts, uint32_t boot, event_type_t type,
                        uint32_t payload = 0) {
    event_entry_t e = {};
    e.timestamp = ts;
    e.boot_id = boot;
    e.type = type;
    e.payload = payload;
    return e;
}

static std::vector<fault_interval_t> spans(const std::vector<event_entry_t>& e,
                                           uint32_t ws, uint32_t we,
                                           uint32_t boot) {
    fault_interval_t out[16];
    size_t n = fault_intervals_from_events(e.data(), e.size(), ws, we, boot,
                                           out, 16);
    return std::vector<fault_interval_t>(out, out + n);
}

static void pairs_appeared_with_cleared() {
    auto s = spans({ev(T0, BOOT_A, EVENT_SYSTEM_START),
                    ev(T0 + 100, BOOT_A, EVENT_ERROR_APPEARED, 7),
                    ev(T0 + 700, BOOT_A, EVENT_ERROR_CLEARED, 7)},
                   T0, T0 + 3600, BOOT_A);
    CHECK_EQ_U(s.size(), 1);
    if (s.size() == 1) {
        CHECK_EQ_U(s[0].site, 7);
        CHECK_EQ_U(s[0].start, T0 + 100);
        CHECK_EQ_U(s[0].end, T0 + 700);
    }
}

static void active_fault_in_this_boot_is_open() {
    auto s = spans({ev(T0 + 100, BOOT_A, EVENT_ERROR_APPEARED, 3)}, T0,
                   T0 + 3600, BOOT_A);
    CHECK_EQ_U(s.size(), 1);
    if (s.size() == 1) CHECK_EQ_U(s[0].end, 0);
}

static void reboot_closes_the_span_at_the_last_event_of_that_boot() {
    // P02 active, the controller reboots, and the fault is logged again by the
    // new boot then cleared. Two spans, with no fault drawn over the gap.
    auto s = spans({ev(T0 + 100, BOOT_A, EVENT_ERROR_APPEARED, 2),
                    ev(T0 + 400, BOOT_A, EVENT_COMPRESSOR_OFF),
                    ev(T0 + 5000, BOOT_B, EVENT_SYSTEM_START),
                    ev(T0 + 5010, BOOT_B, EVENT_ERROR_APPEARED, 2),
                    ev(T0 + 5300, BOOT_B, EVENT_ERROR_CLEARED, 2)},
                   T0, T0 + 9000, BOOT_B);
    CHECK_EQ_U(s.size(), 2);
    if (s.size() == 2) {
        CHECK_EQ_U(s[0].start, T0 + 100);
        CHECK_EQ_U(s[0].end, T0 + 400);
        CHECK_EQ_U(s[1].start, T0 + 5010);
        CHECK_EQ_U(s[1].end, T0 + 5300);
    }
}

static void open_span_from_an_old_boot_is_never_active() {
    auto s = spans({ev(T0 + 100, BOOT_A, EVENT_ERROR_APPEARED, 2),
                    ev(T0 + 200, BOOT_A, EVENT_COMPRESSOR_OFF)},
                   T0, T0 + 9000, BOOT_B);
    CHECK_EQ_U(s.size(), 1);
    if (s.size() == 1) CHECK_EQ_U(s[0].end, T0 + 200);
}

static void duplicate_appeared_keeps_the_first_start() {
    auto s = spans({ev(T0 + 100, BOOT_A, EVENT_ERROR_APPEARED, 5),
                    ev(T0 + 200, BOOT_A, EVENT_ERROR_APPEARED, 5),
                    ev(T0 + 300, BOOT_A, EVENT_ERROR_CLEARED, 5)},
                   T0, T0 + 9000, BOOT_A);
    CHECK_EQ_U(s.size(), 1);
    if (s.size() == 1) CHECK_EQ_U(s[0].start, T0 + 100);
}

static void clear_without_appeared_is_ignored() {
    auto s = spans({ev(T0 + 300, BOOT_A, EVENT_ERROR_CLEARED, 5)}, T0,
                   T0 + 9000, BOOT_A);
    CHECK_EQ_U(s.size(), 0);
}

static void untimed_events_are_skipped() {
    auto s = spans({ev(0, BOOT_A, EVENT_ERROR_APPEARED, 5),
                    ev(T0 + 300, BOOT_A, EVENT_ERROR_CLEARED, 5)},
                   T0, T0 + 9000, BOOT_A);
    CHECK_EQ_U(s.size(), 0);
}

static void only_spans_overlapping_the_window_are_returned() {
    auto s = spans({ev(T0 + 100, BOOT_A, EVENT_ERROR_APPEARED, 1),
                    ev(T0 + 200, BOOT_A, EVENT_ERROR_CLEARED, 1),
                    ev(T0 + 1000, BOOT_A, EVENT_ERROR_APPEARED, 2),
                    ev(T0 + 2000, BOOT_A, EVENT_ERROR_CLEARED, 2),
                    ev(T0 + 5000, BOOT_A, EVENT_ERROR_APPEARED, 3),
                    ev(T0 + 5100, BOOT_A, EVENT_ERROR_CLEARED, 3)},
                   T0 + 1500, T0 + 4000, BOOT_A);
    CHECK_EQ_U(s.size(), 1);
    if (s.size() == 1) CHECK_EQ_U(s[0].site, 2);
}

static void overlapping_faults_are_sorted_by_start() {
    auto s = spans({ev(T0 + 100, BOOT_A, EVENT_ERROR_APPEARED, 1),
                    ev(T0 + 150, BOOT_A, EVENT_ERROR_APPEARED, 2),
                    ev(T0 + 160, BOOT_A, EVENT_ERROR_CLEARED, 2),
                    ev(T0 + 900, BOOT_A, EVENT_ERROR_CLEARED, 1)},
                   T0, T0 + 9000, BOOT_A);
    CHECK_EQ_U(s.size(), 2);
    if (s.size() == 2) {
        CHECK_EQ_U(s[0].site, 1);
        CHECK_EQ_U(s[1].site, 2);
    }
}

static void output_is_capped() {
    std::vector<event_entry_t> e;
    for (uint32_t i = 0; i < 10; ++i) {
        e.push_back(ev(T0 + i * 10, BOOT_A, EVENT_ERROR_APPEARED, 1));
        e.push_back(ev(T0 + i * 10 + 5, BOOT_A, EVENT_ERROR_CLEARED, 1));
    }
    fault_interval_t out[4];
    CHECK_EQ_U(fault_intervals_from_events(e.data(), e.size(), T0, T0 + 9000,
                                           BOOT_A, out, 4),
               4);
}

static void cap_keeps_the_newest_spans() {
    std::vector<event_entry_t> e;
    for (uint32_t i = 0; i < 10; ++i) {
        e.push_back(ev(T0 + i * 10, BOOT_A, EVENT_ERROR_APPEARED, 1));
        e.push_back(ev(T0 + i * 10 + 5, BOOT_A, EVENT_ERROR_CLEARED, 1));
    }
    // Plus one still active, emitted last.
    e.push_back(ev(T0 + 500, BOOT_A, EVENT_ERROR_APPEARED, 2));
    fault_interval_t out[4];
    size_t n = fault_intervals_from_events(e.data(), e.size(), T0, T0 + 9000,
                                           BOOT_A, out, 4);
    CHECK_EQ_U(n, 4);
    if (n == 4) {
        CHECK_EQ_U(out[0].start, T0 + 70);
        CHECK_EQ_U(out[1].start, T0 + 80);
        CHECK_EQ_U(out[2].start, T0 + 90);
        CHECK_EQ_U(out[3].start, T0 + 500);
        CHECK_EQ_U(out[3].end, 0);
    }
}

int main() {
    pairs_appeared_with_cleared();
    active_fault_in_this_boot_is_open();
    reboot_closes_the_span_at_the_last_event_of_that_boot();
    open_span_from_an_old_boot_is_never_active();
    duplicate_appeared_keeps_the_first_start();
    clear_without_appeared_is_ignored();
    untimed_events_are_skipped();
    only_spans_overlapping_the_window_are_returned();
    overlapping_faults_are_sorted_by_start();
    output_is_capped();
    cap_keeps_the_newest_spans();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("fault_history: all tests passed\n");
    return 0;
}
