// Native unit tests for main/ext_sensor_notice.cpp -- the "temperature sensor
// offline" notification bell entry.
//
// It is one entry however many outages there are, so a flapping sensor can't
// fill the bell; supply and return dropping together are one outage; and an
// outage the user already acknowledged doesn't come back when it ends.

#include <cstdio>
#include <cstring>
#include <string>

#include "ext_sensor_notice.h"
#include "i18n.h"

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                           \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

#define CHECK_EQ_STR(actual, expected)                                        \
    do {                                                                      \
        const std::string a_ = (actual);                                      \
        const std::string e_ = (expected);                                    \
        if (a_ != e_) {                                                       \
            std::fprintf(stderr, "  FAIL %s:%d: %s -> \"%s\", expected \"%s\"\n", \
                         __FILE__, __LINE__, #actual, a_.c_str(), e_.c_str()); \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

namespace {

ext_sensor_notice_t get() {
    ext_sensor_notice_t n;
    ext_sensor_notice_get(&n);
    return n;
}

std::string text(language_t lang = LANG_ENGLISH) {
    ext_sensor_notice_t n = get();
    char buf[256];
    ext_sensor_notice_format(buf, sizeof(buf), &n, lang);
    return buf;
}

void test_outage_and_recovery() {
    ext_sensor_notice_clear();
    CHECK(!get().pending);
    CHECK_EQ_STR(text(), "");

    uint32_t rev = ext_sensor_notice_revision();
    ext_sensor_notice_record_lost(0);
    CHECK(ext_sensor_notice_revision() != rev);
    CHECK(get().pending);
    CHECK(get().ongoing);
    CHECK(get().count == 1);
    CHECK_EQ_STR(text(), "Temperature sensor not responding \xE2\x80\x93 COP using heat pump sensors");

    // The return sensor on the same Thermux goes too: still one outage.
    ext_sensor_notice_record_lost(1);
    CHECK(get().count == 1);
    ext_sensor_notice_record_restored(0, 1500);
    CHECK(get().ongoing);  // return is still down
    ext_sensor_notice_record_restored(1, 2700);
    CHECK(!get().ongoing);
    CHECK(get().pending);
    CHECK(get().last_down_s == 2700);
    CHECK_EQ_STR(text(), "Temperature sensor was offline for 45 min");
    CHECK_EQ_STR(text(LANG_FRENCH),
                 "Sonde de temp\xC3\xA9rature hors ligne pendant 45 min");
}

void test_repeat_outages_are_one_entry() {
    ext_sensor_notice_clear();
    for (int i = 0; i < 3; ++i) {
        ext_sensor_notice_record_lost(0);
        ext_sensor_notice_record_restored(0, 3900);
    }
    CHECK(get().count == 3);
    CHECK_EQ_STR(text(), "Temperature sensor went offline 3 times \xE2\x80\x93 latest for 1 h 05 min");
    // A duplicate report changes nothing.
    ext_sensor_notice_record_restored(0, 10);
    CHECK(get().count == 3);
    CHECK(get().last_down_s == 3900);
}

void test_acknowledged_outage_stays_acknowledged() {
    ext_sensor_notice_clear();
    ext_sensor_notice_record_lost(0);
    ext_sensor_notice_clear();
    CHECK(!get().pending);
    ext_sensor_notice_record_lost(1);  // same outage, other slot
    ext_sensor_notice_record_restored(0, 600);
    ext_sensor_notice_record_restored(1, 600);
    CHECK(!get().pending);
    // The next outage raises it again.
    ext_sensor_notice_record_lost(0);
    CHECK(get().pending);
    CHECK(get().count == 1);
}

void test_settings_change_drops_ongoing_outage() {
    ext_sensor_notice_clear();
    ext_sensor_notice_record_lost(0);
    ext_sensor_notice_settings_changed();
    CHECK(!get().pending);
    // No stray recovery for the old sensor.
    ext_sensor_notice_record_restored(0, 600);
    CHECK(!get().pending);

    // A finished outage is kept: it really happened.
    ext_sensor_notice_record_lost(0);
    ext_sensor_notice_record_restored(0, 600);
    ext_sensor_notice_settings_changed();
    CHECK(get().pending);
    CHECK_EQ_STR(text(), "Temperature sensor was offline for 10 min");
}

void test_short_outage_and_truncation() {
    ext_sensor_notice_clear();
    ext_sensor_notice_record_lost(0);
    ext_sensor_notice_record_restored(0, 5);
    CHECK_EQ_STR(text(), "Temperature sensor was offline for 1 min");
    ext_sensor_notice_t n = get();
    char tiny[8];
    size_t len = ext_sensor_notice_format(tiny, sizeof(tiny), &n, LANG_ENGLISH);
    CHECK(len == sizeof(tiny) - 1);
    CHECK(std::strlen(tiny) == sizeof(tiny) - 1);
}

}  // namespace

int main() {
    test_outage_and_recovery();
    test_repeat_outages_are_one_entry();
    test_acknowledged_outage_stays_acknowledged();
    test_settings_change_drops_ongoing_outage();
    test_short_outage_and_truncation();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("all ext_sensor_notice tests passed");
    return 0;
}
