// Native unit tests for main/app_preferences.cpp -- the unit setting and the
// eight temperature conversions every screen and every API response goes
// through.
//
// Why this is worth a suite of its own. The conversions are the last thing to
// touch a number before a human reads it or before a human-entered number
// becomes a setpoint, and they split into two families that look identical and
// are not: absolute temperatures carry the +32 offset, differentials do not.
// Getting that backwards is not a crash and not an error. A 2 C hysteresis
// displayed and re-entered as a 36 F hysteresis is a heat pump that
// short-cycles or never cycles, reported by the owner weeks later as "it feels
// wrong". Nothing in the device or API suites can see it either, because both
// read values back through the same conversion that produced them -- a
// consistent pair of wrong functions round-trips perfectly.
//
// So the conversions here are checked against independently stated expected
// values (a table of known C/F pairs, and hand-computed differentials), never
// against each other, and the round-trip properties are asserted separately as
// properties rather than used as the definition of correctness.
//
// The unit strings are asserted as explicit UTF-8 byte sequences rather than
// as literals. The degree sign and the delta are multi-byte, they are rendered
// on an LVGL display and shipped in JSON, and "the right character in the
// wrong encoding" is exactly the failure that looks fine in a source diff.
//
// Each scenario runs in a forked child because app_prefs_init() is single-shot
// per process (the `initialized` guard) and the cached unit is process-wide
// static state -- one child is one boot.
//
// Refs #217 (T13).

#include "nvs_fake.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "app_preferences.h"

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

