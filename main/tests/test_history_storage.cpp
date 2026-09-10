// Native unit tests for main/history_storage.cpp -- the dual-bank event
// journal and the telemetry ring, run against a flash image with real NOR
// semantics.
//
// What existed before: tests/api/test_temperature_history.py, which opens
// history_storage.cpp as TEXT and asserts that certain strings appear in it.
// It would pass with the flash layout completely broken. Everything else was a
// black-box HTTP call to the one physical controller, which cannot erase and
// refill a 2 MB partition per assertion, and certainly cannot lose power in
// the middle of a compaction.
//
// The claims under test are the ones the header makes and nothing verified:
//   * "the old committed bank remains valid until the replacement bank is
//     fully written and committed"  (history_storage.h, replace_events)
//   * records survive a reboot, and a corrupt record is rejected rather than
//     returned as plausible garbage
//   * the telemetry ring wraps without losing the ordering guarantee
//
// HOW A REBOOT IS MODELLED (this matters -- see run_scenario below):
// history_storage.cpp holds its state in file-scope statics and its init()
// early-returns once s_partition is set. So calling init() a second time in
// the same process does nothing, and any "survives a reboot" assertion written
// that way passes vacuously -- it re-reads RAM, never flash. A reboot here is
// therefore a new process (fork, cold statics) over a flash image held in
// shared memory (the medium outlives the process, exactly like real hardware).
//
// Refs #217 (T10, T14).

#include "esp_partition_fake.h"
#include "heap_fake.h"
#include "history_storage.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

