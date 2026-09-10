// Native unit tests for main/iana_tz.cpp and main/location_manager.cpp -- the
// automatic-timezone path.
//
// Open-Meteo's geocoding reply carries an IANA zone name; the device runs on
// POSIX TZ strings. iana_tz.cpp is the translation table between them, and
// location_manager.cpp decides when to apply it.
//
// This is a hand-curated table of 66 entries, which is the worst possible
// thing to have untested. A wrong POSIX rule is not a crash and not a failed
// request: the device simply keeps time in the wrong zone. Every displayed
// clock, every event-log entry and every telemetry sample is then off by an
// hour or more, and the only symptom is a user eventually noticing. Nothing in
// the firmware, the API suite or the UI suite can tell a correct rule from a
// plausible-looking typo, because they all read the value back through the
// same table that produced it.
//
// So the table is not checked against itself here. Each entry is checked
// against an INDEPENDENTLY STATED UTC offset for a winter instant and a summer
// instant, resolved by the C library's own POSIX TZ parser. That catches a
// malformed rule (which silently degrades to UTC), a transposed DST window,
// wrong-hemisphere rules, and an offset with the sign flipped -- the actual
// ways this table goes wrong.
//
// Refs #217 (T13).

#include "nvs_fake.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>
#include <string>

#include "iana_tz.h"
#include "location_manager.h"
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

