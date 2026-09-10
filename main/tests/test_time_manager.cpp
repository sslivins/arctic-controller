// Native unit tests for main/time_manager.cpp -- NTP synchronisation state,
// timezone persistence and the clock-validity guard.
//
// #217 singles out the existing coverage here as weaker than its name
// suggests: "test_events_and_misc_api.py::TestTimeSync::test_time_sync_
// returns_200 -- proves command acceptance; not that NTP succeeded or the
// clock moved." That gap is structural rather than an oversight. On the
// physical controller you cannot make an NTP server reply on demand, cannot
// make it reply with a chosen timestamp, and certainly cannot make it lie.
// So the interesting half of this module -- what happens when the reply
// arrives, and what happens when the reply is wrong -- has never been
// exercised anywhere.
//
// Here the SNTP fake delivers the callback itself, so the tests can assert the
// thing that actually matters: the device only believes its clock once a
// plausible time has arrived.
//
// Why the clock-validity guard is worth this much attention: MIN_VALID_EPOCH
// is what stops a controller with an unset clock from stamping the event log
// and the telemetry journal with 1970 timestamps. Those records are the
// evidence used to diagnose field failures, and a boot that wrongly declares
// itself synced poisons them permanently -- there is no later correction, and
// nothing in the system notices.
//
// time() is overridden (time_fake) so "the clock is not set yet" is reachable
// at all; on the host the real clock is always plausibly current.
//
// Refs #217 (T13).

#include "freertos/event_groups.h"
#include "esp_sntp.h"
#include "nvs_fake.h"
#include "time_fake.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "time_manager.h"

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

// Must match time_manager.cpp. Duplicated deliberately: renaming the namespace
// or a key silently discards every deployed device's saved timezone, and that
// should fail a test rather than follow the rename.
constexpr const char* NS = "time_cfg";
constexpr const char* KEY_TZ = "timezone";
constexpr const char* KEY_24H = "format_24h";
constexpr const char* DEFAULT_TZ = "EST5EDT,M3.2.0,M11.1.0";

// 2024-01-01T00:00:00Z, the threshold below which the module refuses to treat
// the clock as set.
constexpr time_t MIN_VALID_EPOCH = 1704067200;

void blank_device() {
    nvs_fake::reset();
    sntp_fake::reset();
    event_group_fake::reset();
    time_fake::reset();
}

std::string stored_tz() {
    std::string s;
    if (!nvs_fake::peek_str(NS, KEY_TZ, &s)) return std::string("<absent>");
    return s;
}

// ------------------------------------------------------------------------
// Initialisation and timezone loading
// ------------------------------------------------------------------------
void a_blank_device_uses_the_default_timezone() {
    blank_device();
    time_mgr_init();
    CHECK_EQ_STR(time_mgr_get_timezone(), DEFAULT_TZ);
    // Applied to the process, not merely remembered: everything downstream
    // formats through localtime_r.
    CHECK_EQ_STR(getenv("TZ"), DEFAULT_TZ);
}

void a_saved_timezone_is_restored() {
    blank_device();
    nvs_fake::seed_str(NS, KEY_TZ, "PST8PDT,M3.2.0,M11.1.0");
    time_mgr_init();
    CHECK_EQ_STR(time_mgr_get_timezone(), "PST8PDT,M3.2.0,M11.1.0");
    CHECK_EQ_STR(getenv("TZ"), "PST8PDT,M3.2.0,M11.1.0");
}

void an_unreadable_nvs_falls_back_to_the_default() {
    blank_device();
    nvs_fake::seed_str(NS, KEY_TZ, "PST8PDT,M3.2.0,M11.1.0");
    nvs_fake::fail_next(nvs_fake::Op::Open, ESP_FAIL, -1);
    time_mgr_init();
    nvs_fake::clear_failures();

    // A device whose NVS will not open must still keep time in SOME zone
    // rather than an empty TZ string, which glibc reads as UTC and which would
    // silently shift every displayed and logged timestamp.
    CHECK_EQ_STR(time_mgr_get_timezone(), DEFAULT_TZ);
}

