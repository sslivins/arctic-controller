// Native unit tests for main/boot_stats.cpp -- the crash-streak counter and
// the SAFE MODE policy it feeds.
//
// This is the recovery policy of last resort. If an optional subsystem starts
// crashing during startup, safe mode is the only thing that stops the device
// looping forever: after three consecutive crash reboots it disables the
// optional paths (demo mode and friends) so the unit comes up far enough to be
// reached and repaired. Nothing else in the system can rescue a controller
// that panics before its network stack is usable.
//
// Epic #217 lists T11/T12 (factory reset and crash-streak/safe-mode policy) as
// "destructive, needs care on the shared device", and the destructive half
// genuinely is: you cannot ask the one physical controller to panic three times
// in a row on demand, and you certainly cannot do it on every PR. But the
// policy half is pure logic over two NVS counters, which is exactly the kind of
// item T13 exists to convert from "needs the device" into "runs in CI in
// seconds". This covers that half; the destructive factory-reset step is
// deliberately still out of scope here.
//
// How a "previous boot" is modelled: the ONLY state carried across a reboot is
// the two u32s in the boot_stats NVS namespace. Seeding them is therefore
// exactly equivalent to having rebooted with that history, and it lets each
// transition be asserted on its own instead of through a fragile chain. Each
// scenario still runs in a forked child, because boot_stats_init() is
// single-shot per process (the `initialized` guard) -- one child is one boot.
//
// Refs #217 (T12).

#include "esp_system.h"
#include "nvs_fake.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "boot_stats.h"

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                           \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