#define CHECK_ERR(expr, expected)                                              \
    do {                                                                       \
        esp_err_t e_ = (expr);                                                 \
        if (e_ != (expected)) {                                                \
            std::printf("  FAIL %s:%d: %s -> %s, expected %s\n", __FILE__,     \
                        __LINE__, #expr, esp_err_to_name(e_),                  \
                        esp_err_to_name(expected));                            \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

namespace {

constexpr size_t EVENT_REGION = HISTORY_EVENT_REGION_SIZE;

event_entry_t make_event(uint32_t ts, uint32_t payload) {
    event_entry_t e{};
    e.timestamp = ts;
    e.boot_id = 0xABCD1234;
    e.uptime_ms = ts * 1000ull;
    e.type = (event_type_t)1;
    e.payload = payload;
    return e;
}

history_telemetry_sample_t make_sample(uint32_t ts, uint32_t seq, int16_t inlet) {
    history_telemetry_sample_t s{};
    s.timestamp = ts;
    s.sequence = seq;
    s.inlet_deci_c = inlet;
    s.outlet_deci_c = (int16_t)(inlet + 20);
    s.setpoint_deci_c = 450;
    s.mode = HISTORY_TELEMETRY_MODE_HEATING;
    s.flags = HISTORY_TELEMETRY_CONNECTED | HISTORY_TELEMETRY_INLET_VALID;
    return s;
}

// First boot of a device whose history partition has never been written.
void fresh_device() {
    flash_fake::install_history_partition();
    CHECK_ERR(history_storage_init(), ESP_OK);
}

// A later boot: the image is whatever the previous phase left behind.
void boot() { CHECK_ERR(history_storage_init(), ESP_OK); }

struct LoadResult {
    std::vector<event_entry_t> entries;
    std::vector<uint32_t> ids;
    size_t count = 0;
    uint32_t next_id = 0;
};

LoadResult load_events(size_t capacity = 64) {
    LoadResult r;
    r.entries.resize(capacity);
    r.ids.resize(capacity);
    CHECK_ERR(history_storage_load_events(r.entries.data(), r.ids.data(), capacity,
                                          &r.count, &r.next_id),
              ESP_OK);
    return r;
}

size_t telemetry_count() {
    std::vector<history_telemetry_sample_t> out(4096);
    size_t count = 0;
    CHECK_ERR(history_storage_query_telemetry(0, 0xffffffff, out.data(), out.size(),
                                              &count),
              ESP_OK);
    return count;
}

// ------------------------------------------------------------------------
// Initialisation
// ------------------------------------------------------------------------
void init_requires_the_partition() {
    // No partition installed at all.
    CHECK_ERR(history_storage_init(), ESP_ERR_NOT_FOUND);
}

void init_rejects_a_partition_that_is_too_small() {
    flash_fake::install("history", 0x40, EVENT_REGION - 4096);
    CHECK_ERR(history_storage_init(), ESP_ERR_INVALID_SIZE);
}

void blank_device_yields_an_empty_journal() {
    fresh_device();
    CHECK(load_events().count == 0);
}

void init_formats_the_journal_rather_than_trusting_erased_flash() {
    // A blank device must end up with a committed bank header, otherwise the
    // first append after a power cut would have nothing to attach to.
    fresh_device();
    CHECK(flash_fake::bytes_written() > 0);
    CHECK(!flash_fake::is_erased("history", 0, 32));
}

// ------------------------------------------------------------------------
// Append / reload
// ------------------------------------------------------------------------
void write_two_events() {
    fresh_device();
    event_entry_t a = make_event(1000, 11);
    event_entry_t b = make_event(2000, 22);
    CHECK_ERR(history_storage_append_event(1, &a), ESP_OK);
    CHECK_ERR(history_storage_append_event(2, &b), ESP_OK);
    CHECK(load_events().count == 2);
}

void expect_two_events_after_reboot() {
    boot();
    LoadResult r = load_events();
    CHECK(r.count == 2);
    if (r.count == 2) {
        CHECK(r.entries[0].timestamp == 1000 && r.entries[0].payload == 11);
        CHECK(r.entries[1].timestamp == 2000 && r.entries[1].payload == 22);
        CHECK(r.ids[0] == 1 && r.ids[1] == 2);
    }
}

void events_load_oldest_first() {
    fresh_device();
    for (uint32_t i = 1; i <= 5; ++i) {
        event_entry_t e = make_event(i * 100, i);
        CHECK_ERR(history_storage_append_event(i, &e), ESP_OK);
    }
    LoadResult r = load_events();
    CHECK(r.count == 5);
    for (size_t i = 0; i + 1 < r.count; ++i) {
        CHECK(r.entries[i].timestamp < r.entries[i + 1].timestamp);
    }
}

void write_event_then_backfill_its_timestamp() {
    // The header calls this out: updating an existing event reuses the event
    // ID so a timestamp backfill does not rewrite flash.
    fresh_device();
    event_entry_t e = make_event(0, 7);  // time not synced yet
    CHECK_ERR(history_storage_append_event(42, &e), ESP_OK);
    e.timestamp = 1700000000;            // backfilled after NTP
    CHECK_ERR(history_storage_append_event(42, &e), ESP_OK);
}

void expect_one_backfilled_event_after_reboot() {
    boot();
    LoadResult r = load_events();
    CHECK(r.count == 1);
    if (r.count == 1) {
        CHECK(r.ids[0] == 42);
        CHECK(r.entries[0].timestamp == 1700000000);
    }
}

void write_event_with_id_nine() {
    fresh_device();
    event_entry_t e = make_event(500, 1);
    CHECK_ERR(history_storage_append_event(9, &e), ESP_OK);
}

void expect_next_event_id_past_nine() {
    boot();
    CHECK(load_events().next_id > 9);
}

void load_respects_caller_capacity() {
    fresh_device();
    for (uint32_t i = 1; i <= 10; ++i) {
        event_entry_t e = make_event(i * 10, i);
        CHECK_ERR(history_storage_append_event(i, &e), ESP_OK);
    }
    CHECK(load_events(4).count <= 4);
}

// ------------------------------------------------------------------------
// Corruption
// ------------------------------------------------------------------------
void write_two_events_then_corrupt_the_first_record() {
    fresh_device();
    event_entry_t a = make_event(1000, 11);
    event_entry_t b = make_event(2000, 22);
    CHECK_ERR(history_storage_append_event(1, &a), ESP_OK);
    CHECK_ERR(history_storage_append_event(2, &b), ESP_OK);
    CHECK(load_events().count == 2);

    // Clear bits inside the first record. NOR can only clear bits, so this is
    // a physically possible corruption and the CRC must notice.
    for (size_t off = 4096; off < 4096 + 64; ++off) {
        flash_fake::corrupt_byte("history", off, 0x7f);
    }
}

void expect_corrupt_record_rejected_after_reboot() {
    boot();
    LoadResult r = load_events();
    // The corrupted record must not be handed back as if it were valid.
    CHECK(r.count < 2);
    for (size_t i = 0; i < r.count; ++i) {
        CHECK(r.entries[i].timestamp == 2000);
        CHECK(r.entries[i].payload == 22);
    }
}

void write_an_event_then_wipe_the_journal() {
    fresh_device();
    event_entry_t a = make_event(1000, 11);
    CHECK_ERR(history_storage_append_event(1, &a), ESP_OK);

    // Both bank headers destroyed, as after a bad flash or a factory erase.
    std::vector<uint8_t> blank(EVENT_REGION, 0xff);
    flash_fake::poke("history", 0, blank.data(), blank.size());
}

void expect_wiped_journal_reinitialises_and_is_usable() {
    boot();
    CHECK(load_events().count == 0);  // empty, but not broken
    event_entry_t b = make_event(3000, 33);
    CHECK_ERR(history_storage_append_event(1, &b), ESP_OK);
    CHECK(load_events().count == 1);
}

// ------------------------------------------------------------------------
// Compaction atomicity -- the claim that motivated this whole harness
// ------------------------------------------------------------------------
void compact_eight_events() {
    fresh_device();
    std::vector<event_entry_t> entries;
    std::vector<uint32_t> ids;
    for (uint32_t i = 0; i < 8; ++i) {
        entries.push_back(make_event(1000 + i, i));
        ids.push_back(i + 1);
    }
    CHECK_ERR(history_storage_replace_events(entries.data(), ids.data(),
                                             entries.size(), 0, entries.size()),
              ESP_OK);
    CHECK(load_events().count == 8);
}

void expect_eight_events_after_reboot() {
    boot();
    CHECK(load_events().count == 8);
}

void lose_power_partway_through_a_compaction() {
    // history_storage.h: "The old committed bank remains valid until the
    // replacement bank is fully written and committed."
    //
    // This is THE atomicity claim, and it is unreachable on hardware without
    // cutting power at an exact instant. Here it is one function call -- but a
    // SINGLE cut point is not enough. The first version of this test cut power
    // after exactly 2 flash operations, and a mutant that committed the
    // replacement bank before writing any records into it still passed,
    // because at that one instant the two behaved alike. The cut point is
    // swept by the caller (see HS_POWER_LOSS_AFTER) so every step of the
    // sequence is exercised.
    const char *env = getenv("HS_POWER_LOSS_AFTER");
    const int cut = env ? atoi(env) : 2;

    fresh_device();
    for (uint32_t i = 1; i <= 4; ++i) {
        event_entry_t e = make_event(i * 100, i);
        CHECK_ERR(history_storage_append_event(i, &e), ESP_OK);
    }
    CHECK(load_events().count == 4);

    std::vector<event_entry_t> entries;
    std::vector<uint32_t> ids;
    for (uint32_t i = 0; i < 4; ++i) {
        entries.push_back(make_event(9000 + i, 90 + i));
        ids.push_back(100 + i);
    }

    flash_fake::power_loss_after(cut);
    history_storage_replace_events(entries.data(), ids.data(), entries.size(), 0,
                                   entries.size());
}

void expect_old_bank_intact_after_interrupted_compaction() {
    boot();
    LoadResult r = load_events();

    // The four original events, or the four replacements -- never a mixture,
    // never a truncated journal, never an empty one.
    bool old_intact = (r.count == 4 && r.entries[0].timestamp == 100 &&
                       r.entries[3].timestamp == 400);
    bool new_committed = (r.count == 4 && r.entries[0].payload == 90 &&
                          r.entries[3].payload == 93);
    if (!old_intact && !new_committed) {
        const char *env = getenv("HS_POWER_LOSS_AFTER");
        std::printf("  FAIL: power lost after %s flash op(s): the journal held "
                    "%zu event(s); expected the old 4 or the new 4\n",
                    env ? env : "?", r.count);
        for (size_t i = 0; i < r.count && i < 8; ++i) {
            std::printf("        [%zu] ts=%u payload=%u\n", i,
                        (unsigned)r.entries[i].timestamp,
                        (unsigned)r.entries[i].payload);
        }
        ++g_failures;
    }
}

void fail_the_erase_that_starts_a_compaction() {
    fresh_device();
    for (uint32_t i = 1; i <= 3; ++i) {
        event_entry_t e = make_event(i * 100, i);
        CHECK_ERR(history_storage_append_event(i, &e), ESP_OK);
    }
    std::vector<event_entry_t> entries{make_event(7000, 70)};
    std::vector<uint32_t> ids{55};

    flash_fake::fail_next(flash_fake::Op::Erase, ESP_FAIL, -1);
    esp_err_t err =
        history_storage_replace_events(entries.data(), ids.data(), 1, 0, 1);
    flash_fake::clear_failures();
    CHECK(err != ESP_OK);  // the failure must be reported, not swallowed
}

void expect_three_events_after_failed_compaction() {
    boot();
    CHECK(load_events().count == 3);  // untouched
}

// ------------------------------------------------------------------------
// Telemetry ring
// ------------------------------------------------------------------------
void telemetry_round_trips() {
    fresh_device();
    for (uint32_t i = 0; i < 10; ++i) {
        history_telemetry_sample_t s =
            make_sample(1000 + i * 30, i + 1, (int16_t)(200 + i));
        CHECK_ERR(history_storage_append_telemetry(&s), ESP_OK);
    }
    std::vector<history_telemetry_sample_t> out(64);
    size_t count = 0;
    CHECK_ERR(history_storage_query_telemetry(0, 0xffffffff, out.data(), out.size(),
                                              &count),
              ESP_OK);
    CHECK(count == 10);
    for (size_t i = 0; i + 1 < count; ++i) {
        CHECK(out[i].timestamp <= out[i + 1].timestamp);
    }
}

void write_five_telemetry_samples() {
    fresh_device();
    for (uint32_t i = 0; i < 5; ++i) {
        history_telemetry_sample_t s =
            make_sample(5000 + i * 30, i + 1, (int16_t)(300 + i));
        CHECK_ERR(history_storage_append_telemetry(&s), ESP_OK);
    }
    CHECK(telemetry_count() == 5);
}

void expect_five_telemetry_samples_after_reboot() {
    boot();
    CHECK(telemetry_count() == 5);
}

void telemetry_query_filters_by_time_window() {
    fresh_device();
    for (uint32_t i = 0; i < 20; ++i) {
        history_telemetry_sample_t s =
            make_sample(1000 + i * 30, i + 1, (int16_t)(100 + i));
        CHECK_ERR(history_storage_append_telemetry(&s), ESP_OK);
    }
    std::vector<history_telemetry_sample_t> out(64);
    size_t count = 0;
    CHECK_ERR(history_storage_query_telemetry(1300, 1450, out.data(), out.size(),
                                              &count),
              ESP_OK);
    CHECK(count > 0);
    CHECK(count < 20);  // it really filtered, rather than returning everything
    for (size_t i = 0; i < count; ++i) {
        CHECK(out[i].timestamp >= 1300 && out[i].timestamp <= 1450);
    }
}

void latest_telemetry_timestamp_tracks_the_newest_sample() {
    fresh_device();
    for (uint32_t i = 0; i < 6; ++i) {
        history_telemetry_sample_t s = make_sample(2000 + i * 30, i + 1, 150);
        CHECK_ERR(history_storage_append_telemetry(&s), ESP_OK);
    }
    uint32_t ts = 0;
    CHECK_ERR(history_storage_latest_telemetry_timestamp(&ts), ESP_OK);
    CHECK(ts == 2000 + 5 * 30);
}

void telemetry_ring_wraps_without_losing_ordering() {
    // Fill past one page so the ring has to roll over. The failure this guards
    // against is a wrapped ring returning samples out of order, which renders
    // the history graph as a sawtooth.
    fresh_device();
    const uint32_t n = HISTORY_TELEMETRY_PAGE_CAPACITY + 250;
    for (uint32_t i = 0; i < n; ++i) {
        history_telemetry_sample_t s =
            make_sample(10000 + i * 30, i + 1, (int16_t)(100 + (i % 50)));
        CHECK_ERR(history_storage_append_telemetry(&s), ESP_OK);
    }
    std::vector<history_telemetry_sample_t> out(n + 16);
    size_t count = 0;
    CHECK_ERR(history_storage_query_telemetry(0, 0xffffffff, out.data(), out.size(),
                                              &count),
              ESP_OK);
    CHECK(count > HISTORY_TELEMETRY_PAGE_CAPACITY);
    for (size_t i = 0; i + 1 < count; ++i) {
        CHECK(out[i].timestamp <= out[i + 1].timestamp);
    }
    // The newest sample must still be the newest after wrapping.
    if (count > 0) {
        CHECK(out[count - 1].timestamp == 10000 + (n - 1) * 30);
    }
}

void telemetry_write_failure_is_reported_not_swallowed() {
    fresh_device();
    flash_fake::fail_next(flash_fake::Op::Write, ESP_FAIL, -1);
    history_telemetry_sample_t s = make_sample(4000, 1, 210);
    esp_err_t err = history_storage_append_telemetry(&s);
    flash_fake::clear_failures();
    CHECK(err != ESP_OK);
}

// ------------------------------------------------------------------------
// Shutdown paths
// ------------------------------------------------------------------------
void begin_reboot_stops_accepting_writes() {
    // The comment on history_storage_begin_reboot explains that an in-flight
    // SPI-flash op during esp_restart_noos() faults in ROM. So once it is
    // called, nothing may reach the medium again.
    fresh_device();
    history_telemetry_sample_t s = make_sample(6000, 1, 220);
    CHECK_ERR(history_storage_append_telemetry(&s), ESP_OK);

    size_t before = flash_fake::bytes_written();
    history_storage_begin_reboot();

    history_telemetry_sample_t s2 = make_sample(6030, 2, 221);
    history_storage_append_telemetry(&s2);
    event_entry_t e = make_event(6060, 5);
    history_storage_append_event(77, &e);

    CHECK(flash_fake::bytes_written() == before);
}

void no_leaks_across_a_full_cycle() {
    fresh_device();
    for (uint32_t i = 1; i <= 12; ++i) {
        event_entry_t e = make_event(i * 100, i);
        CHECK_ERR(history_storage_append_event(i, &e), ESP_OK);
    }
    std::vector<event_entry_t> entries;
    std::vector<uint32_t> ids;
    for (uint32_t i = 0; i < 6; ++i) {
        entries.push_back(make_event(20000 + i, i));
        ids.push_back(200 + i);
    }
    CHECK_ERR(history_storage_replace_events(entries.data(), ids.data(),
                                             entries.size(), 0, entries.size()),
              ESP_OK);
    (void)load_events();
    CHECK(heap_fake::outstanding() == 0);
}

using Phase = void (*)();

// Run one scenario. Each phase executes in its own forked child, so its
// firmware-side statics start cold, while the flash image (shared memory)
// carries over -- a genuine power cycle rather than a re-entrant init() call.
int run_scenario(const char *name, std::vector<Phase> phases) {
    flash_fake::reset();
    heap_fake::reset();

    for (size_t i = 0; i < phases.size(); ++i) {
        std::fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            g_failures = 0;
            phases[i]();
            std::fflush(stdout);
            _exit(g_failures == 0 ? 0 : 1);
        }
        if (pid < 0) {
            std::printf("FAIL [%s]: fork failed\n", name);
            return 1;
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFSIGNALED(status)) {
            std::printf("FAIL [%s] phase %zu: crashed with signal %d\n", name, i + 1,
                        WTERMSIG(status));
            return 1;
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            std::printf("FAIL [%s] phase %zu\n", name, i + 1);
            return 1;
        }
    }
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    int total = 0;

#define SCENARIO(name, ...)                              \
    do {                                                 \
        ++total;                                         \
        failed += run_scenario(name, {__VA_ARGS__});     \
    } while (0)

    SCENARIO("init requires the partition", init_requires_the_partition);
    SCENARIO("init rejects an undersized partition",
             init_rejects_a_partition_that_is_too_small);
    SCENARIO("blank device yields an empty journal",
             blank_device_yields_an_empty_journal);
    SCENARIO("init formats the journal",
             init_formats_the_journal_rather_than_trusting_erased_flash);

    SCENARIO("events survive a reboot", write_two_events,
             expect_two_events_after_reboot);
    SCENARIO("events load oldest first", events_load_oldest_first);
    SCENARIO("a timestamp backfill reuses the event id",
             write_event_then_backfill_its_timestamp,
             expect_one_backfilled_event_after_reboot);
    SCENARIO("next event id advances past what is stored",
             write_event_with_id_nine, expect_next_event_id_past_nine);
    SCENARIO("load respects caller capacity", load_respects_caller_capacity);

    SCENARIO("a corrupt record is rejected, not returned",
             write_two_events_then_corrupt_the_first_record,
             expect_corrupt_record_rejected_after_reboot);
    SCENARIO("a wiped journal reinitialises instead of failing",
             write_an_event_then_wipe_the_journal,
             expect_wiped_journal_reinitialises_and_is_usable);

    SCENARIO("compaction moves events into the other bank", compact_eight_events,
             expect_eight_events_after_reboot);

    // Sweep the instant of power loss across the whole compaction sequence.
    // Any single cut point can accidentally agree with a broken
    // implementation; the invariant has to hold at every one of them.
    for (int cut = 0; cut <= 12; ++cut) {
        char value[8];
        std::snprintf(value, sizeof(value), "%d", cut);
        setenv("HS_POWER_LOSS_AFTER", value, 1);
        char name[96];
        std::snprintf(name, sizeof(name),
                      "power loss after %d flash op(s) keeps the old bank", cut);
        SCENARIO(name, lose_power_partway_through_a_compaction,
                 expect_old_bank_intact_after_interrupted_compaction);
    }
    unsetenv("HS_POWER_LOSS_AFTER");

    SCENARIO("a failed erase does not destroy the committed bank",
             fail_the_erase_that_starts_a_compaction,
             expect_three_events_after_failed_compaction);

    SCENARIO("telemetry round trips", telemetry_round_trips);
    SCENARIO("telemetry survives a reboot", write_five_telemetry_samples,
             expect_five_telemetry_samples_after_reboot);
    SCENARIO("telemetry query filters by time window",
             telemetry_query_filters_by_time_window);
    SCENARIO("latest telemetry timestamp tracks the newest sample",
             latest_telemetry_timestamp_tracks_the_newest_sample);
    SCENARIO("telemetry ring wraps without losing ordering",
             telemetry_ring_wraps_without_losing_ordering);
    SCENARIO("telemetry write failure is reported",
             telemetry_write_failure_is_reported_not_swallowed);

    SCENARIO("no leaks across a full cycle", no_leaks_across_a_full_cycle);
    SCENARIO("begin_reboot stops accepting writes",
             begin_reboot_stops_accepting_writes);

#undef SCENARIO

    if (failed) {
        std::printf("%d of %d history_storage scenario(s) failed\n", failed, total);
        return 1;
    }
    std::printf("history_storage: %d scenarios passed\n", total);
    return 0;
}