void the_default_survives_a_missing_key() {
    blank_device();
    // Namespace exists (another key present) but the timezone was never saved.
    nvs_fake::seed_u8(NS, KEY_24H, 1);
    time_mgr_init();
    CHECK_EQ_STR(time_mgr_get_timezone(), DEFAULT_TZ);
}

void init_is_single_shot() {
    blank_device();
    time_mgr_init();
    CHECK_EQ_INT(event_group_fake::created(), 1);

    nvs_fake::seed_str(NS, KEY_TZ, "UTC0");
    time_mgr_init();
    // The second call must not re-read NVS or allocate a second event group;
    // the first group's TIME_VALID_BIT would be orphaned and every later
    // is_synced() would read false forever.
    CHECK_EQ_STR(time_mgr_get_timezone(), DEFAULT_TZ);
    CHECK_EQ_INT(event_group_fake::created(), 1);
}

void a_failed_event_group_allocation_does_not_complete_init() {
    blank_device();
    event_group_fake::fail_next_create();
    time_mgr_init();

    // Out of internal RAM (see #247). init returns early WITHOUT marking
    // itself initialised, so a later retry can still succeed.
    CHECK(time_mgr_is_synced() == false);

    time_mgr_init();
    CHECK_EQ_INT(event_group_fake::created(), 1);
    CHECK_EQ_STR(time_mgr_get_timezone(), DEFAULT_TZ);
}

// ------------------------------------------------------------------------
// The 24-hour format preference
// ------------------------------------------------------------------------
void the_format_defaults_to_24_hour() {
    blank_device();
    time_mgr_init();
    CHECK(time_mgr_get_24h_format() == true);
}

void a_saved_12_hour_preference_is_restored() {
    blank_device();
    nvs_fake::seed_u8(NS, KEY_24H, 0);
    time_mgr_init();
    CHECK(time_mgr_get_24h_format() == false);
}

void setting_the_format_persists_it() {
    blank_device();
    time_mgr_init();
    time_mgr_set_24h_format(false);

    CHECK(time_mgr_get_24h_format() == false);
    uint8_t v = 0xff;
    CHECK(nvs_fake::peek_u8(NS, KEY_24H, &v) == true);
    CHECK_EQ_INT(v, 0);
    CHECK(nvs_fake::call_count(nvs_fake::Op::Commit) > 0);
}

void the_format_survives_an_unwritable_nvs() {
    blank_device();
    time_mgr_init();
    nvs_fake::fail_next(nvs_fake::Op::Open, ESP_FAIL, -1);
    time_mgr_set_24h_format(false);
    nvs_fake::clear_failures();

    // The preference applies to the running UI even if it could not be saved.
    CHECK(time_mgr_get_24h_format() == false);
}

// ------------------------------------------------------------------------
// Setting the timezone
// ------------------------------------------------------------------------
void setting_a_timezone_applies_and_persists_it() {
    blank_device();
    time_mgr_init();
    time_mgr_set_timezone("MST7MDT,M3.2.0,M11.1.0");

    CHECK_EQ_STR(time_mgr_get_timezone(), "MST7MDT,M3.2.0,M11.1.0");
    CHECK_EQ_STR(getenv("TZ"), "MST7MDT,M3.2.0,M11.1.0");
    CHECK(stored_tz() == "MST7MDT,M3.2.0,M11.1.0");
}

void a_null_timezone_is_ignored() {
    blank_device();
    time_mgr_init();
    time_mgr_set_timezone(nullptr);
    CHECK_EQ_STR(time_mgr_get_timezone(), DEFAULT_TZ);
    CHECK(stored_tz() == "<absent>");
}