#define CHECK_NEAR(actual, expected, tol)                                     \
    do {                                                                      \
        double a_ = (double)(actual);                                         \
        double e_ = (double)(expected);                                       \
        if (std::fabs(a_ - e_) > (tol)) {                                     \
            std::fprintf(stderr, "  FAIL %s:%d: %s -> %f, expected %f\n",     \
                         __FILE__, __LINE__, #actual, a_, e_);                \
            ++g_failures;                                                     \
        }                                                                     \
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

// Must match app_preferences.cpp. Duplicated deliberately: a rename here is a
// silent settings reset on every deployed device, so the tests should fail
// loudly rather than follow it.
constexpr const char* NS = "app_prefs";
constexpr const char* KEY_DEMO = "demo_mode";
constexpr const char* KEY_UNIT = "temp_unit";

// The degree sign is U+00B0 and the delta is U+0394. Spelling them as bytes
// keeps this file pure ASCII and makes the assertion about the encoding, not
// just about the glyph.
constexpr const char* DEG_C = "\xc2\xb0" "C";
constexpr const char* DEG_F = "\xc2\xb0" "F";
constexpr const char* DELTA_DEG_F = "\xce\x94" "\xc2\xb0" "F";

void blank_device() { nvs_fake::reset(); }

// A device whose last boot left these settings behind.
void previously(bool demo, uint8_t unit_byte) {
    nvs_fake::reset();
    nvs_fake::seed_u8(NS, KEY_DEMO, demo ? 1 : 0);
    nvs_fake::seed_u8(NS, KEY_UNIT, unit_byte);
}

uint8_t stored(const char* key) {
    uint8_t v = 0xFF;
    if (!nvs_fake::peek_u8(NS, key, &v)) return 0xFF;
    return v;
}

void in_fahrenheit() {
    blank_device();
    app_prefs_init();
    app_prefs_set_temp_unit(TEMP_UNIT_FAHRENHEIT);
}

void in_celsius() {
    blank_device();
    app_prefs_init();
}

// Known-good absolute pairs, stated independently of the implementation.
// -40 is the fixed point of the two scales and the one value that cannot be
// produced by a missing or duplicated offset.
struct AbsPair {
    int16_t celsius;
    int16_t fahrenheit;  // rounded half away from zero
    float exact_f;
};

constexpr AbsPair kAbs[] = {
    {-40, -40, -40.0f},   // the scales cross
    {-30, -22, -22.0f},
    {-18, 0, -0.4f},      // rounds toward zero from a negative
    {-17, 1, 1.4f},
    {-10, 14, 14.0f},
    {-5, 23, 23.0f},
    {0, 32, 32.0f},       // freezing
    {1, 34, 33.8f},
    {5, 41, 41.0f},
    {10, 50, 50.0f},
    {16, 61, 60.8f},
    {20, 68, 68.0f},
    {21, 70, 69.8f},
    {22, 72, 71.6f},
    {25, 77, 77.0f},
    {30, 86, 86.0f},
    {37, 99, 98.6f},      // body temperature, rounds up
    {45, 113, 113.0f},
    {60, 140, 140.0f},
    {100, 212, 212.0f},   // boiling
};

// Differentials: the same scale factor with NO offset. Stated separately so a
// copy of the absolute table cannot accidentally define these too.
struct DiffPair {
    int16_t celsius;
    int16_t fahrenheit;
    float exact_f;
};

constexpr DiffPair kDiff[] = {
    {0, 0, 0.0f},
    {1, 2, 1.8f},        // rounds up
    {2, 4, 3.6f},
    {3, 5, 5.4f},        // rounds down
    {4, 7, 7.2f},
    {5, 9, 9.0f},
    {10, 18, 18.0f},
    {-1, -2, -1.8f},     // away from zero, not toward it
    {-3, -5, -5.4f},
    {-5, -9, -9.0f},
    {-10, -18, -18.0f},
};

// ------------------------------------------------------------------------
// Defaults and loading
// ------------------------------------------------------------------------
void a_blank_device_reports_celsius() {
    blank_device();
    app_prefs_init();

    // Celsius is the safe default: it is what the heat pump's own registers
    // are in, so an unconfigured unit shows raw values rather than converted
    // ones.
    CHECK_EQ_INT(app_prefs_get_temp_unit(), TEMP_UNIT_CELSIUS);
}

void a_blank_device_writes_nothing_at_boot() {
    blank_device();
    app_prefs_init();

    // Startup must not burn an NVS page. This runs on every boot.
    CHECK_EQ_INT(nvs_fake::call_count(nvs_fake::Op::SetU8), 0);
    CHECK_EQ_INT(nvs_fake::call_count(nvs_fake::Op::Commit), 0);

    // Nor create the namespace merely to read it. Opening read-only is what
    // makes that true: NVS refuses a read-only open of a namespace that has
    // never been written, so no key is even looked up on a factory-fresh
    // device. A read-write open here would succeed and allocate the entry.
    CHECK_EQ_INT(nvs_fake::call_count(nvs_fake::Op::GetU8), 0);
}

void a_saved_fahrenheit_setting_is_restored() {
    previously(false, 1);
    app_prefs_init();

    CHECK_EQ_INT(app_prefs_get_temp_unit(), TEMP_UNIT_FAHRENHEIT);
    CHECK_EQ_INT(app_prefs_convert_temp(0), 32);
}

void a_saved_celsius_setting_is_restored() {
    previously(false, 0);
    app_prefs_init();

    CHECK_EQ_INT(app_prefs_get_temp_unit(), TEMP_UNIT_CELSIUS);
    CHECK_EQ_INT(app_prefs_convert_temp(0), 0);
}

void a_corrupt_unit_byte_falls_back_to_celsius() {
    // Only the exact byte 1 means Fahrenheit. Anything else -- a partially
    // written page, a value from a future firmware with more units -- must
    // land on the safe default rather than on whatever the enum cast produces.
    previously(false, 200);
    app_prefs_init();

    CHECK_EQ_INT(app_prefs_get_temp_unit(), TEMP_UNIT_CELSIUS);
}

void an_unopenable_nvs_still_boots_with_defaults() {
    blank_device();
    nvs_fake::fail_next(nvs_fake::Op::Open, ESP_ERR_NVS_NOT_FOUND);
    app_prefs_init();

    CHECK_EQ_INT(app_prefs_get_temp_unit(), TEMP_UNIT_CELSIUS);
    CHECK(app_prefs_is_demo_mode() == false);
}

void a_second_init_does_not_reload() {
    previously(false, 1);
    app_prefs_init();
    CHECK_EQ_INT(app_prefs_get_temp_unit(), TEMP_UNIT_FAHRENHEIT);

    // A later component calling init again must not undo a change the user
    // made in between. The change is made with flash unavailable on purpose:
    // if the user's choice were also written back, a re-reading init would
    // simply read the new value and agree by accident, and the guard could be
    // deleted without any test noticing. Here RAM says Celsius and flash still
    // says Fahrenheit, so a reload is visible.
    nvs_fake::fail_next(nvs_fake::Op::Open, ESP_ERR_NVS_NOT_FOUND);
    app_prefs_set_temp_unit(TEMP_UNIT_CELSIUS);
    CHECK_EQ_INT(stored(KEY_UNIT), 1);

    app_prefs_init();
    CHECK_EQ_INT(app_prefs_get_temp_unit(), TEMP_UNIT_CELSIUS);
}

// ------------------------------------------------------------------------
// Persisting a change
// ------------------------------------------------------------------------
void choosing_fahrenheit_is_persisted() {
    blank_device();
    app_prefs_init();
    app_prefs_set_temp_unit(TEMP_UNIT_FAHRENHEIT);

    CHECK_EQ_INT(app_prefs_get_temp_unit(), TEMP_UNIT_FAHRENHEIT);
    // In flash, not just in RAM: the setting has to survive the reboot.
    CHECK_EQ_INT(stored(KEY_UNIT), 1);
    CHECK(nvs_fake::call_count(nvs_fake::Op::Commit) > 0);
}

void choosing_celsius_again_is_persisted() {
    in_fahrenheit();
    app_prefs_set_temp_unit(TEMP_UNIT_CELSIUS);

    // Persisting 0 matters as much as persisting 1: if the write were skipped
    // the device would come back in Fahrenheit after every reboot.
    CHECK_EQ_INT(stored(KEY_UNIT), 0);
    CHECK_EQ_INT(app_prefs_get_temp_unit(), TEMP_UNIT_CELSIUS);
}

void re_choosing_the_current_unit_writes_nothing() {
    blank_device();
    app_prefs_init();
    app_prefs_set_temp_unit(TEMP_UNIT_CELSIUS);

    CHECK_EQ_INT(nvs_fake::call_count(nvs_fake::Op::SetU8), 0);
}

void a_failed_write_still_changes_the_displayed_unit() {
    blank_device();
    app_prefs_init();
    nvs_fake::fail_next(nvs_fake::Op::Open, ESP_ERR_NVS_NOT_FOUND);
    app_prefs_set_temp_unit(TEMP_UNIT_FAHRENHEIT);

    // Deliberate: the user pressed the button, so the screen changes now and
    // the setting is simply lost at the next reboot. The alternative -- a
    // control that silently does nothing when flash is full -- is worse.
    CHECK_EQ_INT(app_prefs_get_temp_unit(), TEMP_UNIT_FAHRENHEIT);
    CHECK_EQ_INT(stored(KEY_UNIT), 0xFF);
}

void a_retry_after_a_failed_write_cannot_persist() {
    // Consequence of the early return: the cached value already matches, so
    // setting it again is a no-op and the failed write is never retried. This
    // is asserted so the behaviour is a known property rather than a surprise
    // during a support call.
    blank_device();
    app_prefs_init();
    nvs_fake::fail_next(nvs_fake::Op::Open, ESP_ERR_NVS_NOT_FOUND);
    app_prefs_set_temp_unit(TEMP_UNIT_FAHRENHEIT);
    app_prefs_set_temp_unit(TEMP_UNIT_FAHRENHEIT);

    CHECK_EQ_INT(stored(KEY_UNIT), 0xFF);
}

// ------------------------------------------------------------------------
// Demo mode
// ------------------------------------------------------------------------
void demo_mode_is_off_on_a_blank_device() {
    blank_device();
    app_prefs_init();

    CHECK(app_prefs_is_demo_mode() == false);
}

void a_saved_demo_flag_is_restored() {
    previously(true, 0);
    app_prefs_init();

    CHECK(app_prefs_is_demo_mode() == true);
}

void enabling_demo_mode_is_persisted() {
    blank_device();
    app_prefs_init();
    app_prefs_set_demo_mode(true);

    CHECK(app_prefs_is_demo_mode() == true);
    CHECK_EQ_INT(stored(KEY_DEMO), 1);
}

void disabling_demo_mode_is_persisted() {
    previously(true, 0);
    app_prefs_init();
    app_prefs_set_demo_mode(false);

    // A device left in demo mode reports fabricated temperatures. Failing to
    // persist the exit would put it straight back into simulation after a
    // power cut, with nothing on screen to say so.
    CHECK(app_prefs_is_demo_mode() == false);
    CHECK_EQ_INT(stored(KEY_DEMO), 0);
}

void re_setting_demo_mode_writes_nothing() {
    blank_device();
    app_prefs_init();
    app_prefs_set_demo_mode(false);

    CHECK_EQ_INT(nvs_fake::call_count(nvs_fake::Op::SetU8), 0);
}

void demo_mode_and_the_unit_are_independent() {
    previously(true, 1);
    app_prefs_init();
    app_prefs_set_temp_unit(TEMP_UNIT_CELSIUS);

    // One namespace, two keys: writing one must not clear the other.
    CHECK(app_prefs_is_demo_mode() == true);
    CHECK_EQ_INT(stored(KEY_DEMO), 1);
    CHECK_EQ_INT(stored(KEY_UNIT), 0);
}

// ------------------------------------------------------------------------
// Celsius: every conversion is a pass-through
// ------------------------------------------------------------------------
void in_celsius_absolute_values_are_unchanged() {
    in_celsius();
    for (const AbsPair& p : kAbs) {
        CHECK_EQ_INT(app_prefs_convert_temp(p.celsius), p.celsius);
        CHECK_NEAR(app_prefs_convert_temp_f(p.celsius), (float)p.celsius, 0.001);
        CHECK_EQ_INT(app_prefs_temp_to_celsius(p.celsius), p.celsius);
    }
}

void in_celsius_differentials_are_unchanged() {
    in_celsius();
    for (const DiffPair& p : kDiff) {
        CHECK_EQ_INT(app_prefs_convert_temp_diff(p.celsius), p.celsius);
        CHECK_NEAR(app_prefs_convert_temp_diff_f(p.celsius), (float)p.celsius,
                   0.001);
        CHECK_EQ_INT(app_prefs_temp_diff_to_celsius(p.celsius), p.celsius);
    }
}

void in_celsius_a_float_entry_is_only_rounded() {
    in_celsius();

    // The float-input variants exist because the on-screen editor works in
    // tenths. In Celsius there is nothing to convert, but the value still has
    // to be rounded to the integer the register takes -- half away from zero,
    // including on the negative side.
    CHECK_EQ_INT(app_prefs_temp_to_celsius_from_f(21.4f), 21);
    CHECK_EQ_INT(app_prefs_temp_to_celsius_from_f(21.5f), 22);
    CHECK_EQ_INT(app_prefs_temp_to_celsius_from_f(-3.5f), -4);
    CHECK_EQ_INT(app_prefs_temp_to_celsius_from_f(-3.4f), -3);
    CHECK_EQ_INT(app_prefs_temp_diff_to_celsius_from_f(2.5f), 3);
    CHECK_EQ_INT(app_prefs_temp_diff_to_celsius_from_f(-2.5f), -3);
}

// ------------------------------------------------------------------------
// Fahrenheit: absolute temperatures
// ------------------------------------------------------------------------
void absolute_temperatures_match_the_known_pairs() {
    in_fahrenheit();
    for (const AbsPair& p : kAbs) {
        CHECK_EQ_INT(app_prefs_convert_temp(p.celsius), p.fahrenheit);
        CHECK_NEAR(app_prefs_convert_temp_f(p.celsius), p.exact_f, 0.01);
    }
}

void the_two_scales_cross_at_minus_forty() {
    in_fahrenheit();

    // The only temperature where the offset is invisible. Asserted on its own
    // so that a suite reduced to this one value would still be obviously
    // insufficient.
    CHECK_EQ_INT(app_prefs_convert_temp(-40), -40);
    CHECK_EQ_INT(app_prefs_temp_to_celsius(-40), -40);
}

void absolute_temperatures_convert_back() {
    in_fahrenheit();
    for (const AbsPair& p : kAbs) {
        CHECK_EQ_INT(app_prefs_temp_to_celsius(p.fahrenheit), p.celsius);
    }
}

void a_negative_fahrenheit_result_rounds_away_from_zero() {
    in_fahrenheit();

    // 20 F is -6.67 C. A truncating cast gives -6, which is a whole degree of
    // setpoint error in the direction that matters on a heat pump. roundf is
    // half away from zero; the cast is toward it, and the two only disagree on
    // negatives, which is why this needs its own scenario.
    CHECK_EQ_INT(app_prefs_temp_to_celsius(20), -7);
    CHECK_EQ_INT(app_prefs_temp_to_celsius(0), -18);
    CHECK_EQ_INT(app_prefs_temp_to_celsius(-10), -23);
}

void a_float_fahrenheit_entry_is_converted_and_rounded() {
    in_fahrenheit();

    CHECK_EQ_INT(app_prefs_temp_to_celsius_from_f(70.0f), 21);
    CHECK_EQ_INT(app_prefs_temp_to_celsius_from_f(71.6f), 22);
    CHECK_EQ_INT(app_prefs_temp_to_celsius_from_f(32.0f), 0);
    CHECK_EQ_INT(app_prefs_temp_to_celsius_from_f(-40.0f), -40);
    CHECK_EQ_INT(app_prefs_temp_to_celsius_from_f(20.0f), -7);
}

// ------------------------------------------------------------------------
// Fahrenheit: differentials, which must NOT carry the offset
// ------------------------------------------------------------------------
void differentials_match_the_known_pairs() {
    in_fahrenheit();
    for (const DiffPair& p : kDiff) {
        CHECK_EQ_INT(app_prefs_convert_temp_diff(p.celsius), p.fahrenheit);
        CHECK_NEAR(app_prefs_convert_temp_diff_f(p.celsius), p.exact_f, 0.01);
    }
}

void a_zero_differential_stays_zero() {
    in_fahrenheit();

    // The single sharpest check in the file. If the absolute formula is used
    // for a differential, "no difference" becomes 32 -- a hysteresis the unit
    // can never satisfy, so it either never cycles or never stops.
    CHECK_EQ_INT(app_prefs_convert_temp_diff(0), 0);
    CHECK_NEAR(app_prefs_convert_temp_diff_f(0), 0.0f, 0.001);
    CHECK_EQ_INT(app_prefs_temp_diff_to_celsius(0), 0);
    CHECK_EQ_INT(app_prefs_temp_diff_to_celsius_from_f(0.0f), 0);
}

void a_differential_is_never_the_absolute_conversion() {
    in_fahrenheit();

    // Stated as a property over the whole working range rather than as a pair
    // of tables, because the two families are otherwise easy to swap at a call
    // site and each table would still pass.
    for (int16_t c = -50; c <= 100; ++c) {
        int16_t abs_f = app_prefs_convert_temp(c);
        int16_t diff_f = app_prefs_convert_temp_diff(c);
        CHECK_EQ_INT(abs_f - diff_f, 32);
    }
}

void differentials_convert_back() {
    in_fahrenheit();
    for (const DiffPair& p : kDiff) {
        CHECK_EQ_INT(app_prefs_temp_diff_to_celsius(p.fahrenheit), p.celsius);
    }
}

void a_float_differential_entry_is_converted_and_rounded() {
    in_fahrenheit();

    CHECK_EQ_INT(app_prefs_temp_diff_to_celsius_from_f(1.8f), 1);
    CHECK_EQ_INT(app_prefs_temp_diff_to_celsius_from_f(9.0f), 5);
    CHECK_EQ_INT(app_prefs_temp_diff_to_celsius_from_f(-5.4f), -3);
    CHECK_EQ_INT(app_prefs_temp_diff_to_celsius_from_f(0.9f), 1);
    CHECK_EQ_INT(app_prefs_temp_diff_to_celsius_from_f(-0.9f), -1);
}

// ------------------------------------------------------------------------
// Round-trip properties
// ------------------------------------------------------------------------
void a_celsius_value_survives_a_round_trip() {
    in_fahrenheit();

    // One Celsius degree is 1.8 Fahrenheit degrees, so the integer Fahrenheit
    // scale is strictly finer and nothing is lost going out and back. This is
    // what makes it safe for a screen to show a converted value and send back
    // whatever the user leaves in the field untouched.
    for (int16_t c = -60; c <= 120; ++c) {
        CHECK_EQ_INT(app_prefs_temp_to_celsius(app_prefs_convert_temp(c)), c);
    }
}

void a_celsius_differential_survives_a_round_trip() {
    in_fahrenheit();
    for (int16_t d = -40; d <= 40; ++d) {
        CHECK_EQ_INT(app_prefs_temp_diff_to_celsius(app_prefs_convert_temp_diff(d)),
                     d);
    }
}

void the_reverse_round_trip_is_lossy_but_bounded() {
    in_fahrenheit();

    // Going the other way cannot be exact -- roughly two Fahrenheit values
    // share each Celsius one -- so the property to hold is that the drift is
    // bounded by a single degree and never accumulates. Asserting this rather
    // than exactness stops someone "fixing" it with a lookup table that would
    // break the direction above.
    for (int16_t f = -60; f <= 120; ++f) {
        int16_t back = app_prefs_convert_temp(app_prefs_temp_to_celsius(f));
        CHECK(std::abs(back - f) <= 1);

        // Idempotent from the second trip onward: no drift under repeated
        // display-then-save cycles.
        int16_t again = app_prefs_convert_temp(app_prefs_temp_to_celsius(back));
        CHECK_EQ_INT(again, back);
    }
}

void the_float_and_integer_conversions_agree() {
    in_fahrenheit();

    // The float variants feed the editor and the integer ones feed the
    // register write. If they disagreed by more than rounding, a value would
    // change the moment the user opened and closed a field without editing it.
    for (int16_t c = -60; c <= 120; ++c) {
        CHECK_EQ_INT(app_prefs_convert_temp(c),
                     (int16_t)std::lround(app_prefs_convert_temp_f(c)));
        CHECK_EQ_INT(app_prefs_convert_temp_diff(c),
                     (int16_t)std::lround(app_prefs_convert_temp_diff_f(c)));
    }
}

void the_float_entry_paths_agree_with_the_integer_ones() {
    in_fahrenheit();
    for (int16_t f = -60; f <= 120; ++f) {
        CHECK_EQ_INT(app_prefs_temp_to_celsius_from_f((float)f),
                     app_prefs_temp_to_celsius(f));
        CHECK_EQ_INT(app_prefs_temp_diff_to_celsius_from_f((float)f),
                     app_prefs_temp_diff_to_celsius(f));
    }
}

// ------------------------------------------------------------------------
// Unit strings
// ------------------------------------------------------------------------
void the_unit_suffix_follows_the_setting() {
    in_celsius();
    CHECK_EQ_STR(app_prefs_temp_unit_str(), DEG_C);

    app_prefs_set_temp_unit(TEMP_UNIT_FAHRENHEIT);
    CHECK_EQ_STR(app_prefs_temp_unit_str(), DEG_F);
}

void the_differential_suffix_follows_the_setting() {
    in_celsius();
    CHECK_EQ_STR(app_prefs_temp_diff_unit_str(), DEG_C);

    app_prefs_set_temp_unit(TEMP_UNIT_FAHRENHEIT);
    // The delta prefix is how a Fahrenheit user can tell a 9 degree span from
    // a 9 degree temperature, which are wildly different numbers on that
    // scale.
    CHECK_EQ_STR(app_prefs_temp_diff_unit_str(), DELTA_DEG_F);
}

void the_suffixes_are_valid_utf8() {
    in_fahrenheit();

    // Byte-level, because these strings are handed straight to LVGL and to
    // JSON. A single-byte 0xB0 renders as a replacement glyph on the display
    // and makes the API response invalid UTF-8.
    const char* unit = app_prefs_temp_unit_str();
    CHECK_EQ_INT(std::strlen(unit), 3);
    CHECK_EQ_INT((unsigned char)unit[0], 0xC2);
    CHECK_EQ_INT((unsigned char)unit[1], 0xB0);

    const char* diff = app_prefs_temp_diff_unit_str();
    CHECK_EQ_INT(std::strlen(diff), 5);
    CHECK_EQ_INT((unsigned char)diff[0], 0xCE);
    CHECK_EQ_INT((unsigned char)diff[1], 0x94);
}

// ------------------------------------------------------------------------
// The setting actually reaches the conversions
// ------------------------------------------------------------------------
void switching_units_takes_effect_immediately() {
    in_celsius();
    CHECK_EQ_INT(app_prefs_convert_temp(20), 20);

    app_prefs_set_temp_unit(TEMP_UNIT_FAHRENHEIT);
    // No re-init, no reboot: the screen the user is looking at has to update.
    CHECK_EQ_INT(app_prefs_convert_temp(20), 68);

    app_prefs_set_temp_unit(TEMP_UNIT_CELSIUS);
    CHECK_EQ_INT(app_prefs_convert_temp(20), 20);
}

void a_saved_unit_reaches_the_conversions_at_boot() {
    previously(false, 1);
    app_prefs_init();

    // The link the other persistence scenarios do not make: that the byte read
    // out of flash is the one the conversions consult, not just the one
    // get_temp_unit() reports.
    CHECK_EQ_INT(app_prefs_convert_temp(100), 212);
    CHECK_EQ_INT(app_prefs_convert_temp_diff(5), 9);
    CHECK_EQ_STR(app_prefs_temp_unit_str(), DEG_F);
}

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

    SCENARIO("a blank device reports celsius", a_blank_device_reports_celsius);
    SCENARIO("a blank device writes nothing at boot",
             a_blank_device_writes_nothing_at_boot);
    SCENARIO("a saved fahrenheit setting is restored",
             a_saved_fahrenheit_setting_is_restored);
    SCENARIO("a saved celsius setting is restored",
             a_saved_celsius_setting_is_restored);
    SCENARIO("a corrupt unit byte falls back to celsius",
             a_corrupt_unit_byte_falls_back_to_celsius);
    SCENARIO("an unopenable nvs still boots with defaults",
             an_unopenable_nvs_still_boots_with_defaults);
    SCENARIO("a second init does not reload", a_second_init_does_not_reload);

    SCENARIO("choosing fahrenheit is persisted", choosing_fahrenheit_is_persisted);
    SCENARIO("choosing celsius again is persisted",
             choosing_celsius_again_is_persisted);
    SCENARIO("re-choosing the current unit writes nothing",
             re_choosing_the_current_unit_writes_nothing);
    SCENARIO("a failed write still changes the displayed unit",
             a_failed_write_still_changes_the_displayed_unit);
    SCENARIO("a retry after a failed write cannot persist",
             a_retry_after_a_failed_write_cannot_persist);

    SCENARIO("demo mode is off on a blank device",
             demo_mode_is_off_on_a_blank_device);
    SCENARIO("a saved demo flag is restored", a_saved_demo_flag_is_restored);
    SCENARIO("enabling demo mode is persisted", enabling_demo_mode_is_persisted);
    SCENARIO("disabling demo mode is persisted", disabling_demo_mode_is_persisted);
    SCENARIO("re-setting demo mode writes nothing",
             re_setting_demo_mode_writes_nothing);
    SCENARIO("demo mode and the unit are independent",
             demo_mode_and_the_unit_are_independent);

    SCENARIO("in celsius absolute values are unchanged",
             in_celsius_absolute_values_are_unchanged);
    SCENARIO("in celsius differentials are unchanged",
             in_celsius_differentials_are_unchanged);
    SCENARIO("in celsius a float entry is only rounded",
             in_celsius_a_float_entry_is_only_rounded);

    SCENARIO("absolute temperatures match the known pairs",
             absolute_temperatures_match_the_known_pairs);
    SCENARIO("the two scales cross at minus forty",
             the_two_scales_cross_at_minus_forty);
    SCENARIO("absolute temperatures convert back",
             absolute_temperatures_convert_back);
    SCENARIO("a negative fahrenheit result rounds away from zero",
             a_negative_fahrenheit_result_rounds_away_from_zero);
    SCENARIO("a float fahrenheit entry is converted and rounded",
             a_float_fahrenheit_entry_is_converted_and_rounded);

    SCENARIO("differentials match the known pairs",
             differentials_match_the_known_pairs);
    SCENARIO("a zero differential stays zero", a_zero_differential_stays_zero);
    SCENARIO("a differential is never the absolute conversion",
             a_differential_is_never_the_absolute_conversion);
    SCENARIO("differentials convert back", differentials_convert_back);
    SCENARIO("a float differential entry is converted and rounded",
             a_float_differential_entry_is_converted_and_rounded);

    SCENARIO("a celsius value survives a round trip",
             a_celsius_value_survives_a_round_trip);
    SCENARIO("a celsius differential survives a round trip",
             a_celsius_differential_survives_a_round_trip);
    SCENARIO("the reverse round trip is lossy but bounded",
             the_reverse_round_trip_is_lossy_but_bounded);
    SCENARIO("the float and integer conversions agree",
             the_float_and_integer_conversions_agree);
    SCENARIO("the float entry paths agree with the integer ones",
             the_float_entry_paths_agree_with_the_integer_ones);

    SCENARIO("the unit suffix follows the setting",
             the_unit_suffix_follows_the_setting);
    SCENARIO("the differential suffix follows the setting",
             the_differential_suffix_follows_the_setting);
    SCENARIO("the suffixes are valid utf8", the_suffixes_are_valid_utf8);

    SCENARIO("switching units takes effect immediately",
             switching_units_takes_effect_immediately);
    SCENARIO("a saved unit reaches the conversions at boot",
             a_saved_unit_reaches_the_conversions_at_boot);

#undef SCENARIO

    if (failed == 0) {
        std::printf("app_preferences: %d scenarios passed\n", total);
        std::fprintf(stderr, "app_preferences: %d scenarios passed\n", total);
        return 0;
    }
    std::fprintf(stderr, "app_preferences: %d of %d scenarios FAILED\n", failed,
                 total);
    return 1;
}