#define CHECK_EQ_STR(actual, expected)                                           \
    do {                                                                         \
        const char* a_ = (actual);                                               \
        const char* e_ = (expected);                                             \
        if (a_ == nullptr || std::strcmp(a_, e_) != 0) {                         \
            std::fprintf(stderr, "  FAIL %s:%d: %s -> \"%s\", expected \"%s\"\n", \
                         __FILE__, __LINE__, #actual, a_ ? a_ : "(null)", e_);   \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

namespace {

// Must match location_manager.cpp. Duplicated deliberately: renaming a key
// silently discards every deployed device's saved location.
constexpr const char* NS = "loc_cfg";
constexpr const char* KEY_LAT = "lat";
constexpr const char* KEY_LON = "lon";
constexpr const char* KEY_NAME = "name";
constexpr const char* KEY_IANA = "iana";
constexpr const char* KEY_VALID = "valid";
constexpr const char* KEY_TZAUTO = "tz_auto";

constexpr const char* DEFAULT_NAME = "Sun Peaks, British Columbia, CA";
constexpr const char* DEFAULT_IANA = "America/Vancouver";

// Four instants, not two. January and July alone cannot tell the European DST
// rule (last Sunday of March / last Sunday of October) from the North American
// one (second Sunday of March / first Sunday of November): both halves of the
// year agree. The two shoulder instants below fall inside the weeks where the
// two rule families disagree, which is the only place a transplanted rule is
// observable -- and a zone that is wrong for three weeks a year is exactly the
// kind of defect that ships.
constexpr time_t JAN = 1736942400;  // 2025-01-15T12:00:00Z
constexpr time_t MAR = 1742472000;  // 2025-03-20T12:00:00Z  US on DST, EU not
constexpr time_t JUL = 1752580800;  // 2025-07-15T12:00:00Z
constexpr time_t OCT = 1761652800;  // 2025-10-28T12:00:00Z  EU off DST, US not

// UTC offset in minutes that `posix` produces at `when`, according to the C
// library's POSIX TZ parser rather than according to this repository.
int offset_minutes(const char* posix, time_t when) {
    setenv("TZ", posix, 1);
    tzset();
    struct tm lt {};
    localtime_r(&when, &lt);
    return (int)(lt.tm_gmtoff / 60);
}

struct ZoneCase {
    const char* iana;
    int jan_minutes;
    int mar_minutes;
    int jul_minutes;
    int oct_minutes;
};

// Offsets stated from first principles, NOT read off the table under test.
// A zone whose four offsets are all equal is asserting "this zone does not
// observe DST" -- several entries exist precisely to encode that (Arizona,
// Saskatchewan, the BC Peace River, Yukon, post-2022 Mexico, Queensland).
const ZoneCase kZones[] = {
    // ---- United States: DST 2025-03-09 .. 2025-11-02, so both shoulder
    // instants are inside DST. ----
    {"America/New_York", -300, -240, -240, -240},
    {"America/Detroit", -300, -240, -240, -240},
    {"America/Indiana/Indianapolis", -300, -240, -240, -240},
    {"America/Chicago", -360, -300, -300, -300},
    {"America/Denver", -420, -360, -360, -360},
    {"America/Boise", -420, -360, -360, -360},
    {"America/Phoenix", -420, -420, -420, -420},
    {"America/Los_Angeles", -480, -420, -420, -420},
    {"America/Anchorage", -540, -480, -480, -480},
    {"America/Juneau", -540, -480, -480, -480},
    {"America/Sitka", -540, -480, -480, -480},
    {"Pacific/Honolulu", -600, -600, -600, -600},

    // ---- Canada ----
    {"America/Toronto", -300, -240, -240, -240},
    {"America/Montreal", -300, -240, -240, -240},
    {"America/Nipigon", -300, -240, -240, -240},
    {"America/Winnipeg", -360, -300, -300, -300},
    {"America/Regina", -360, -360, -360, -360},
    {"America/Swift_Current", -360, -360, -360, -360},
    {"America/Edmonton", -420, -360, -360, -360},
    {"America/Vancouver", -480, -420, -420, -420},
    {"America/Dawson_Creek", -420, -420, -420, -420},
    {"America/Fort_Nelson", -420, -420, -420, -420},
    {"America/Whitehorse", -420, -420, -420, -420},
    {"America/Dawson", -420, -420, -420, -420},
    {"America/Halifax", -240, -180, -180, -180},
    {"America/Moncton", -240, -180, -180, -180},
    {"America/Glace_Bay", -240, -180, -180, -180},
    {"America/St_Johns", -210, -150, -150, -150},

    // ---- Mexico ----
    {"America/Mexico_City", -360, -360, -360, -360},
    {"America/Monterrey", -360, -360, -360, -360},
    {"America/Tijuana", -480, -420, -420, -420},

    // ---- Europe: DST 2025-03-30 .. 2025-10-26, so BOTH shoulder instants
    // are outside DST. This is what separates a European rule from a North
    // American one pasted in its place. ----
    {"Europe/London", 0, 0, 60, 0},
    {"Europe/Dublin", 0, 0, 60, 0},
    {"Europe/Lisbon", 0, 0, 60, 0},
    {"Europe/Paris", 60, 60, 120, 60},
    {"Europe/Berlin", 60, 60, 120, 60},
    {"Europe/Madrid", 60, 60, 120, 60},
    {"Europe/Rome", 60, 60, 120, 60},
    {"Europe/Amsterdam", 60, 60, 120, 60},
    {"Europe/Brussels", 60, 60, 120, 60},
    {"Europe/Zurich", 60, 60, 120, 60},
    {"Europe/Vienna", 60, 60, 120, 60},
    {"Europe/Warsaw", 60, 60, 120, 60},
    {"Europe/Stockholm", 60, 60, 120, 60},
    {"Europe/Helsinki", 120, 120, 180, 120},
    {"Europe/Athens", 120, 120, 180, 120},
    {"Europe/Bucharest", 120, 120, 180, 120},
    {"Europe/Kiev", 120, 120, 180, 120},
    {"Europe/Kyiv", 120, 120, 180, 120},

    // ---- Asia: no DST anywhere in this set ----
    {"Asia/Tokyo", 540, 540, 540, 540},
    {"Asia/Shanghai", 480, 480, 480, 480},
    {"Asia/Hong_Kong", 480, 480, 480, 480},
    {"Asia/Singapore", 480, 480, 480, 480},
    {"Asia/Seoul", 540, 540, 540, 540},
    {"Asia/Kolkata", 330, 330, 330, 330},
    {"Asia/Calcutta", 330, 330, 330, 330},
    {"Asia/Dubai", 240, 240, 240, 240},

    // ---- Australia / New Zealand: southern hemisphere, so January is the
    // DST half and July is not. A northern rule pasted here shows up as an
    // offset that moves the wrong way. Both shoulder instants are inside
    // southern DST. ----
    {"Australia/Sydney", 660, 660, 600, 660},
    {"Australia/Melbourne", 660, 660, 600, 660},
    {"Australia/Brisbane", 600, 600, 600, 600},
    {"Australia/Adelaide", 630, 630, 570, 630},
    {"Australia/Perth", 480, 480, 480, 480},
    {"Pacific/Auckland", 780, 780, 720, 780},

    // ---- UTC ----
    {"UTC", 0, 0, 0, 0},
    {"Etc/UTC", 0, 0, 0, 0},
    {"Etc/GMT", 0, 0, 0, 0},
};

constexpr size_t kZoneCount = sizeof(kZones) / sizeof(kZones[0]);

void reset_all() {
    nvs_fake::reset();
    unsetenv("TZ");
    tzset();
}

// ------------------------------------------------------------------------
// iana_tz: the table
// ------------------------------------------------------------------------
void every_mapped_zone_produces_the_expected_utc_offsets() {
    for (size_t i = 0; i < kZoneCount; ++i) {
        const ZoneCase& z = kZones[i];
        const char* posix = iana_to_posix(z.iana);
        if (posix == nullptr) {
            std::fprintf(stderr, "  FAIL %s is not in the table\n", z.iana);
            ++g_failures;
            continue;
        }
        const int got[4] = {offset_minutes(posix, JAN), offset_minutes(posix, MAR),
                            offset_minutes(posix, JUL), offset_minutes(posix, OCT)};
        const int want[4] = {z.jan_minutes, z.mar_minutes, z.jul_minutes,
                             z.oct_minutes};
        for (int k = 0; k < 4; ++k) {
            if (got[k] != want[k]) {
                std::fprintf(stderr,
                             "  FAIL %-32s -> \"%s\": Jan %+d Mar %+d Jul %+d "
                             "Oct %+d, expected Jan %+d Mar %+d Jul %+d Oct %+d\n",
                             z.iana, posix, got[0], got[1], got[2], got[3],
                             want[0], want[1], want[2], want[3]);
                ++g_failures;
                break;
            }
        }
    }
}

void the_dst_changeover_windows_match_the_right_region() {
    // Stated separately from the offset table so the intent survives a
    // well-meaning "fix" to an individual number. Europe and North America
    // agree in midwinter and midsummer and differ only here; getting this
    // wrong leaves a zone an hour out for about three weeks each spring and
    // one week each autumn, which no other test in this repository could see.
    struct {
        const char* iana;
        bool dst_in_march;
        bool dst_in_late_october;
    } cases[] = {
        {"America/New_York", true, true},   // 2nd Sun Mar .. 1st Sun Nov
        {"America/Vancouver", true, true},
        {"America/Tijuana", true, true},    // follows the US, not Mexico
        {"Europe/London", false, false},    // last Sun Mar .. last Sun Oct
        {"Europe/Berlin", false, false},
        {"Europe/Helsinki", false, false},
    };
    for (auto& c : cases) {
        const char* posix = iana_to_posix(c.iana);
        CHECK(posix != nullptr);
        if (!posix) continue;
        bool mar_dst = offset_minutes(posix, MAR) != offset_minutes(posix, JAN);
        bool oct_dst = offset_minutes(posix, OCT) != offset_minutes(posix, JAN);
        if (mar_dst != c.dst_in_march || oct_dst != c.dst_in_late_october) {
            std::fprintf(stderr,
                         "  FAIL %s changeover window is wrong: \"%s\" "
                         "(Mar DST=%d Oct DST=%d, expected %d/%d)\n",
                         c.iana, posix, mar_dst, oct_dst, c.dst_in_march,
                         c.dst_in_late_october);
            ++g_failures;
        }
    }
}

void no_mapped_zone_silently_degrades_to_utc() {
    // A POSIX string the C library cannot parse does not raise an error: it is
    // treated as UTC. That is the failure mode a typo in the table actually
    // produces, and it is invisible unless the zone is genuinely meant to be
    // UTC. Checking every non-UTC zone differs from UTC in at least one half
    // of the year catches a whole class of malformed rules at once.
    for (size_t i = 0; i < kZoneCount; ++i) {
        const ZoneCase& z = kZones[i];
        if (z.jan_minutes == 0 && z.jul_minutes == 0) continue;
        const char* posix = iana_to_posix(z.iana);
        CHECK(posix != nullptr);
        if (!posix) continue;
        bool differs = offset_minutes(posix, JAN) != 0 ||
                       offset_minutes(posix, JUL) != 0;
        if (!differs) {
            std::fprintf(stderr, "  FAIL %s parsed as UTC: \"%s\"\n", z.iana,
                         posix);
            ++g_failures;
        }
    }
}

void a_zone_that_should_not_observe_dst_does_not() {
    // Stated separately from the offset table so that the intent survives even
    // if someone "fixes" an offset. Arizona, Saskatchewan, the BC Peace River,
    // Yukon, Queensland and post-2022 Mexico are the entries whose whole
    // reason for existing is that the obvious rule would be wrong.
    const char* no_dst[] = {
        "America/Phoenix",      "America/Regina",  "America/Swift_Current",
        "America/Dawson_Creek", "America/Whitehorse", "America/Fort_Nelson",
        "America/Dawson",       "America/Mexico_City", "America/Monterrey",
        "Australia/Brisbane",   "Australia/Perth", "Pacific/Honolulu",
    };
    for (const char* iana : no_dst) {
        const char* posix = iana_to_posix(iana);
        CHECK(posix != nullptr);
        if (!posix) continue;
        int base = offset_minutes(posix, JAN);
        if (offset_minutes(posix, MAR) != base ||
            offset_minutes(posix, JUL) != base ||
            offset_minutes(posix, OCT) != base) {
            std::fprintf(stderr, "  FAIL %s observes DST but should not: \"%s\"\n",
                         iana, posix);
            ++g_failures;
        }
    }
}

void an_unmapped_zone_returns_null() {
    // The caller's contract: NULL means "fall back to manual selection", so it
    // must be distinguishable from a mapped zone rather than guessed at.
    CHECK(iana_to_posix("America/Nowhere") == nullptr);
    CHECK(iana_to_posix("Mars/Olympus_Mons") == nullptr);
    CHECK(iana_to_posix("Europe") == nullptr);
    // A prefix of a real key must not match: a substring match here would map
    // half the world to Toronto.
    CHECK(iana_to_posix("America/") == nullptr);
    CHECK(iana_to_posix("America/New_Yor") == nullptr);
    CHECK(iana_to_posix("America/New_York_City") == nullptr);
}

void lookup_is_case_sensitive_and_exact() {
    CHECK(iana_to_posix("america/new_york") == nullptr);
    CHECK(iana_to_posix("AMERICA/NEW_YORK") == nullptr);
    CHECK(iana_to_posix(" America/New_York") == nullptr);
    CHECK(iana_to_posix("America/New_York ") == nullptr);
}

void a_null_or_empty_zone_returns_null() {
    CHECK(iana_to_posix(nullptr) == nullptr);
    CHECK(iana_to_posix("") == nullptr);
}

void the_table_returns_stable_static_storage() {
    // Documented contract: the caller must not free the result, and callers do
    // hold it across calls.
    const char* a = iana_to_posix("America/Vancouver");
    const char* b = iana_to_posix("America/Vancouver");
    CHECK(a == b);
    (void)iana_to_posix("Asia/Tokyo");
    CHECK_EQ_STR(a, "PST8PDT,M3.2.0,M11.1.0");
}

void aliases_agree_with_their_canonical_zone() {
    // Both spellings are reachable from the geocoding API depending on its tz
    // database vintage; disagreeing would make the device's clock depend on
    // which name happened to come back.
    CHECK_EQ_STR(iana_to_posix("Asia/Calcutta"), iana_to_posix("Asia/Kolkata"));
    CHECK_EQ_STR(iana_to_posix("Europe/Kiev"), iana_to_posix("Europe/Kyiv"));
    CHECK_EQ_STR(iana_to_posix("Etc/UTC"), iana_to_posix("UTC"));
    CHECK_EQ_STR(iana_to_posix("Etc/GMT"), iana_to_posix("UTC"));
}

// ------------------------------------------------------------------------
// location_manager: defaults and persistence
// ------------------------------------------------------------------------
void a_blank_device_defaults_to_the_deployment_site() {
    reset_all();
    location_mgr_init();

    const location_t* loc = location_mgr_get();
    CHECK(loc != nullptr);
    // valid stays false: the default is a seed for weather and timezone, not a
    // claim that the user chose it.
    CHECK(loc->valid == false);
    CHECK_EQ_STR(loc->name, DEFAULT_NAME);
    CHECK_EQ_STR(loc->iana_tz, DEFAULT_IANA);
    CHECK(loc->latitude > 50.0 && loc->latitude < 51.0);
    CHECK(loc->longitude < -119.0 && loc->longitude > -120.0);
}

void auto_timezone_is_on_by_default_and_applied_at_boot() {
    reset_all();
    location_mgr_init();

    CHECK(location_mgr_get_tz_auto() == true);
    // The point of the default location is that the clock is right before the
    // user has configured anything.
    CHECK_EQ_STR(time_mgr_get_timezone(), "PST8PDT,M3.2.0,M11.1.0");
    CHECK_EQ_STR(getenv("TZ"), "PST8PDT,M3.2.0,M11.1.0");
}

void a_saved_location_is_restored() {
    reset_all();
    double lat = 45.5019, lon = -73.5674;
    nvs_fake::seed_u8(NS, KEY_VALID, 1);
    nvs_fake::seed_blob(NS, KEY_LAT,
                        std::vector<uint8_t>((uint8_t*)&lat, (uint8_t*)&lat + sizeof(lat)));
    nvs_fake::seed_blob(NS, KEY_LON,
                        std::vector<uint8_t>((uint8_t*)&lon, (uint8_t*)&lon + sizeof(lon)));
    nvs_fake::seed_str(NS, KEY_NAME, "Montreal, Quebec, CA");
    nvs_fake::seed_str(NS, KEY_IANA, "America/Montreal");

    location_mgr_init();

    const location_t* loc = location_mgr_get();
    CHECK(loc->valid == true);
    CHECK_EQ_STR(loc->name, "Montreal, Quebec, CA");
    CHECK_EQ_STR(loc->iana_tz, "America/Montreal");
    CHECK(loc->latitude > 45.50 && loc->latitude < 45.51);
    CHECK(loc->longitude < -73.56 && loc->longitude > -73.57);
    CHECK_EQ_STR(time_mgr_get_timezone(), "EST5EDT,M3.2.0,M11.1.0");
}

void a_location_saved_but_marked_invalid_is_ignored() {
    reset_all();
    double lat = 45.5, lon = -73.5;
    nvs_fake::seed_u8(NS, KEY_VALID, 0);
    nvs_fake::seed_blob(NS, KEY_LAT,
                        std::vector<uint8_t>((uint8_t*)&lat, (uint8_t*)&lat + sizeof(lat)));
    nvs_fake::seed_blob(NS, KEY_LON,
                        std::vector<uint8_t>((uint8_t*)&lon, (uint8_t*)&lon + sizeof(lon)));
    nvs_fake::seed_str(NS, KEY_NAME, "Montreal, Quebec, CA");

    location_mgr_init();

    const location_t* loc = location_mgr_get();
    CHECK(loc->valid == false);
    CHECK_EQ_STR(loc->name, DEFAULT_NAME);
}

void a_half_written_location_is_not_half_applied() {
    reset_all();
    double lat = 45.5019;
    nvs_fake::seed_u8(NS, KEY_VALID, 1);
    nvs_fake::seed_blob(NS, KEY_LAT,
                        std::vector<uint8_t>((uint8_t*)&lat, (uint8_t*)&lat + sizeof(lat)));
    // Longitude never made it -- power loss between the two blob writes, which
    // save_to_nvs() performs as separate operations under one commit.
    nvs_fake::seed_str(NS, KEY_NAME, "Montreal, Quebec, CA");
    nvs_fake::seed_str(NS, KEY_IANA, "America/Montreal");

    location_mgr_init();

    const location_t* loc = location_mgr_get();
    // Latitude without longitude is a point in the Atlantic, not a location.
    // Falling back wholesale to the default is the only safe reading.
    CHECK(loc->valid == false);
    CHECK_EQ_STR(loc->name, DEFAULT_NAME);
    CHECK_EQ_STR(loc->iana_tz, DEFAULT_IANA);
}

void a_saved_location_with_no_name_still_loads() {
    reset_all();
    double lat = 35.6762, lon = 139.6503;
    nvs_fake::seed_u8(NS, KEY_VALID, 1);
    nvs_fake::seed_blob(NS, KEY_LAT,
                        std::vector<uint8_t>((uint8_t*)&lat, (uint8_t*)&lat + sizeof(lat)));
    nvs_fake::seed_blob(NS, KEY_LON,
                        std::vector<uint8_t>((uint8_t*)&lon, (uint8_t*)&lon + sizeof(lon)));
    nvs_fake::seed_str(NS, KEY_IANA, "Asia/Tokyo");

    location_mgr_init();

    const location_t* loc = location_mgr_get();
    CHECK(loc->valid == true);
    // A missing label must be cleared, not left showing the default site's
    // name next to another city's coordinates.
    CHECK_EQ_STR(loc->name, "");
    CHECK_EQ_STR(time_mgr_get_timezone(), "JST-9");
}

void an_unreadable_nvs_leaves_the_defaults_intact() {
    reset_all();
    nvs_fake::fail_next(nvs_fake::Op::Open, ESP_FAIL, -1);
    location_mgr_init();
    nvs_fake::clear_failures();

    const location_t* loc = location_mgr_get();
    CHECK(loc->valid == false);
    CHECK_EQ_STR(loc->name, DEFAULT_NAME);
    // And the default zone is still derived, so the clock is not left in UTC.
    CHECK_EQ_STR(time_mgr_get_timezone(), "PST8PDT,M3.2.0,M11.1.0");
}

// ------------------------------------------------------------------------
// location_manager: setting a location
// ------------------------------------------------------------------------
void setting_a_location_persists_every_field() {
    reset_all();
    location_mgr_init();
    location_mgr_set(35.6762, 139.6503, "Tokyo, JP", "Asia/Tokyo");

    uint8_t v = 0;
    CHECK(nvs_fake::peek_u8(NS, KEY_VALID, &v) && v == 1);
    std::string s;
    CHECK(nvs_fake::peek_str(NS, KEY_NAME, &s) && s == "Tokyo, JP");
    CHECK(nvs_fake::peek_str(NS, KEY_IANA, &s) && s == "Asia/Tokyo");
    std::vector<uint8_t> blob;
    CHECK(nvs_fake::peek_blob(NS, KEY_LAT, &blob) && blob.size() == sizeof(double));
    CHECK(nvs_fake::peek_blob(NS, KEY_LON, &blob) && blob.size() == sizeof(double));
}

void setting_a_location_applies_its_timezone_immediately() {
    reset_all();
    location_mgr_init();
    CHECK_EQ_STR(time_mgr_get_timezone(), "PST8PDT,M3.2.0,M11.1.0");

    location_mgr_set(-33.8688, 151.2093, "Sydney, AU", "Australia/Sydney");
    CHECK_EQ_STR(time_mgr_get_timezone(), "AEST-10AEDT,M10.1.0,M4.1.0/3");
    CHECK_EQ_STR(getenv("TZ"), "AEST-10AEDT,M10.1.0,M4.1.0/3");
}

void coordinates_survive_the_blob_round_trip_exactly() {
    reset_all();
    location_mgr_init();
    // A negative longitude with full double precision: storing these as float,
    // or losing the sign, puts the weather lookup on the wrong continent.
    const double lat = 50.87620000000001;
    const double lon = -119.91075000000001;
    location_mgr_set(lat, lon, "Sun Peaks", "America/Vancouver");

    std::vector<uint8_t> blob;
    CHECK(nvs_fake::peek_blob(NS, KEY_LAT, &blob));
    double back_lat = 0;
    std::memcpy(&back_lat, blob.data(), sizeof(back_lat));
    CHECK(back_lat == lat);

    CHECK(nvs_fake::peek_blob(NS, KEY_LON, &blob));
    double back_lon = 0;
    std::memcpy(&back_lon, blob.data(), sizeof(back_lon));
    CHECK(back_lon == lon);
    CHECK(back_lon < 0);
}

void an_unmapped_zone_leaves_the_timezone_alone() {
    reset_all();
    location_mgr_init();
    // Deliberately NOT the module's default zone: if the "leave it alone"
    // branch were replaced by "reset to the default", a test that started from
    // the default could not tell the difference.
    time_mgr_set_timezone("JST-9");

    location_mgr_set(-1.2921, 36.8219, "Nairobi, KE", "Africa/Nairobi");

    // The zone is not in the curated table. Resetting to a default, or to UTC,
    // would silently move a clock the user may have set by hand; keeping the
    // current zone and letting them choose manually is the documented
    // behaviour.
    CHECK_EQ_STR(time_mgr_get_timezone(), "JST-9");
    CHECK(location_mgr_derived_posix() == nullptr);
    // The location itself is still stored -- weather does not need the zone.
    CHECK(location_mgr_get()->valid == true);
    CHECK_EQ_STR(location_mgr_get()->iana_tz, "Africa/Nairobi");
}

void a_null_zone_clears_the_stored_zone_without_changing_the_clock() {
    reset_all();
    location_mgr_init();
    time_mgr_set_timezone("JST-9");

    location_mgr_set(48.8566, 2.3522, "Paris, FR", nullptr);

    CHECK_EQ_STR(location_mgr_get()->iana_tz, "");
    CHECK(location_mgr_derived_posix() == nullptr);
    CHECK_EQ_STR(time_mgr_get_timezone(), "JST-9");
}

void a_null_name_leaves_the_previous_name_in_place() {
    reset_all();
    location_mgr_init();
    location_mgr_set(48.8566, 2.3522, "Paris, FR", "Europe/Paris");
    location_mgr_set(48.8566, 2.3522, nullptr, "Europe/Paris");
    CHECK_EQ_STR(location_mgr_get()->name, "Paris, FR");
}

void an_over_long_name_is_truncated_and_terminated() {
    reset_all();
    location_mgr_init();
    std::string long_name(300, 'N');
    location_mgr_set(0.0, 0.0, long_name.c_str(), "UTC");

    const location_t* loc = location_mgr_get();
    CHECK_EQ_INT(std::strlen(loc->name), 95);  // name[96]
    // What is persisted must be what is in RAM, or the label changes on reboot.
    std::string s;
    CHECK(nvs_fake::peek_str(NS, KEY_NAME, &s));
    CHECK(s == std::string(loc->name));
}

void an_over_long_zone_is_truncated_and_does_not_match() {
    reset_all();
    location_mgr_init();
    time_mgr_set_timezone("JST-9");
    std::string long_tz(200, 'Z');
    location_mgr_set(0.0, 0.0, "Nowhere", long_tz.c_str());

    const location_t* loc = location_mgr_get();
    CHECK_EQ_INT(std::strlen(loc->iana_tz), 47);  // iana_tz[48]
    CHECK(location_mgr_derived_posix() == nullptr);
    CHECK_EQ_STR(time_mgr_get_timezone(), "JST-9");
}

// ------------------------------------------------------------------------
// location_manager: the auto/manual switch
// ------------------------------------------------------------------------
void disabling_auto_mode_protects_a_manually_chosen_zone() {
    reset_all();
    location_mgr_init();
    location_mgr_set_tz_auto(false);
    time_mgr_set_timezone("UTC0");

    location_mgr_set(35.6762, 139.6503, "Tokyo, JP", "Asia/Tokyo");

    // This is the whole reason the flag exists: picking a location for weather
    // must not silently overwrite a timezone the user set by hand.
    CHECK_EQ_STR(time_mgr_get_timezone(), "UTC0");
    // ...but the derivation is still available for the UI to offer.
    CHECK_EQ_STR(location_mgr_derived_posix(), "JST-9");
}

void re_enabling_auto_mode_re_derives_the_zone() {
    reset_all();
    location_mgr_init();
    location_mgr_set_tz_auto(false);
    location_mgr_set(35.6762, 139.6503, "Tokyo, JP", "Asia/Tokyo");
    time_mgr_set_timezone("UTC0");

    location_mgr_set_tz_auto(true);
    CHECK_EQ_STR(time_mgr_get_timezone(), "JST-9");
}

void disabling_auto_mode_does_not_itself_change_the_zone() {
    reset_all();
    location_mgr_init();
    CHECK_EQ_STR(time_mgr_get_timezone(), "PST8PDT,M3.2.0,M11.1.0");
    location_mgr_set_tz_auto(false);
    CHECK_EQ_STR(time_mgr_get_timezone(), "PST8PDT,M3.2.0,M11.1.0");
}

void the_auto_mode_preference_is_persisted() {
    reset_all();
    location_mgr_init();
    location_mgr_set_tz_auto(false);

    uint8_t v = 0xff;
    CHECK(nvs_fake::peek_u8(NS, KEY_TZAUTO, &v));
    CHECK_EQ_INT(v, 0);
    CHECK(nvs_fake::call_count(nvs_fake::Op::Commit) > 0);
}

void a_persisted_manual_preference_survives_a_reboot() {
    reset_all();
    nvs_fake::seed_u8(NS, KEY_TZAUTO, 0);
    time_mgr_set_timezone("UTC0");

    location_mgr_init();

    CHECK(location_mgr_get_tz_auto() == false);
    // init() must not re-derive over a manual choice either.
    CHECK_EQ_STR(time_mgr_get_timezone(), "UTC0");
}

void derived_posix_is_null_when_no_zone_is_known() {
    reset_all();
    location_mgr_init();
    location_mgr_set(0.0, 0.0, "Null Island", "");
    CHECK(location_mgr_derived_posix() == nullptr);
}

void derived_posix_tracks_the_current_location() {
    reset_all();
    location_mgr_init();
    CHECK_EQ_STR(location_mgr_derived_posix(), "PST8PDT,M3.2.0,M11.1.0");
    location_mgr_set(51.5074, -0.1278, "London, UK", "Europe/London");
    CHECK_EQ_STR(location_mgr_derived_posix(), "GMT0BST,M3.5.0/1,M10.5.0");
}

void the_zone_a_location_derives_is_the_zone_the_clock_uses() {
    // Ties the two modules together: whatever derived_posix() advertises is
    // what actually got applied. A divergence here is how the settings screen
    // ends up displaying one zone while the device keeps time in another.
    reset_all();
    location_mgr_init();
    const char* cities[][2] = {
        {"Europe/Berlin", "Berlin, DE"},
        {"Australia/Adelaide", "Adelaide, AU"},
        {"America/St_Johns", "St John's, CA"},
        {"Asia/Kolkata", "Kolkata, IN"},
    };
    for (auto& c : cities) {
        location_mgr_set(0.0, 0.0, c[1], c[0]);
        const char* derived = location_mgr_derived_posix();
        CHECK(derived != nullptr);
        if (!derived) continue;
        CHECK_EQ_STR(time_mgr_get_timezone(), derived);
        CHECK_EQ_STR(getenv("TZ"), derived);
    }
}

// ------------------------------------------------------------------------
// Harness: one forked child per scenario. location_manager and time_manager
// both hold module-level state and TZ is process-wide, so a child per scenario
// is the only way to get a genuinely fresh boot.
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

    SCENARIO("every mapped zone produces the expected utc offsets",
             every_mapped_zone_produces_the_expected_utc_offsets);
    SCENARIO("no mapped zone silently degrades to utc",
             no_mapped_zone_silently_degrades_to_utc);
    SCENARIO("a zone that should not observe dst does not",
             a_zone_that_should_not_observe_dst_does_not);
    SCENARIO("the dst changeover windows match the right region",
             the_dst_changeover_windows_match_the_right_region);
    SCENARIO("an unmapped zone returns null", an_unmapped_zone_returns_null);
    SCENARIO("lookup is case sensitive and exact",
             lookup_is_case_sensitive_and_exact);
    SCENARIO("a null or empty zone returns null",
             a_null_or_empty_zone_returns_null);
    SCENARIO("the table returns stable static storage",
             the_table_returns_stable_static_storage);
    SCENARIO("aliases agree with their canonical zone",
             aliases_agree_with_their_canonical_zone);

    SCENARIO("a blank device defaults to the deployment site",
             a_blank_device_defaults_to_the_deployment_site);
    SCENARIO("auto timezone is on by default and applied at boot",
             auto_timezone_is_on_by_default_and_applied_at_boot);
    SCENARIO("a saved location is restored", a_saved_location_is_restored);
    SCENARIO("a location saved but marked invalid is ignored",
             a_location_saved_but_marked_invalid_is_ignored);
    SCENARIO("a half written location is not half applied",
             a_half_written_location_is_not_half_applied);
    SCENARIO("a saved location with no name still loads",
             a_saved_location_with_no_name_still_loads);
    SCENARIO("an unreadable nvs leaves the defaults intact",
             an_unreadable_nvs_leaves_the_defaults_intact);

    SCENARIO("setting a location persists every field",
             setting_a_location_persists_every_field);
    SCENARIO("setting a location applies its timezone immediately",
             setting_a_location_applies_its_timezone_immediately);
    SCENARIO("coordinates survive the blob round trip exactly",
             coordinates_survive_the_blob_round_trip_exactly);
    SCENARIO("an unmapped zone leaves the timezone alone",
             an_unmapped_zone_leaves_the_timezone_alone);
    SCENARIO("a null zone clears the stored zone without changing the clock",
             a_null_zone_clears_the_stored_zone_without_changing_the_clock);
    SCENARIO("a null name leaves the previous name in place",
             a_null_name_leaves_the_previous_name_in_place);
    SCENARIO("an over long name is truncated and terminated",
             an_over_long_name_is_truncated_and_terminated);
    SCENARIO("an over long zone is truncated and does not match",
             an_over_long_zone_is_truncated_and_does_not_match);

    SCENARIO("disabling auto mode protects a manually chosen zone",
             disabling_auto_mode_protects_a_manually_chosen_zone);
    SCENARIO("re enabling auto mode re derives the zone",
             re_enabling_auto_mode_re_derives_the_zone);
    SCENARIO("disabling auto mode does not itself change the zone",
             disabling_auto_mode_does_not_itself_change_the_zone);
    SCENARIO("the auto mode preference is persisted",
             the_auto_mode_preference_is_persisted);
    SCENARIO("a persisted manual preference survives a reboot",
             a_persisted_manual_preference_survives_a_reboot);
    SCENARIO("derived posix is null when no zone is known",
             derived_posix_is_null_when_no_zone_is_known);
    SCENARIO("derived posix tracks the current location",
             derived_posix_tracks_the_current_location);
    SCENARIO("the zone a location derives is the zone the clock uses",
             the_zone_a_location_derives_is_the_zone_the_clock_uses);

#undef SCENARIO

    if (failed == 0) {
        std::printf("location: %d scenarios passed (%zu zones)\n", total, kZoneCount);
        std::fprintf(stderr, "location: %d scenarios passed (%zu zones)\n", total,
                     kZoneCount);
        return 0;
    }
    std::fprintf(stderr, "location: %d of %d scenarios FAILED\n", failed, total);
    return 1;
}