void the_timezone_actually_changes_the_local_time() {
    blank_device();
    time_mgr_init();

    // A fixed instant, so the only variable is the zone. 2025-07-01 12:00 UTC
    // is inside DST for both zones below.
    time_fake::set(1751371200);

    time_mgr_set_timezone("UTC0");
    struct tm utc {};
    CHECK(time_mgr_get_local_time(&utc) == true);

    time_mgr_set_timezone("EST5EDT,M3.2.0,M11.1.0");
    struct tm eastern {};
    CHECK(time_mgr_get_local_time(&eastern) == true);

    // Eastern daylight time is UTC-4. If tzset() were not called, or the zone
    // were merely stored and not applied, both would read identically and
    // every displayed timestamp would be wrong by hours.
    int utc_h = utc.tm_hour;
    int est_h = eastern.tm_hour;
    CHECK_EQ_INT(((utc_h - est_h) + 24) % 24, 4);
}

void a_timezone_too_long_for_the_buffer_is_truncated_consistently() {
    blank_device();
    time_mgr_init();

    // 64-byte buffer in the module. A longer string must not overflow it, and
    // -- the part that used to be wrong -- what is persisted must be what is
    // running. Saving the caller's full string while RAM holds a truncated
    // copy means the zone silently changes on the next boot, when the load
    // path truncates it differently or refuses it outright.
    std::string long_tz(200, 'X');
    time_mgr_set_timezone(long_tz.c_str());

    const char* got = time_mgr_get_timezone();
    CHECK(got != nullptr);
    CHECK_EQ_INT(std::strlen(got), 63);
    CHECK(stored_tz() == std::string(got));
}

// ------------------------------------------------------------------------
// Starting and stopping NTP
// ------------------------------------------------------------------------
void sync_does_not_start_before_init() {
    blank_device();
    time_mgr_start_sync();

    // SNTP configuration touches the TCP/IP stack; doing it before init (and
    // therefore potentially before the stack exists) is what the guard is for.
    CHECK_EQ_INT(sntp_fake::init_count(), 0);
    CHECK(sntp_fake::has_callback() == false);
}

void starting_sync_configures_and_starts_sntp() {
    blank_device();
    time_mgr_init();
    time_mgr_start_sync();

    CHECK_EQ_INT(sntp_fake::init_count(), 1);
    CHECK(sntp_fake::has_callback() == true);
    CHECK_EQ_INT(sntp_fake::operating_mode(), SNTP_OPMODE_POLL);
    CHECK(sntp_fake::sync_interval() > 0);

    // Three servers, so losing one provider does not leave the fleet unable to
    // set its clock.
    CHECK(sntp_fake::server(0) == "pool.ntp.org");
    CHECK(sntp_fake::server(1) == "time.google.com");
    CHECK(sntp_fake::server(2) == "time.cloudflare.com");
    CHECK(sntp_fake::server(0) != sntp_fake::server(1));
    CHECK(sntp_fake::server(1) != sntp_fake::server(2));
}

void sntp_is_configured_only_once() {
    blank_device();
    time_mgr_init();
    time_mgr_start_sync();
    time_mgr_start_sync();
    time_mgr_start_sync();

    // Re-registering servers and the callback on every reconnect would be
    // harmless-looking but is exactly how duplicate-callback bugs appear.
    CHECK_EQ_INT(sntp_fake::callback_set_count(), 1);
    CHECK_EQ_INT(sntp_fake::operating_mode_set_count(), 1);
}

void restarting_sync_stops_the_running_client_first() {
    blank_device();
    time_mgr_init();
    time_mgr_start_sync();
    CHECK(esp_sntp_enabled() == true);

    time_mgr_start_sync();
    // Calling esp_sntp_init() twice without stopping is what the guard exists
    // to avoid.
    CHECK_EQ_INT(sntp_fake::stop_count(), 1);
    CHECK_EQ_INT(sntp_fake::init_count(), 2);
    CHECK(esp_sntp_enabled() == true);
}

void stopping_sync_when_idle_does_nothing() {
    blank_device();
    time_mgr_init();
    time_mgr_stop_sync();
    CHECK_EQ_INT(sntp_fake::stop_count(), 0);
}

