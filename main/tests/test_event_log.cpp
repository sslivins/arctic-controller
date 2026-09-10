// Native unit tests for main/event_log.cpp -- the operational event ring and
// the boot-scoped timestamp repair that runs after NTP.
//
// Why these and not device tests: the event log's interesting behaviour is all
// in states the one physical controller cannot easily be put into. Filling the
// 1000-entry ring means generating a thousand real events. Proving that a
// previous boot's entries are NOT rewritten by a timestamp backfill needs two
// boots with different boot IDs and a clock that starts out unset. Proving a
// failed journal append falls back to compaction needs flash to fail on
// demand. Over HTTP, all of this is invisible: the API returns a list of
// events either way.
//
// Layering: these tests run event_log.cpp on top of the REAL
// history_storage.cpp over the flash fake -- not a mocked store. The two
// modules' contract (append vs. replace, event-id reuse on backfill) is where
// the bugs would live, so stubbing the lower half would test nothing worth
// testing.
//
// HOW A REBOOT IS MODELLED: same as test_history_storage.cpp -- each phase
// runs in a forked child, so file-scope statics start cold while the flash
// image (shared memory) carries over. event_log.cpp keeps its ring, head,
// count and boot id in statics, so an in-process "reboot" would re-read RAM
// and every persistence assertion would pass vacuously.
//
// ONE SHARP EDGE, called out because it silently weakens tests: the RNG is
// seeded per process and a forked child inherits the parent's state. Two
// phases that both call event_log_init() would therefore mint the SAME
// boot_id, and every "previous boot is left alone" assertion would pass for
// the wrong reason -- there would be only one boot. boot() below takes a seed
// and demands the caller vary it.
//
// Refs #217 (T13).

#include "esp_partition_fake.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "event_log.h"
#include "history_storage.h"
#include "time_fake.h"

#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

static int g_failures = 0;

// Failures go to stderr: each recorded event emits an ESP_LOGI line on stdout,
// and the ring-wrap tests record a thousand of them. Child stdout is sent to
// /dev/null (see run_scenario) so a failure is not buried under it.
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                           \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