#define CHECK_EQ_INT(actual, expected)                                      \
    do {                                                                    \
        long a_ = (long)(actual);                                           \
        long e_ = (long)(expected);                                         \
        if (a_ != e_) {                                                     \
            std::fprintf(stderr, "  FAIL %s:%d: %s -> %ld, expected %ld\n", \
                         __FILE__, __LINE__, #actual, a_, e_);              \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

#define CHECK_EQ_STR(actual, expected)                                       \
    do {                                                                     \
        const char* a_ = (actual);                                           \
        const char* e_ = (expected);                                         \
        if (a_ == nullptr || std::strcmp(a_, e_) != 0) {                     \
            std::fprintf(stderr, "  FAIL %s:%d: %s -> \"%s\", expected \"%s\"\n", \
                         __FILE__, __LINE__, #actual, a_ ? a_ : "(null)", e_); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

namespace {

// Must match boot_stats.cpp. Duplicated deliberately: if either the namespace
// or a key name changes, every persisted counter on every deployed device
// silently resets to zero, and the tests should fail loudly rather than follow
// the rename.
constexpr const char* NS = "boot_stats";
constexpr const char* KEY_BROWNOUT = "bo_count";
constexpr const char* KEY_STREAK = "panic_strk";
constexpr uint32_t SAFE_MODE_THRESHOLD = 3;

// A previous boot history: whatever the last boot left in NVS.
void previously(uint32_t brownouts, uint32_t streak) {
    nvs_fake::reset();
    nvs_fake::seed_u32(NS, KEY_BROWNOUT, brownouts);
    nvs_fake::seed_u32(NS, KEY_STREAK, streak);
}

void blank_device() { nvs_fake::reset(); }

uint32_t stored(const char* key) {
    uint32_t v = 0xFFFFFFFFu;
    if (!nvs_fake::peek_u32(NS, key, &v)) return 0xFFFFFFFFu;
    return v;
}

// ------------------------------------------------------------------------
// A first boot on a device with no history
// ------------------------------------------------------------------------
void a_blank_device_starts_clean() {
    blank_device();
    boot_stats_init(ESP_RST_POWERON);

    CHECK_EQ_INT(boot_stats_brownout_count(), 0);
    CHECK_EQ_INT(boot_stats_panic_streak(), 0);
    CHECK(boot_stats_in_safe_mode() == false);
    CHECK_EQ_INT(boot_stats_last_reset_reason(), ESP_RST_POWERON);
}

void a_clean_boot_writes_nothing() {
    blank_device();
    boot_stats_init(ESP_RST_POWERON);

    // A normal boot must not touch flash. This runs on every single startup;
    // an unconditional write would burn an NVS page per boot for no reason.
    CHECK_EQ_INT(nvs_fake::call_count(nvs_fake::Op::SetU32), 0);
    CHECK_EQ_INT(nvs_fake::call_count(nvs_fake::Op::Commit), 0);
}

// ------------------------------------------------------------------------
// Brownouts
// ------------------------------------------------------------------------
void a_brownout_is_counted_and_persisted() {
    previously(4, 0);
    boot_stats_init(ESP_RST_BROWNOUT);

    CHECK_EQ_INT(boot_stats_brownout_count(), 5);
    // Persisted, not just held in RAM: the whole point is that it outlives the
    // reboot it is counting.
    CHECK_EQ_INT(stored(KEY_BROWNOUT), 5);
    CHECK(nvs_fake::call_count(nvs_fake::Op::Commit) > 0);
}

void a_brownout_is_not_a_crash() {
    previously(0, 0);
    boot_stats_init(ESP_RST_BROWNOUT);

    // A sagging supply is an environment problem, not a crashing firmware.
    // Counting it toward the crash streak would drop a unit on a weak PSU into
    // safe mode and disable working features for the wrong reason.
    CHECK_EQ_INT(boot_stats_panic_streak(), 0);
    CHECK(boot_stats_in_safe_mode() == false);
}

void a_non_brownout_boot_leaves_the_brownout_count_alone() {
    previously(7, 0);
    boot_stats_init(ESP_RST_SW);

    CHECK_EQ_INT(boot_stats_brownout_count(), 7);
    CHECK_EQ_INT(stored(KEY_BROWNOUT), 7);
}

// ------------------------------------------------------------------------
// What counts as a crash
// ------------------------------------------------------------------------
void a_panic_increments_the_streak() {
    previously(0, 0);
    boot_stats_init(ESP_RST_PANIC);
    CHECK_EQ_INT(boot_stats_panic_streak(), 1);
    CHECK_EQ_INT(stored(KEY_STREAK), 1);
    CHECK(boot_stats_in_safe_mode() == false);
}

void a_task_watchdog_reset_increments_the_streak() {
    previously(0, 0);
    boot_stats_init(ESP_RST_TASK_WDT);
    CHECK_EQ_INT(boot_stats_panic_streak(), 1);
}

void an_interrupt_watchdog_reset_increments_the_streak() {
    previously(0, 0);
    boot_stats_init(ESP_RST_INT_WDT);
    CHECK_EQ_INT(boot_stats_panic_streak(), 1);
}

void an_other_watchdog_reset_increments_the_streak() {
    previously(0, 0);
    boot_stats_init(ESP_RST_WDT);
    // This is the reason an HP_WDT_RESET surfaces as (see #210), so it has to
    // count -- a wedge that only ever reports ESP_RST_WDT would otherwise
    // never trip safe mode no matter how many times it looped.
    CHECK_EQ_INT(boot_stats_panic_streak(), 1);
}

void a_deliberate_software_reset_is_not_a_crash() {
    previously(0, 1);
    boot_stats_init(ESP_RST_SW);
    // An OTA reboot or a user-requested restart must not push the device
    // toward safe mode.
    CHECK_EQ_INT(boot_stats_panic_streak(), 1);
    CHECK_EQ_INT(stored(KEY_STREAK), 1);
}

void an_external_reset_is_not_a_crash() {
    previously(0, 1);
    boot_stats_init(ESP_RST_EXT);
    CHECK_EQ_INT(boot_stats_panic_streak(), 1);
}

void a_power_cycle_does_not_clear_the_streak() {
    previously(0, 2);
    boot_stats_init(ESP_RST_POWERON);

    // This is the documented behaviour and the interesting one: pulling the
    // plug is the first thing anyone tries on a wedged unit, and if that reset
    // the streak the device would never reach safe mode -- the exact scenario
    // safe mode exists for would be the one that defeats it.
    CHECK_EQ_INT(boot_stats_panic_streak(), 2);
    CHECK_EQ_INT(stored(KEY_STREAK), 2);
    CHECK(boot_stats_in_safe_mode() == false);
}

// ------------------------------------------------------------------------
// The safe-mode threshold
// ------------------------------------------------------------------------
void two_consecutive_crashes_do_not_trip_safe_mode() {
    previously(0, 1);
    boot_stats_init(ESP_RST_PANIC);
    CHECK_EQ_INT(boot_stats_panic_streak(), 2);
    CHECK(boot_stats_in_safe_mode() == false);
}

void the_third_consecutive_crash_trips_safe_mode() {
    previously(0, 2);
    boot_stats_init(ESP_RST_PANIC);
    CHECK_EQ_INT(boot_stats_panic_streak(), SAFE_MODE_THRESHOLD);
    CHECK(boot_stats_in_safe_mode() == true);
}

void safe_mode_persists_beyond_the_threshold() {
    previously(0, 9);
    boot_stats_init(ESP_RST_TASK_WDT);
    CHECK_EQ_INT(boot_stats_panic_streak(), 10);
    // The comparison is >=, not ==: a device that crashed its way well past
    // the threshold must stay in safe mode, not fall back out of it.
    CHECK(boot_stats_in_safe_mode() == true);
}

void a_clean_boot_after_a_long_streak_still_enters_safe_mode() {
    previously(0, SAFE_MODE_THRESHOLD);
    boot_stats_init(ESP_RST_POWERON);

    // The streak is not incremented (this boot was clean) but it is still at
    // the threshold, so the device is still in a crash loop as far as anyone
    // knows: it has not yet proven healthy. Coming up with optional subsystems
    // enabled here would resume the loop.
    CHECK_EQ_INT(boot_stats_panic_streak(), SAFE_MODE_THRESHOLD);
    CHECK(boot_stats_in_safe_mode() == true);
}

// ------------------------------------------------------------------------
// Proving the device healthy again
// ------------------------------------------------------------------------
void a_healthy_boot_clears_the_streak() {
    previously(0, 2);
    boot_stats_init(ESP_RST_PANIC);
    CHECK_EQ_INT(boot_stats_panic_streak(), 3);

    CHECK(boot_stats_note_healthy() == true);
    CHECK_EQ_INT(boot_stats_panic_streak(), 0);
    // Cleared in flash, so the NEXT boot starts from zero. Clearing only the
    // RAM copy would leave the device permanently one crash from safe mode.
    CHECK_EQ_INT(stored(KEY_STREAK), 0);
}

void note_healthy_does_not_leave_safe_mode_this_boot() {
    previously(0, 2);
    boot_stats_init(ESP_RST_PANIC);
    CHECK(boot_stats_in_safe_mode() == true);

    CHECK(boot_stats_note_healthy() == true);
    // Safe mode is fixed for the boot: subsystems were already skipped during
    // startup, so flipping the flag now would report a state the running
    // firmware is not actually in.
    CHECK(boot_stats_in_safe_mode() == true);
}

void note_healthy_on_an_already_clean_device_writes_nothing() {
    previously(0, 0);
    boot_stats_init(ESP_RST_POWERON);

    nvs_fake::reset_volatile();
    CHECK(boot_stats_note_healthy() == true);
    // Called periodically once the device is up; writing every time would
    // wear the NVS partition for nothing.
    CHECK_EQ_INT(nvs_fake::call_count(nvs_fake::Op::SetU32), 0);
}

void note_healthy_reports_failure_when_nvs_cannot_be_opened() {
    previously(0, 3);
    boot_stats_init(ESP_RST_PANIC);
    CHECK_EQ_INT(boot_stats_panic_streak(), 4);

    nvs_fake::fail_next(nvs_fake::Op::Open, ESP_FAIL);
    CHECK(boot_stats_note_healthy() == false);
    nvs_fake::clear_failures();

    // The RAM value must survive: reporting success, or zeroing the RAM copy,
    // would make the caller stop retrying while flash still says the device is
    // in a crash loop. Then a single later crash re-enters safe mode.
    CHECK_EQ_INT(boot_stats_panic_streak(), 4);
    // Still 4 in flash: this boot's own increment was already persisted by
    // init(). The failed clear must leave that untouched so the next boot
    // still sees the crash history.
    CHECK_EQ_INT(stored(KEY_STREAK), 4);
}

void note_healthy_reports_failure_when_the_write_fails() {
    previously(0, 3);
    boot_stats_init(ESP_RST_PANIC);

    nvs_fake::fail_next(nvs_fake::Op::SetU32, ESP_FAIL);
    CHECK(boot_stats_note_healthy() == false);
    nvs_fake::clear_failures();

    CHECK_EQ_INT(boot_stats_panic_streak(), 4);
    CHECK_EQ_INT(stored(KEY_STREAK), 4);
}

void note_healthy_reports_failure_when_the_commit_fails() {
    previously(0, 3);
    boot_stats_init(ESP_RST_PANIC);

    // The set can succeed and the commit still fail; the value is not durable
    // until the commit lands, so this must be treated as a failure too.
    nvs_fake::fail_next(nvs_fake::Op::Commit, ESP_FAIL);
    CHECK(boot_stats_note_healthy() == false);
    nvs_fake::clear_failures();

    CHECK_EQ_INT(boot_stats_panic_streak(), 4);
}

void a_retry_after_a_failed_persist_succeeds() {
    previously(0, 3);
    boot_stats_init(ESP_RST_PANIC);

    nvs_fake::fail_next(nvs_fake::Op::Open, ESP_FAIL);
    CHECK(boot_stats_note_healthy() == false);
    nvs_fake::clear_failures();

    // The contract is "the caller should retry later", so retrying has to
    // actually work rather than leaving the module wedged.
    CHECK(boot_stats_note_healthy() == true);
    CHECK_EQ_INT(boot_stats_panic_streak(), 0);
    CHECK_EQ_INT(stored(KEY_STREAK), 0);
}

// ------------------------------------------------------------------------
// NVS unavailable at boot
// ------------------------------------------------------------------------
void an_unopenable_nvs_does_not_prevent_booting() {
    previously(2, 5);
    nvs_fake::fail_next(nvs_fake::Op::Open, ESP_FAIL);
    boot_stats_init(ESP_RST_PANIC);
    nvs_fake::clear_failures();

    // A corrupt or full NVS partition must not brick startup, and it must not
    // invent a crash streak either.
    CHECK_EQ_INT(boot_stats_brownout_count(), 0);
    CHECK_EQ_INT(boot_stats_panic_streak(), 0);
    CHECK(boot_stats_in_safe_mode() == false);
    CHECK_EQ_INT(boot_stats_last_reset_reason(), ESP_RST_PANIC);
}

void a_failed_streak_write_still_reports_the_streak_this_boot() {
    previously(0, 2);
    nvs_fake::fail_next(nvs_fake::Op::SetU32, ESP_FAIL, -1);
    boot_stats_init(ESP_RST_PANIC);
    nvs_fake::clear_failures();

    // The increment could not be persisted, but this boot IS the third crash,
    // so safe mode must still engage now. Deferring it because flash is
    // unwritable would disable the protection exactly when the device is in
    // the worst shape.
    CHECK_EQ_INT(boot_stats_panic_streak(), 3);
    CHECK(boot_stats_in_safe_mode() == true);
}

// ------------------------------------------------------------------------
// init is single-shot
// ------------------------------------------------------------------------
void a_second_init_is_ignored() {
    previously(0, 0);
    boot_stats_init(ESP_RST_PANIC);
    CHECK_EQ_INT(boot_stats_panic_streak(), 1);

    // Calling init twice must not double-count the same reset.
    boot_stats_init(ESP_RST_PANIC);
    CHECK_EQ_INT(boot_stats_panic_streak(), 1);
    CHECK_EQ_INT(stored(KEY_STREAK), 1);
    CHECK_EQ_INT(boot_stats_last_reset_reason(), ESP_RST_PANIC);
}

// ------------------------------------------------------------------------
// Clearing
// ------------------------------------------------------------------------
void clear_zeroes_both_counters_in_flash() {
    previously(6, 2);
    boot_stats_init(ESP_RST_BROWNOUT);
    CHECK_EQ_INT(boot_stats_brownout_count(), 7);

    boot_stats_clear();
    CHECK_EQ_INT(boot_stats_brownout_count(), 0);
    CHECK_EQ_INT(boot_stats_panic_streak(), 0);
    CHECK_EQ_INT(stored(KEY_BROWNOUT), 0);
    CHECK_EQ_INT(stored(KEY_STREAK), 0);
}

// ------------------------------------------------------------------------
// Reason names
// ------------------------------------------------------------------------
void every_reset_reason_has_a_distinct_name() {
    // These strings reach the event log and the diagnostics API, so they are
    // the vocabulary a post-mortem is written in. A duplicate or a wrong
    // mapping makes a crash report describe the wrong failure.
    struct { esp_reset_reason_t reason; const char* name; } expected[] = {
        {ESP_RST_POWERON,    "POWER_ON"},
        {ESP_RST_EXT,        "EXTERNAL"},
        {ESP_RST_SW,         "SOFTWARE"},
        {ESP_RST_PANIC,      "PANIC"},
        {ESP_RST_INT_WDT,    "INTERRUPT_WDT"},
        {ESP_RST_TASK_WDT,   "TASK_WDT"},
        {ESP_RST_WDT,        "OTHER_WDT"},
        {ESP_RST_DEEPSLEEP,  "DEEP_SLEEP"},
        {ESP_RST_BROWNOUT,   "BROWNOUT"},
        {ESP_RST_SDIO,       "SDIO"},
        {ESP_RST_USB,        "USB"},
        {ESP_RST_JTAG,       "JTAG"},
        {ESP_RST_EFUSE,      "EFUSE_ERROR"},
        {ESP_RST_PWR_GLITCH, "POWER_GLITCH"},
        {ESP_RST_CPU_LOCKUP, "CPU_LOCKUP"},
    };
    for (const auto& e : expected) {
        CHECK_EQ_STR(boot_stats_reset_reason_name(e.reason), e.name);
    }

    // Distinctness, so a copy-paste in the switch cannot go unnoticed.
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        for (size_t j = i + 1; j < sizeof(expected) / sizeof(expected[0]); ++j) {
            CHECK(std::strcmp(expected[i].name, expected[j].name) != 0);
        }
    }

    CHECK_EQ_STR(boot_stats_reset_reason_name(ESP_RST_UNKNOWN), "UNKNOWN");
    CHECK_EQ_STR(boot_stats_reset_reason_name((esp_reset_reason_t)999), "UNKNOWN");
}

// ------------------------------------------------------------------------
// Harness: one forked child per scenario, because boot_stats_init() is
// single-shot per process -- one child is one boot.
// ------------------------------------------------------------------------
int run_scenario(const char* name, void (*body)()) {
    std::fflush(nullptr);
    pid_t pid = fork();
    if (pid == 0) {
        g_failures = 0;
        body();
        std::fflush(nullptr);
        _exit(g_failures == 0 ? 0 : 1);
    }
    if (pid < 0) {
        std::fprintf(stderr, "FAIL [%s]: fork failed\n", name);
        return 1;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
        std::fprintf(stderr, "FAIL [%s]: crashed with signal %d\n", name,
                     WTERMSIG(status));
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::fprintf(stderr, "FAIL [%s]\n", name);
        return 1;
    }
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    int total = 0;

#define SCENARIO(name, fn)              \
    do {                                \
        ++total;                        \
        failed += run_scenario(name, fn); \
    } while (0)

    SCENARIO("a blank device starts clean", a_blank_device_starts_clean);
    SCENARIO("a clean boot writes nothing", a_clean_boot_writes_nothing);

    SCENARIO("a brownout is counted and persisted",
             a_brownout_is_counted_and_persisted);
    SCENARIO("a brownout is not a crash", a_brownout_is_not_a_crash);
    SCENARIO("a non-brownout boot leaves the brownout count alone",
             a_non_brownout_boot_leaves_the_brownout_count_alone);

    SCENARIO("a panic increments the streak", a_panic_increments_the_streak);
    SCENARIO("a task watchdog reset increments the streak",
             a_task_watchdog_reset_increments_the_streak);
    SCENARIO("an interrupt watchdog reset increments the streak",
             an_interrupt_watchdog_reset_increments_the_streak);
    SCENARIO("an other watchdog reset increments the streak",
             an_other_watchdog_reset_increments_the_streak);
    SCENARIO("a deliberate software reset is not a crash",
             a_deliberate_software_reset_is_not_a_crash);
    SCENARIO("an external reset is not a crash", an_external_reset_is_not_a_crash);
    SCENARIO("a power cycle does not clear the streak",
             a_power_cycle_does_not_clear_the_streak);

    SCENARIO("two consecutive crashes do not trip safe mode",
             two_consecutive_crashes_do_not_trip_safe_mode);
    SCENARIO("the third consecutive crash trips safe mode",
             the_third_consecutive_crash_trips_safe_mode);
    SCENARIO("safe mode persists beyond the threshold",
             safe_mode_persists_beyond_the_threshold);
    SCENARIO("a clean boot after a long streak still enters safe mode",
             a_clean_boot_after_a_long_streak_still_enters_safe_mode);

    SCENARIO("a healthy boot clears the streak", a_healthy_boot_clears_the_streak);
    SCENARIO("note_healthy does not leave safe mode this boot",
             note_healthy_does_not_leave_safe_mode_this_boot);
    SCENARIO("note_healthy on an already clean device writes nothing",
             note_healthy_on_an_already_clean_device_writes_nothing);
    SCENARIO("note_healthy reports failure when nvs cannot be opened",
             note_healthy_reports_failure_when_nvs_cannot_be_opened);
    SCENARIO("note_healthy reports failure when the write fails",
             note_healthy_reports_failure_when_the_write_fails);
    SCENARIO("note_healthy reports failure when the commit fails",
             note_healthy_reports_failure_when_the_commit_fails);
    SCENARIO("a retry after a failed persist succeeds",
             a_retry_after_a_failed_persist_succeeds);

    SCENARIO("an unopenable nvs does not prevent booting",
             an_unopenable_nvs_does_not_prevent_booting);
    SCENARIO("a failed streak write still reports the streak this boot",
             a_failed_streak_write_still_reports_the_streak_this_boot);

    SCENARIO("a second init is ignored", a_second_init_is_ignored);
    SCENARIO("clear zeroes both counters in flash",
             clear_zeroes_both_counters_in_flash);
    SCENARIO("every reset reason has a distinct name",
             every_reset_reason_has_a_distinct_name);

#undef SCENARIO

    if (failed == 0) {
        std::printf("boot_stats: %d scenarios passed\n", total);
        std::fprintf(stderr, "boot_stats: %d scenarios passed\n", total);
        return 0;
    }
    std::fprintf(stderr, "boot_stats: %d of %d scenarios FAILED\n", failed, total);
    return 1;
}