void stopping_sync_stops_a_running_client() {
    blank_device();
    time_mgr_init();
    time_mgr_start_sync();
    time_mgr_stop_sync();
    CHECK_EQ_INT(sntp_fake::stop_count(), 1);
    CHECK(esp_sntp_enabled() == false);
}

void forcing_a_sync_restarts_the_client() {
    blank_device();
    time_mgr_init();
    time_mgr_start_sync();

    time_mgr_force_sync();
    CHECK_EQ_INT(sntp_fake::stop_count(), 1);
    CHECK_EQ_INT(sntp_fake::init_count(), 2);
    CHECK(esp_sntp_enabled() == true);
}

void forcing_a_sync_before_init_does_nothing() {
    blank_device();
    time_mgr_force_sync();
    CHECK_EQ_INT(sntp_fake::init_count(), 0);
}

// ------------------------------------------------------------------------
// The clock-validity guard -- the part the device tests cannot reach
// ------------------------------------------------------------------------
void an_unsynced_device_does_not_claim_to_be_synced() {
    blank_device();
    time_fake::set(MIN_VALID_EPOCH + 86400);  // plausible clock...
    time_mgr_init();
    time_mgr_start_sync();

    // ...but no NTP reply has arrived. A plausible-looking clock is not
    // evidence of synchronisation: the RTC could simply be holding a stale
    // value from before the reboot.
    CHECK(time_mgr_is_synced() == false);
}

void a_good_ntp_reply_marks_the_clock_valid() {
    blank_device();
    time_fake::set(MIN_VALID_EPOCH + 86400);
    time_mgr_init();
    time_mgr_start_sync();

    sntp_fake::deliver_sync(MIN_VALID_EPOCH + 86400);
    CHECK(time_mgr_is_synced() == true);
}

void an_ntp_reply_from_1970_is_refused() {
    blank_device();
    time_fake::set(MIN_VALID_EPOCH + 86400);
    time_mgr_init();
    time_mgr_start_sync();

    // A server (or a mangled packet) claiming the epoch. Accepting it would
    // latch TIME_VALID_BIT permanently -- the bit is never cleared -- so every
    // subsequent event-log and telemetry record would be written with a
    // timestamp the firmware wrongly believes is trustworthy.
    sntp_fake::deliver_sync(0);
    CHECK(time_mgr_is_synced() == false);

    // ...and a later good reply must still be accepted.
    sntp_fake::deliver_sync(MIN_VALID_EPOCH + 86400);
    CHECK(time_mgr_is_synced() == true);
}

void an_ntp_reply_just_below_the_threshold_is_refused() {
    blank_device();
    time_fake::set(MIN_VALID_EPOCH + 86400);
    time_mgr_init();
    time_mgr_start_sync();

    sntp_fake::deliver_sync(MIN_VALID_EPOCH - 1);
    CHECK(time_mgr_is_synced() == false);
}

void an_ntp_reply_exactly_at_the_threshold_is_accepted() {
    blank_device();
    time_fake::set(MIN_VALID_EPOCH);
    time_mgr_init();
    time_mgr_start_sync();

    sntp_fake::deliver_sync(MIN_VALID_EPOCH);
    CHECK(time_mgr_is_synced() == true);
}

void a_null_timeval_does_not_mark_the_clock_valid() {
    blank_device();
    time_fake::set(MIN_VALID_EPOCH + 86400);
    time_mgr_init();
    time_mgr_start_sync();

    sntp_fake::deliver_sync_null();
    CHECK(time_mgr_is_synced() == false);
}

void a_synced_flag_does_not_survive_a_clock_that_reads_1970() {
    blank_device();
    time_fake::set(MIN_VALID_EPOCH + 86400);
    time_mgr_init();
    time_mgr_start_sync();
    sntp_fake::deliver_sync(MIN_VALID_EPOCH + 86400);
    CHECK(time_mgr_is_synced() == true);

    // The bit is latched, but is_synced() also re-checks the clock itself. If
    // something later resets the RTC, the device must stop claiming a good
    // clock rather than trusting a stale bit.
    time_fake::set(1000);
    CHECK(time_mgr_is_synced() == false);
}