#define CHECK_EQ_INT(actual, expected)                                       \
    do {                                                                     \
        long a_ = (long)(actual);                                            \
        long e_ = (long)(expected);                                          \
        if (a_ != e_) {                                                      \
            std::fprintf(stderr, "  FAIL %s:%d: %s -> %ld, expected %ld\n",  \
                         __FILE__, __LINE__, #actual, a_, e_);               \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

namespace {

// A boot. The seed must differ between phases of the same scenario, or both
// boots mint the same boot_id -- see the header comment.
void boot(uint32_t rng_seed) {
    random_fake::reset(rng_seed);
    timer_fake::reset();
    event_log_init();
}

// First boot of a device whose history partition has never been written.
void fresh_boot(uint32_t rng_seed) {
    flash_fake::install_history_partition();
    boot(rng_seed);
}

std::vector<event_entry_t> read_all(int offset = 0) {
    std::vector<event_entry_t> out(EVENT_LOG_MAX_ENTRIES);
    int n = event_log_get(out.data(), (int)out.size(), offset);
    out.resize(n < 0 ? 0 : (size_t)n);
    return out;
}

// The newest entry, or a zeroed entry if the log is empty.
event_entry_t newest() {
    event_entry_t e{};
    event_log_get(&e, 1, 0);
    return e;
}

struct timeval tv_at(time_t sec) {
    struct timeval tv {};
    tv.tv_sec = sec;
    tv.tv_usec = 0;
    return tv;
}

// ------------------------------------------------------------------------
// Naming and categorisation
//
// These strings are the API's stable vocabulary -- tests/api and the UI match
// on them -- but nothing asserted they were complete or distinct. A duplicated
// entry in s_event_names would make two different events indistinguishable to
// every consumer, and the _Static_assert on array length would not notice.
// ------------------------------------------------------------------------
void every_event_type_has_a_unique_non_empty_name() {
    std::set<std::string> seen;
    for (int t = 0; t < EVENT_TYPE_COUNT; ++t) {
        const char* name = event_type_name((event_type_t)t);
        CHECK(name != nullptr);
        if (name == nullptr) continue;
        CHECK(name[0] != '\0');
        CHECK(std::strcmp(name, "unknown") != 0);
        // Duplicate names would collapse two distinct events into one as far
        // as any API consumer is concerned.
        CHECK(seen.insert(name).second);
    }
    CHECK_EQ_INT(seen.size(), EVENT_TYPE_COUNT);
}

void out_of_range_event_type_is_named_unknown() {
    CHECK(std::strcmp(event_type_name((event_type_t)EVENT_TYPE_COUNT), "unknown") == 0);
    CHECK(std::strcmp(event_type_name((event_type_t)(EVENT_TYPE_COUNT + 99)), "unknown") == 0);
}

void event_names_are_the_documented_api_strings() {
    // Spot-check the ones the API and UI match on by name. If someone renames
    // these, that is an API break and should fail here rather than in a
    // browser.
    CHECK(std::strcmp(event_type_name(EVENT_SYSTEM_START), "system_start") == 0);
    CHECK(std::strcmp(event_type_name(EVENT_ERROR_APPEARED), "error_appeared") == 0);
    CHECK(std::strcmp(event_type_name(EVENT_NETWORK_RECOVERED), "network_recovered") == 0);
    CHECK(std::strcmp(event_type_name(EVENT_WATCHDOG_RESET), "watchdog_reset") == 0);
}

void every_event_type_maps_to_a_valid_category() {
    for (int t = 0; t < EVENT_TYPE_COUNT; ++t) {
        event_category_t c = event_type_category((event_type_t)t);
        CHECK(c >= 0 && c < EVENT_CATEGORY_COUNT);
    }
}

void categories_match_the_intended_grouping() {
    // Problems: what a user is asked to act on.
    const event_type_t problems[] = {
        EVENT_ERROR_APPEARED, EVENT_ERROR_CLEARED,     EVENT_DISCONNECTED,
        EVENT_BROWNOUT_RESET, EVENT_APPLICATION_CRASH, EVENT_WATCHDOG_RESET,
        EVENT_NETWORK_UNREACHABLE, EVENT_NETWORK_RECOVERED};
    for (event_type_t t : problems) {
        CHECK(event_type_category(t) == EVENT_CATEGORY_PROBLEMS);
    }

    const event_type_t equipment[] = {
        EVENT_COMPRESSOR_ON, EVENT_COMPRESSOR_OFF, EVENT_FAN_ON,
        EVENT_FAN_OFF,       EVENT_PUMP_ON,        EVENT_PUMP_OFF,
        EVENT_AUX_HEATER_ON, EVENT_AUX_HEATER_OFF, EVENT_DEFROST_START,
        EVENT_DEFROST_END};
    for (event_type_t t : equipment) {
        CHECK(event_type_category(t) == EVENT_CATEGORY_EQUIPMENT);
    }

    const event_type_t changes[] = {EVENT_POWER_ON, EVENT_POWER_OFF,
                                    EVENT_MODE_CHANGED, EVENT_SETPOINT_CHANGED};
    for (event_type_t t : changes) {
        CHECK(event_type_category(t) == EVENT_CATEGORY_CHANGES);
    }

    const event_type_t system[] = {EVENT_SYSTEM_START, EVENT_CONNECTED};
    for (event_type_t t : system) {
        CHECK(event_type_category(t) == EVENT_CATEGORY_SYSTEM);
    }
}

void category_names_are_stable_and_bounded() {
    CHECK(std::strcmp(event_category_name(EVENT_CATEGORY_PROBLEMS), "problems") == 0);
    CHECK(std::strcmp(event_category_name(EVENT_CATEGORY_EQUIPMENT), "equipment") == 0);
    CHECK(std::strcmp(event_category_name(EVENT_CATEGORY_CHANGES), "changes") == 0);
    CHECK(std::strcmp(event_category_name(EVENT_CATEGORY_SYSTEM), "system") == 0);
    // Out of range must not index past the table.
    CHECK(std::strcmp(event_category_name((event_category_t)EVENT_CATEGORY_COUNT), "system") == 0);
}

// ------------------------------------------------------------------------
// Recording
// ------------------------------------------------------------------------
void recording_before_init_is_ignored_not_a_crash() {
    // No init: the mutex is still null. This is reachable on the device --
    // anything that records an event during early boot ordering changes.
    event_log_record(EVENT_POWER_ON, 1);
    CHECK_EQ_INT(event_log_count(), 0);
    CHECK_EQ_INT(event_log_revision(), 0);
    CHECK_EQ_INT(event_log_get(nullptr, 1, 0), 0);
}

void init_records_a_system_start() {
    fresh_boot(1);
    CHECK_EQ_INT(event_log_count(), 1);
    CHECK(newest().type == EVENT_SYSTEM_START);
}

void an_invalid_event_type_is_rejected() {
    fresh_boot(1);
    event_log_record((event_type_t)EVENT_TYPE_COUNT, 0);
    event_log_record((event_type_t)(EVENT_TYPE_COUNT + 5), 0);
    // Only the SYSTEM_START from init.
    CHECK_EQ_INT(event_log_count(), 1);
}

void events_come_back_newest_first() {
    fresh_boot(1);
    event_log_record(EVENT_POWER_ON, 11);
    event_log_record(EVENT_COMPRESSOR_ON, 22);

    std::vector<event_entry_t> all = read_all();
    CHECK_EQ_INT(all.size(), 3);
    if (all.size() != 3) return;
    CHECK(all[0].type == EVENT_COMPRESSOR_ON && all[0].payload == 22);
    CHECK(all[1].type == EVENT_POWER_ON && all[1].payload == 11);
    CHECK(all[2].type == EVENT_SYSTEM_START);
}

void the_revision_advances_with_every_record() {
    fresh_boot(1);
    uint32_t r0 = event_log_revision();
    event_log_record(EVENT_FAN_ON, 0);
    uint32_t r1 = event_log_revision();
    event_log_record(EVENT_FAN_OFF, 0);
    uint32_t r2 = event_log_revision();
    CHECK(r1 == r0 + 1);
    CHECK(r2 == r1 + 1);
}

void every_event_from_one_boot_shares_the_boot_id() {
    fresh_boot(1);
    event_log_record(EVENT_PUMP_ON, 0);
    event_log_record(EVENT_PUMP_OFF, 0);

    uint32_t boot_id = event_log_current_boot_id();
    CHECK(boot_id != 0);
    for (const event_entry_t& e : read_all()) {
        CHECK(e.boot_id == boot_id);
    }
}

void a_zero_boot_id_is_never_accepted() {
    // boot_id 0 is the sentinel meaning "no boot", so event_log_init retries
    // until the RNG gives something else. Real hardware would essentially
    // never produce three zeros in a row, so this branch is unreachable
    // without steering the RNG -- and it is exactly the kind of loop that gets
    // "simplified" into a single call.
    flash_fake::install_history_partition();
    random_fake::reset(1);
    random_fake::force_next(0, 3);
    event_log_init();

    CHECK(event_log_current_boot_id() != 0);
    // Three rejected draws plus the one that was accepted.
    CHECK(random_fake::calls() >= 4);
}

// ------------------------------------------------------------------------
// Pagination and bounds
// ------------------------------------------------------------------------
void pagination_walks_backwards_without_gaps_or_repeats() {
    fresh_boot(1);
    for (uint32_t i = 1; i <= 10; ++i) {
        event_log_record(EVENT_MODE_CHANGED, i);
    }

    // Page through two at a time and rebuild the sequence.
    std::vector<uint32_t> seen;
    for (int offset = 0;; offset += 2) {
        event_entry_t page[2];
        int n = event_log_get(page, 2, offset);
        if (n == 0) break;
        for (int i = 0; i < n; ++i) seen.push_back(page[i].payload);
        if (n < 2) break;
    }

    // 10 recorded events plus the SYSTEM_START, newest first.
    CHECK_EQ_INT(seen.size(), 11);
    if (seen.size() != 11) return;
    for (size_t i = 0; i < 10; ++i) {
        CHECK_EQ_INT(seen[i], 10 - i);
    }
    CHECK_EQ_INT(seen[10], 0);  // SYSTEM_START, payload 0
}

void reading_past_the_end_returns_nothing() {
    fresh_boot(1);
    event_log_record(EVENT_POWER_ON, 1);

    event_entry_t out[4];
    CHECK_EQ_INT(event_log_get(out, 4, 2), 0);    // exactly at the end
    CHECK_EQ_INT(event_log_get(out, 4, 99), 0);   // well past it
}

void invalid_get_arguments_are_rejected() {
    fresh_boot(1);
    event_entry_t out[4];
    CHECK_EQ_INT(event_log_get(nullptr, 4, 0), 0);
    CHECK_EQ_INT(event_log_get(out, 0, 0), 0);
    CHECK_EQ_INT(event_log_get(out, -1, 0), 0);
    CHECK_EQ_INT(event_log_get(out, 4, -1), 0);
}

void a_short_buffer_is_not_overrun() {
    fresh_boot(1);
    for (uint32_t i = 1; i <= 5; ++i) event_log_record(EVENT_FAN_ON, i);

    // Sentinel past the requested window: event_log_get must not write there.
    event_entry_t out[4];
    std::memset(out, 0, sizeof(out));
    out[3].payload = 0xDEADBEEF;

    CHECK_EQ_INT(event_log_get(out, 3, 0), 3);
    CHECK_EQ_INT(out[3].payload, 0xDEADBEEF);
    CHECK_EQ_INT(out[0].payload, 5);
}

// ------------------------------------------------------------------------
// Ring wrap
// ------------------------------------------------------------------------
void the_ring_drops_the_oldest_entries_when_full() {
    fresh_boot(1);
    // init already recorded SYSTEM_START, so this overshoots capacity by one
    // and the SYSTEM_START must be the entry that falls off.
    for (uint32_t i = 1; i <= EVENT_LOG_MAX_ENTRIES; ++i) {
        event_log_record(EVENT_MODE_CHANGED, i);
    }

    CHECK_EQ_INT(event_log_count(), EVENT_LOG_MAX_ENTRIES);

    std::vector<event_entry_t> all = read_all();
    CHECK_EQ_INT(all.size(), EVENT_LOG_MAX_ENTRIES);
    if (all.size() != (size_t)EVENT_LOG_MAX_ENTRIES) return;

    // Newest first, contiguous, and the SYSTEM_START is gone.
    CHECK_EQ_INT(all.front().payload, EVENT_LOG_MAX_ENTRIES);
    CHECK_EQ_INT(all.back().payload, 1);
    for (size_t i = 0; i < all.size(); ++i) {
        CHECK(all[i].type == EVENT_MODE_CHANGED);
        CHECK_EQ_INT(all[i].payload, EVENT_LOG_MAX_ENTRIES - i);
    }
}

void ordering_survives_wrapping_several_times_over() {
    fresh_boot(1);
    const uint32_t total = EVENT_LOG_MAX_ENTRIES * 3 + 7;
    for (uint32_t i = 1; i <= total; ++i) {
        event_log_record(EVENT_DEFROST_START, i);
    }
    CHECK_EQ_INT(event_log_count(), EVENT_LOG_MAX_ENTRIES);

    std::vector<event_entry_t> all = read_all();
    CHECK_EQ_INT(all.size(), EVENT_LOG_MAX_ENTRIES);
    if (all.size() != (size_t)EVENT_LOG_MAX_ENTRIES) return;
    for (size_t i = 0; i < all.size(); ++i) {
        CHECK_EQ_INT(all[i].payload, total - i);
    }
}

// ------------------------------------------------------------------------
// Clearing
// ------------------------------------------------------------------------
void clearing_empties_the_log_and_bumps_the_revision() {
    fresh_boot(1);
    event_log_record(EVENT_POWER_ON, 1);
    uint32_t before = event_log_revision();

    event_log_clear();

    CHECK_EQ_INT(event_log_count(), 0);
    CHECK(event_log_revision() > before);
    CHECK_EQ_INT(read_all().size(), 0);
}

void the_log_is_usable_again_after_clearing() {
    fresh_boot(1);
    event_log_record(EVENT_POWER_ON, 1);
    event_log_clear();
    event_log_record(EVENT_POWER_OFF, 2);

    std::vector<event_entry_t> all = read_all();
    CHECK_EQ_INT(all.size(), 1);
    if (all.empty()) return;
    CHECK(all[0].type == EVENT_POWER_OFF);
    CHECK_EQ_INT(all[0].payload, 2);
}

// ------------------------------------------------------------------------
// Reset-reason mapping
//
// This is the controller's only record of why it restarted. Getting the
// mapping wrong -- or silently dropping a watchdog reset -- would hide exactly
// the faults #217 exists to make visible.
// ------------------------------------------------------------------------
void abnormal_resets_are_recorded_with_the_right_type() {
    fresh_boot(1);
    struct Case {
        esp_reset_reason_t reason;
        event_type_t expected;
    };
    const Case cases[] = {
        {ESP_RST_BROWNOUT, EVENT_BROWNOUT_RESET},
        {ESP_RST_PANIC, EVENT_APPLICATION_CRASH},
        {ESP_RST_CPU_LOCKUP, EVENT_APPLICATION_CRASH},
        {ESP_RST_INT_WDT, EVENT_WATCHDOG_RESET},
        {ESP_RST_TASK_WDT, EVENT_WATCHDOG_RESET},
        {ESP_RST_WDT, EVENT_WATCHDOG_RESET},
    };

    for (const Case& c : cases) {
        event_log_clear();
        event_log_record_reset_reason(c.reason);
        CHECK_EQ_INT(event_log_count(), 1);
        event_entry_t e = newest();
        CHECK(e.type == c.expected);
        // The watchdog cases carry which watchdog fired; the others do not.
        if (c.expected == EVENT_WATCHDOG_RESET) {
            CHECK_EQ_INT(e.payload, (uint32_t)c.reason);
        } else {
            CHECK_EQ_INT(e.payload, 0);
        }
    }
}

void normal_resets_are_not_recorded() {
    fresh_boot(1);
    const esp_reset_reason_t ignored[] = {ESP_RST_UNKNOWN, ESP_RST_POWERON,
                                          ESP_RST_EXT, ESP_RST_SW,
                                          ESP_RST_DEEPSLEEP, ESP_RST_SDIO};
    for (esp_reset_reason_t r : ignored) {
        event_log_clear();
        event_log_record_reset_reason(r);
        CHECK_EQ_INT(event_log_count(), 0);
    }
}

// ------------------------------------------------------------------------
// Timestamps and the post-NTP backfill
// ------------------------------------------------------------------------
void events_before_the_clock_is_set_have_no_timestamp() {
    time_fake::set(1000);  // long before 2024: the RTC is not set
    fresh_boot(1);
    event_log_record(EVENT_POWER_ON, 1);

    for (const event_entry_t& e : read_all()) {
        CHECK_EQ_INT(e.timestamp, 0);
    }
}

void events_after_the_clock_is_set_are_timestamped() {
    time_fake::set(time_fake::DEFAULT_NOW);
    fresh_boot(1);
    event_log_record(EVENT_POWER_ON, 1);

    for (const event_entry_t& e : read_all()) {
        CHECK_EQ_INT(e.timestamp, (uint32_t)time_fake::DEFAULT_NOW);
    }
}

void a_clock_just_before_the_threshold_still_counts_as_unset() {
    // The boundary itself: 1704067200 is accepted, one second earlier is not.
    time_fake::set(time_fake::SYNC_THRESHOLD - 1);
    fresh_boot(1);
    CHECK_EQ_INT(newest().timestamp, 0);

    time_fake::set(time_fake::SYNC_THRESHOLD);
    event_log_record(EVENT_POWER_ON, 1);
    CHECK_EQ_INT(newest().timestamp, (uint32_t)time_fake::SYNC_THRESHOLD);
}

void the_backfill_derives_timestamps_from_uptime() {
    time_fake::set(1000);  // clock unset for the whole boot
    flash_fake::install_history_partition();
    random_fake::reset(1);
    timer_fake::reset();
    timer_fake::set_us(0);

    event_log_init();               // SYSTEM_START at uptime 0
    timer_fake::advance_ms(5000);
    event_log_record(EVENT_POWER_ON, 1);   // uptime 5000 ms
    timer_fake::advance_ms(3000);
    event_log_record(EVENT_COMPRESSOR_ON, 2);  // uptime 8000 ms

    for (const event_entry_t& e : read_all()) CHECK_EQ_INT(e.timestamp, 0);

    // NTP lands at uptime 10 s, so the device booted at DEFAULT_NOW - 10 s.
    timer_fake::advance_ms(2000);
    struct timeval synced = tv_at(time_fake::DEFAULT_NOW);
    event_log_time_synced(&synced);

    const time_t boot_epoch = time_fake::DEFAULT_NOW - 10;
    std::vector<event_entry_t> all = read_all();
    CHECK_EQ_INT(all.size(), 3);
    if (all.size() != 3) return;
    CHECK_EQ_INT(all[0].timestamp, (uint32_t)(boot_epoch + 8));  // COMPRESSOR_ON
    CHECK_EQ_INT(all[1].timestamp, (uint32_t)(boot_epoch + 5));  // POWER_ON
    CHECK_EQ_INT(all[2].timestamp, (uint32_t)boot_epoch);        // SYSTEM_START
}

void the_backfill_leaves_already_stamped_events_alone() {
    time_fake::set(time_fake::DEFAULT_NOW);
    flash_fake::install_history_partition();
    random_fake::reset(1);
    timer_fake::reset();
    timer_fake::set_us(0);
    event_log_init();
    timer_fake::advance_ms(5000);
    event_log_record(EVENT_POWER_ON, 1);

    const uint32_t original = newest().timestamp;
    CHECK(original != 0);

    // A backfill implying a wildly different wall clock must not rewrite an
    // event that already knew what time it was.
    struct timeval synced = tv_at(time_fake::DEFAULT_NOW + 100000);
    event_log_time_synced(&synced);

    CHECK_EQ_INT(newest().timestamp, original);
}

void the_backfill_only_bumps_the_revision_when_it_changes_something() {
    time_fake::set(1000);
    flash_fake::install_history_partition();
    random_fake::reset(1);
    timer_fake::reset();
    timer_fake::set_us(0);
    event_log_init();
    timer_fake::advance_ms(5000);

    struct timeval synced = tv_at(time_fake::DEFAULT_NOW);
    event_log_time_synced(&synced);
    uint32_t after_first = event_log_revision();

    // Everything is stamped now, so a second sync has nothing to do.
    event_log_time_synced(&synced);
    CHECK_EQ_INT(event_log_revision(), after_first);
}

void a_null_sync_time_is_ignored() {
    time_fake::set(1000);
    fresh_boot(1);
    uint32_t before = event_log_revision();
    event_log_time_synced(nullptr);
    CHECK_EQ_INT(event_log_revision(), before);
    CHECK_EQ_INT(newest().timestamp, 0);
}

void a_backfill_that_would_predate_2024_is_refused() {
    time_fake::set(1000);
    flash_fake::install_history_partition();
    random_fake::reset(1);
    timer_fake::reset();
    timer_fake::set_us(0);
    event_log_init();

    // A bogus "sync" to 1970 would otherwise stamp events with a time that is
    // plainly wrong; leaving them at 0 ("unknown") is the honest answer.
    struct timeval bogus = tv_at(50000);
    event_log_time_synced(&bogus);

    CHECK_EQ_INT(newest().timestamp, 0);
}

// ------------------------------------------------------------------------
// Persistence across reboots
// ------------------------------------------------------------------------
void first_boot_records_two_events() {
    time_fake::set(time_fake::DEFAULT_NOW);
    fresh_boot(1);
    event_log_record(EVENT_POWER_ON, 77);
    CHECK_EQ_INT(event_log_count(), 2);
}

void second_boot_restores_them_and_adds_its_own_start() {
    time_fake::set(time_fake::DEFAULT_NOW);
    boot(999);  // a different seed: a different boot must mean a different id

    // Two restored plus this boot's SYSTEM_START.
    CHECK_EQ_INT(event_log_count(), 3);

    std::vector<event_entry_t> all = read_all();
    CHECK_EQ_INT(all.size(), 3);
    if (all.size() != 3) return;

    const uint32_t this_boot = event_log_current_boot_id();
    CHECK(all[0].type == EVENT_SYSTEM_START);
    CHECK(all[0].boot_id == this_boot);

    CHECK(all[1].type == EVENT_POWER_ON);
    CHECK_EQ_INT(all[1].payload, 77);
    // The restored entries must still be attributed to the boot that made
    // them, or the event log cannot answer "what happened before the reboot".
    CHECK(all[1].boot_id != this_boot);
    CHECK(all[2].type == EVENT_SYSTEM_START);
    CHECK(all[2].boot_id == all[1].boot_id);
}

void first_boot_leaves_unstamped_events_behind() {
    time_fake::set(1000);  // clock never set during this boot
    fresh_boot(1);
    event_log_record(EVENT_POWER_ON, 77);
    for (const event_entry_t& e : read_all()) CHECK_EQ_INT(e.timestamp, 0);
}

void a_backfill_must_not_rewrite_a_previous_boots_events() {
    // The backfill infers wall time from THIS boot's uptime. Applying that to
    // an earlier boot's entries would date them to the wrong day entirely --
    // and since both are unstamped, only the boot_id check prevents it.
    time_fake::set(1000);
    random_fake::reset(999);
    timer_fake::reset();
    timer_fake::set_us(0);
    event_log_init();

    const uint32_t this_boot = event_log_current_boot_id();
    timer_fake::advance_ms(4000);

    struct timeval synced = tv_at(time_fake::DEFAULT_NOW);
    event_log_time_synced(&synced);

    int stamped = 0;
    int left_alone = 0;
    for (const event_entry_t& e : read_all()) {
        if (e.boot_id == this_boot) {
            CHECK(e.timestamp != 0);
            ++stamped;
        } else {
            CHECK_EQ_INT(e.timestamp, 0);
            ++left_alone;
        }
    }
    CHECK_EQ_INT(stamped, 1);      // this boot's SYSTEM_START
    CHECK_EQ_INT(left_alone, 2);   // the previous boot's two events
}

void first_boot_then_disables_persistence() {
    time_fake::set(time_fake::DEFAULT_NOW);
    fresh_boot(1);
    event_log_record(EVENT_POWER_ON, 1);

    // Everything after this point must stay in RAM only: the factory-reset
    // path is about to erase the partition, and a late journal write would
    // resurrect data the user asked to destroy.
    event_log_prepare_factory_reset();
    event_log_record(EVENT_POWER_OFF, 2);

    // Still visible in RAM for the rest of this boot.
    CHECK_EQ_INT(event_log_count(), 3);
}

void the_event_written_after_factory_reset_prep_is_gone() {
    time_fake::set(time_fake::DEFAULT_NOW);
    boot(999);

    // Restored: SYSTEM_START and POWER_ON. Not POWER_OFF. Plus this boot's
    // SYSTEM_START.
    CHECK_EQ_INT(event_log_count(), 3);
    for (const event_entry_t& e : read_all()) {
        CHECK(e.type != EVENT_POWER_OFF);
    }
}

void first_boot_clears_after_factory_reset_prep() {
    // event_log_clear() calls compact_locked() unconditionally, so the ONLY
    // thing stopping a clear from writing to flash after factory-reset prep is
    // the s_storage_ready guard inside compact_locked.
    //
    // This is reachable in production: factory_reset_task() calls
    // event_log_prepare_factory_reset() and then erases the history
    // partition, while DELETE /api/events (api_server.cpp) and the event-log
    // screen's clear button can still fire from another task in that window.
    // Writing the journal there would either resurrect data the user asked to
    // destroy or land on top of an erase in flight.
    time_fake::set(time_fake::DEFAULT_NOW);
    fresh_boot(1);
    event_log_record(EVENT_POWER_ON, 1);

    event_log_prepare_factory_reset();
    event_log_clear();
    CHECK_EQ_INT(event_log_count(), 0);
}

void the_clear_after_prep_never_touched_the_journal() {
    time_fake::set(time_fake::DEFAULT_NOW);
    boot(999);
    // The journal is untouched, so the pre-prep events restore normally --
    // two of them plus this boot's SYSTEM_START. (The real factory reset
    // erases the whole partition immediately afterwards; the point is that
    // event_log did not write to it.)
    CHECK_EQ_INT(event_log_count(), 3);
    int power_on = 0;
    for (const event_entry_t& e : read_all()) {
        if (e.type == EVENT_POWER_ON) ++power_on;
    }
    CHECK_EQ_INT(power_on, 1);
}

void first_boot_records_then_clears() {
    time_fake::set(time_fake::DEFAULT_NOW);
    fresh_boot(1);
    event_log_record(EVENT_POWER_ON, 1);
    event_log_clear();
    CHECK_EQ_INT(event_log_count(), 0);
}

void the_clear_survived_the_reboot() {
    time_fake::set(time_fake::DEFAULT_NOW);
    boot(999);
    // Only this boot's SYSTEM_START: a clear that only emptied RAM would leave
    // the journal intact and the events would come back.
    CHECK_EQ_INT(event_log_count(), 1);
    CHECK(newest().type == EVENT_SYSTEM_START);
}

void first_boot_backfills_timestamps() {
    time_fake::set(1000);
    flash_fake::install_history_partition();
    random_fake::reset(1);
    timer_fake::reset();
    timer_fake::set_us(0);
    event_log_init();
    timer_fake::advance_ms(6000);
    event_log_record(EVENT_POWER_ON, 5);

    struct timeval synced = tv_at(time_fake::DEFAULT_NOW);
    event_log_time_synced(&synced);
    CHECK(newest().timestamp != 0);
}

void the_backfilled_timestamps_survived_the_reboot() {
    // The header claims a backfill reuses the event id so the journal is
    // updated in place rather than appended as a duplicate. If that were
    // broken, the reboot would restore the pre-backfill (zero) timestamps or
    // show the event twice.
    time_fake::set(time_fake::DEFAULT_NOW);
    boot(999);

    int restored_power_on = 0;
    for (const event_entry_t& e : read_all()) {
        if (e.type == EVENT_POWER_ON) {
            ++restored_power_on;
            CHECK(e.timestamp != 0);
        }
    }
    CHECK_EQ_INT(restored_power_on, 1);
}

// ------------------------------------------------------------------------
// Storage failure handling
// ------------------------------------------------------------------------
void the_log_still_works_with_no_history_partition() {
    // No partition installed: history_storage_init fails, persistence is off.
    // The event log must degrade to RAM-only rather than refusing to record --
    // losing the in-RAM log too would blind the diagnostics screen on exactly
    // the units whose flash is in trouble.
    random_fake::reset(1);
    timer_fake::reset();
    event_log_init();

    event_log_record(EVENT_POWER_ON, 1);
    CHECK_EQ_INT(event_log_count(), 2);
    CHECK(newest().type == EVENT_POWER_ON);
}

void a_failed_journal_append_falls_back_to_compaction() {
    time_fake::set(time_fake::DEFAULT_NOW);
    fresh_boot(1);
    event_log_record(EVENT_POWER_ON, 1);

    size_t erased_before = flash_fake::bytes_erased();

    // Break the next journal write. event_log_record should notice and rewrite
    // the whole bank instead of quietly losing the event.
    flash_fake::fail_next(flash_fake::Op::Write, ESP_FAIL, 1);
    event_log_record(EVENT_COMPRESSOR_ON, 2);
    flash_fake::clear_failures();

    // Still present in RAM.
    CHECK_EQ_INT(event_log_count(), 3);
    CHECK(newest().type == EVENT_COMPRESSOR_ON);
    // Compaction erases the other bank before rewriting it; without the
    // fallback nothing would have been erased.
    CHECK(flash_fake::bytes_erased() > erased_before);
}

void the_compacted_events_survive_a_reboot() {
    time_fake::set(time_fake::DEFAULT_NOW);
    boot(999);
    // Three restored (the compaction preserved all of them) plus this boot's
    // SYSTEM_START.
    CHECK_EQ_INT(event_log_count(), 4);
    int compressor = 0;
    for (const event_entry_t& e : read_all()) {
        if (e.type == EVENT_COMPRESSOR_ON) ++compressor;
    }
    CHECK_EQ_INT(compressor, 1);
}

using Phase = void (*)();

// Run one scenario. Each phase executes in its own forked child, so its
// firmware-side statics start cold, while the flash image (shared memory)
// carries over -- a genuine power cycle rather than a re-entrant init() call.
int run_scenario(const char* name, std::vector<Phase> phases) {
    flash_fake::reset();
    time_fake::reset();

    for (size_t i = 0; i < phases.size(); ++i) {
        std::fflush(stdout);
        std::fflush(stderr);
        pid_t pid = fork();
        if (pid == 0) {
            // Every recorded event emits a log line; the wrap tests record
            // thousands. Failures go to stderr, which is left connected.
            std::freopen("/dev/null", "w", stdout);
            g_failures = 0;
            phases[i]();
            std::fflush(stderr);
            _exit(g_failures == 0 ? 0 : 1);
        }
        if (pid < 0) {
            std::fprintf(stderr, "FAIL [%s]: fork failed\n", name);
            return 1;
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFSIGNALED(status)) {
            std::fprintf(stderr, "FAIL [%s] phase %zu: crashed with signal %d\n",
                         name, i + 1, WTERMSIG(status));
            return 1;
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            std::fprintf(stderr, "FAIL [%s] phase %zu\n", name, i + 1);
            return 1;
        }
    }
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    int total = 0;

#define SCENARIO(name, ...)                          \
    do {                                             \
        ++total;                                     \
        failed += run_scenario(name, {__VA_ARGS__}); \
    } while (0)

    // Naming and categorisation
    SCENARIO("every event type has a unique name",
             every_event_type_has_a_unique_non_empty_name);
    SCENARIO("an out-of-range event type is named unknown",
             out_of_range_event_type_is_named_unknown);
    SCENARIO("event names are the documented API strings",
             event_names_are_the_documented_api_strings);
    SCENARIO("every event type maps to a valid category",
             every_event_type_maps_to_a_valid_category);
    SCENARIO("categories match the intended grouping",
             categories_match_the_intended_grouping);
    SCENARIO("category names are stable and bounded",
             category_names_are_stable_and_bounded);

    // Recording
    SCENARIO("recording before init is ignored",
             recording_before_init_is_ignored_not_a_crash);
    SCENARIO("init records a system start", init_records_a_system_start);
    SCENARIO("an invalid event type is rejected", an_invalid_event_type_is_rejected);
    SCENARIO("events come back newest first", events_come_back_newest_first);
    SCENARIO("the revision advances with every record",
             the_revision_advances_with_every_record);
    SCENARIO("every event from one boot shares the boot id",
             every_event_from_one_boot_shares_the_boot_id);
    SCENARIO("a zero boot id is never accepted", a_zero_boot_id_is_never_accepted);

    // Pagination and bounds
    SCENARIO("pagination walks backwards without gaps",
             pagination_walks_backwards_without_gaps_or_repeats);
    SCENARIO("reading past the end returns nothing",
             reading_past_the_end_returns_nothing);
    SCENARIO("invalid get arguments are rejected", invalid_get_arguments_are_rejected);
    SCENARIO("a short buffer is not overrun", a_short_buffer_is_not_overrun);

    // Ring wrap
    SCENARIO("the ring drops the oldest entries when full",
             the_ring_drops_the_oldest_entries_when_full);
    SCENARIO("ordering survives wrapping several times over",
             ordering_survives_wrapping_several_times_over);

    // Clearing
    SCENARIO("clearing empties the log and bumps the revision",
             clearing_empties_the_log_and_bumps_the_revision);
    SCENARIO("the log is usable again after clearing",
             the_log_is_usable_again_after_clearing);

    // Reset reasons
    SCENARIO("abnormal resets are recorded with the right type",
             abnormal_resets_are_recorded_with_the_right_type);
    SCENARIO("normal resets are not recorded", normal_resets_are_not_recorded);

    // Timestamps and backfill
    SCENARIO("events before the clock is set have no timestamp",
             events_before_the_clock_is_set_have_no_timestamp);
    SCENARIO("events after the clock is set are timestamped",
             events_after_the_clock_is_set_are_timestamped);
    SCENARIO("the sync threshold is exact",
             a_clock_just_before_the_threshold_still_counts_as_unset);
    SCENARIO("the backfill derives timestamps from uptime",
             the_backfill_derives_timestamps_from_uptime);
    SCENARIO("the backfill leaves already-stamped events alone",
             the_backfill_leaves_already_stamped_events_alone);
    SCENARIO("the backfill only bumps the revision when it changes something",
             the_backfill_only_bumps_the_revision_when_it_changes_something);
    SCENARIO("a null sync time is ignored", a_null_sync_time_is_ignored);
    SCENARIO("a backfill that would predate 2024 is refused",
             a_backfill_that_would_predate_2024_is_refused);

    // Persistence
    SCENARIO("events survive a reboot", first_boot_records_two_events,
             second_boot_restores_them_and_adds_its_own_start);
    SCENARIO("a backfill must not rewrite a previous boot's events",
             first_boot_leaves_unstamped_events_behind,
             a_backfill_must_not_rewrite_a_previous_boots_events);
    SCENARIO("factory-reset prep stops persisting",
             first_boot_then_disables_persistence,
             the_event_written_after_factory_reset_prep_is_gone);
    SCENARIO("a clear after factory-reset prep does not touch flash",
             first_boot_clears_after_factory_reset_prep,
             the_clear_after_prep_never_touched_the_journal);
    SCENARIO("a clear survives a reboot", first_boot_records_then_clears,
             the_clear_survived_the_reboot);
    SCENARIO("backfilled timestamps survive a reboot",
             first_boot_backfills_timestamps,
             the_backfilled_timestamps_survived_the_reboot);

    // Storage failures
    SCENARIO("the log still works with no history partition",
             the_log_still_works_with_no_history_partition);
    SCENARIO("a failed journal append falls back to compaction",
             a_failed_journal_append_falls_back_to_compaction,
             the_compacted_events_survive_a_reboot);

#undef SCENARIO

    if (failed == 0) {
        std::printf("event_log: %d scenarios passed\n", total);
        std::fprintf(stderr, "event_log: %d scenarios passed\n", total);
        return 0;
    }
    std::fprintf(stderr, "event_log: %d of %d scenarios FAILED\n", failed, total);
    return 1;
}