void is_synced_is_false_before_init() {
    blank_device();
    // No event group exists yet; this must be false rather than reading a
    // null handle.
    CHECK(time_mgr_is_synced() == false);
}

// ------------------------------------------------------------------------
// Formatting
// ------------------------------------------------------------------------
void time_is_formatted_in_the_configured_zone() {
    blank_device();
    time_mgr_init();
    // A zone offset from UTC on purpose: formatting through gmtime instead of
    // localtime is invisible if the test only ever configures UTC.
    time_mgr_set_timezone("EST5EDT,M3.2.0,M11.1.0");
    time_fake::set(1751371200);  // 2025-07-01T12:00:00Z, EDT = UTC-4

    char buf[64];
    CHECK(time_mgr_get_time_str(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S") == true);
    CHECK_EQ_STR(buf, "2025-07-01 08:00:00");
}

void a_saved_timezone_takes_effect_at_boot() {
    blank_device();

    // Put the process in a known, different zone and force libc to latch it.
    // glibc only re-reads TZ when tzset() is called, so an init that sets the
    // environment variable without calling tzset() leaves every timestamp
    // formatted in whatever zone happened to be active first -- which on a
    // device is UTC, silently, for the entire uptime.
    setenv("TZ", "UTC0", 1);
    tzset();

    nvs_fake::seed_str(NS, KEY_TZ, "EST5EDT,M3.2.0,M11.1.0");
    time_mgr_init();
    time_fake::set(1751371200);

    char buf[64];
    CHECK(time_mgr_get_time_str(buf, sizeof(buf), "%H:%M:%S") == true);
    CHECK_EQ_STR(buf, "08:00:00");
}

void formatting_rejects_bad_arguments() {
    blank_device();
    time_mgr_init();
    char buf[64];
    CHECK(time_mgr_get_time_str(nullptr, sizeof(buf), "%Y") == false);
    CHECK(time_mgr_get_time_str(buf, 0, "%Y") == false);
    CHECK(time_mgr_get_time_str(buf, sizeof(buf), nullptr) == false);
}

void formatting_into_too_small_a_buffer_fails_rather_than_truncating() {
    blank_device();
    time_mgr_init();
    time_mgr_set_timezone("UTC0");
    time_fake::set(1751371200);

    char buf[4];
    // A truncated timestamp that looked successful would be worse than a
    // failure: the caller would render it.
    CHECK(time_mgr_get_time_str(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S") == false);
}

void local_time_reports_whether_the_clock_is_plausible() {
    blank_device();
    time_mgr_init();
    time_mgr_set_timezone("UTC0");

    struct tm info {};
    time_fake::set(1000);  // 1970
    CHECK(time_mgr_get_local_time(&info) == false);
    // Populated regardless, so a caller that ignores the return value still
    // renders something rather than reading uninitialised memory.
    CHECK_EQ_INT(info.tm_year + 1900, 1970);

    time_fake::set(1751371200);
    CHECK(time_mgr_get_local_time(&info) == true);
    CHECK_EQ_INT(info.tm_year + 1900, 2025);
}

void local_time_rejects_a_null_destination() {
    blank_device();
    time_mgr_init();
    CHECK(time_mgr_get_local_time(nullptr) == false);
}

// ------------------------------------------------------------------------
// Harness: one forked child per scenario. time_manager keeps module-level
// state with an `initialized` latch, and TZ is process-wide, so a child per
// scenario is the only way to get a genuinely fresh boot.
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

#define SCENARIO(name, fn)                \
    do {                                  \
        ++total;                          \
        failed += run_scenario(name, fn); \
    } while (0)

    SCENARIO("a blank device uses the default timezone",
             a_blank_device_uses_the_default_timezone);
    SCENARIO("a saved timezone is restored", a_saved_timezone_is_restored);
    SCENARIO("an unreadable nvs falls back to the default",
             an_unreadable_nvs_falls_back_to_the_default);
    SCENARIO("the default survives a missing key",
             the_default_survives_a_missing_key);
    SCENARIO("init is single shot", init_is_single_shot);
    SCENARIO("a failed event group allocation does not complete init",
             a_failed_event_group_allocation_does_not_complete_init);

    SCENARIO("the format defaults to 24 hour", the_format_defaults_to_24_hour);
    SCENARIO("a saved 12 hour preference is restored",
             a_saved_12_hour_preference_is_restored);
    SCENARIO("setting the format persists it", setting_the_format_persists_it);
    SCENARIO("the format survives an unwritable nvs",
             the_format_survives_an_unwritable_nvs);

    SCENARIO("setting a timezone applies and persists it",
             setting_a_timezone_applies_and_persists_it);
    SCENARIO("a null timezone is ignored", a_null_timezone_is_ignored);
    SCENARIO("the timezone actually changes the local time",
             the_timezone_actually_changes_the_local_time);
    SCENARIO("a timezone too long for the buffer is truncated consistently",
             a_timezone_too_long_for_the_buffer_is_truncated_consistently);

    SCENARIO("sync does not start before init", sync_does_not_start_before_init);
    SCENARIO("starting sync configures and starts sntp",
             starting_sync_configures_and_starts_sntp);
    SCENARIO("sntp is configured only once", sntp_is_configured_only_once);
    SCENARIO("restarting sync stops the running client first",
             restarting_sync_stops_the_running_client_first);
    SCENARIO("stopping sync when idle does nothing",
             stopping_sync_when_idle_does_nothing);
    SCENARIO("stopping sync stops a running client",
             stopping_sync_stops_a_running_client);
    SCENARIO("forcing a sync restarts the client",
             forcing_a_sync_restarts_the_client);
    SCENARIO("forcing a sync before init does nothing",
             forcing_a_sync_before_init_does_nothing);

    SCENARIO("an unsynced device does not claim to be synced",
             an_unsynced_device_does_not_claim_to_be_synced);
    SCENARIO("a good ntp reply marks the clock valid",
             a_good_ntp_reply_marks_the_clock_valid);
    SCENARIO("an ntp reply from 1970 is refused",
             an_ntp_reply_from_1970_is_refused);
    SCENARIO("an ntp reply just below the threshold is refused",
             an_ntp_reply_just_below_the_threshold_is_refused);
    SCENARIO("an ntp reply exactly at the threshold is accepted",
             an_ntp_reply_exactly_at_the_threshold_is_accepted);
    SCENARIO("a null timeval does not mark the clock valid",
             a_null_timeval_does_not_mark_the_clock_valid);
    SCENARIO("a synced flag does not survive a clock that reads 1970",
             a_synced_flag_does_not_survive_a_clock_that_reads_1970);
    SCENARIO("is_synced is false before init", is_synced_is_false_before_init);

    SCENARIO("time is formatted in the configured zone",
             time_is_formatted_in_the_configured_zone);
    SCENARIO("a saved timezone takes effect at boot",
             a_saved_timezone_takes_effect_at_boot);
    SCENARIO("formatting rejects bad arguments", formatting_rejects_bad_arguments);
    SCENARIO("formatting into too small a buffer fails rather than truncating",
             formatting_into_too_small_a_buffer_fails_rather_than_truncating);
    SCENARIO("local time reports whether the clock is plausible",
             local_time_reports_whether_the_clock_is_plausible);
    SCENARIO("local time rejects a null destination",
             local_time_rejects_a_null_destination);

#undef SCENARIO

    if (failed == 0) {
        std::printf("time_manager: %d scenarios passed\n", total);
        std::fprintf(stderr, "time_manager: %d scenarios passed\n", total);
        return 0;
    }
    std::fprintf(stderr, "time_manager: %d of %d scenarios FAILED\n", failed, total);
    return 1;
}
