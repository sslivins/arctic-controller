// Native unit tests for main/i18n/i18n.cpp -- the first source compiled and
// executed on the host rather than merely grepped.
//
// Why this matters: tests/api/test_i18n_completeness.py proves the three
// string tables are fully populated, but it reads the file as text. It cannot
// tell you what i18n_get() does with an out-of-range id, whether the English
// fallback is reachable, or whether a language choice survives a reboot --
// those are runtime behaviours, and until now the only way to reach them was
// to drive the single physical controller.
//
// Framework-free: prints failures, non-zero exit. Matches the style of
// components/arctic-macon/tests/.

#include "i18n.h"
#include "nvs_fake.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

#define CHECK_STR(actual, expected)                                            \
    do {                                                                       \
        const char *a_ = (actual);                                             \
        const char *e_ = (expected);                                           \
        if (a_ == nullptr || e_ == nullptr || std::strcmp(a_, e_) != 0) {      \
            std::printf("FAIL %s:%d: %s == \"%s\", expected \"%s\"\n",         \
                        __FILE__, __LINE__, #actual,                           \
                        a_ ? a_ : "(null)", e_ ? e_ : "(null)");               \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

static const char *NS = "settings";
static const char *KEY = "language";

// --------------------------------------------------------------------------
// i18n_get: lookup, bounds, and the English fallback
// --------------------------------------------------------------------------
static void test_get_returns_the_current_language(void) {
    i18n_set_language(LANG_ENGLISH);
    CHECK_STR(i18n_get(STR_CANCEL), "Cancel");

    i18n_set_language(LANG_FRENCH);
    const char *fr = i18n_get(STR_CANCEL);
    CHECK(fr != nullptr && std::strcmp(fr, "Cancel") != 0);

    i18n_set_language(LANG_SPANISH);
    const char *es = i18n_get(STR_CANCEL);
    CHECK(es != nullptr);
    CHECK(fr != nullptr && es != nullptr && std::strcmp(fr, es) != 0);

    i18n_set_language(LANG_ENGLISH);
}

static void test_get_never_returns_null(void) {
    // Callers pass the result straight to LVGL, which dereferences it. A NULL
    // here is a crash on the device, so the contract is "never NULL" even for
    // an id that does not exist.
    //
    // Note on the English-fallback branch inside i18n_get: it is currently
    // unreachable, because all three tables are fully populated and the only
    // blank entries are empty strings rather than NULL. Mutation-testing
    // confirms removing that branch breaks nothing here. It is a safety net
    // for a table that has a hole; what actually guarantees there is no hole is
    // tests/api/test_i18n_completeness.py. Do not delete the branch on the
    // strength of its coverage -- delete the lint first, if ever.
    for (int lang = 0; lang < LANG_COUNT; ++lang) {
        i18n_set_language((language_t)lang);
        for (int id = 0; id < STR_COUNT; ++id) {
            if (i18n_get((string_id_t)id) == nullptr) {
                std::printf("FAIL: i18n_get(%d) returned NULL in language %d\n", id, lang);
                ++g_failures;
                return;  // one report is enough; do not spam 359 lines
            }
        }
    }
    i18n_set_language(LANG_ENGLISH);
}

static void test_out_of_range_id_is_reported_not_read(void) {
    // Out-of-range must be visible rather than silently blank, and must not
    // index past the end of the table.
    CHECK_STR(i18n_get(STR_COUNT), "???");
    CHECK_STR(i18n_get((string_id_t)(STR_COUNT + 1)), "???");
    CHECK_STR(i18n_get((string_id_t)9999), "???");
}

static void test_every_string_is_non_empty_in_every_language(void) {
    // The completeness lint asserts this over the source text; assert it again
    // over what the program actually returns, which is what users see.
    int empty = 0;
    for (int lang = 0; lang < LANG_COUNT; ++lang) {
        i18n_set_language((language_t)lang);
        for (int id = 0; id < STR_COUNT; ++id) {
            const char *s = i18n_get((string_id_t)id);
            if (s && s[0] == '\0') {
                ++empty;
            }
        }
    }
    // The two STR_HP_RESERVED_* slots are deliberately blank in all three
    // languages (see main/i18n/strings.h); anything beyond that is a gap.
    if (empty != LANG_COUNT * 2) {
        std::printf("FAIL: %d empty strings across all languages, expected %d "
                    "(the reserved slots only)\n", empty, LANG_COUNT * 2);
        ++g_failures;
    }
    i18n_set_language(LANG_ENGLISH);
}

// --------------------------------------------------------------------------
// Language selection and bounds
// --------------------------------------------------------------------------
static void test_set_language_rejects_out_of_range(void) {
    i18n_set_language(LANG_FRENCH);
    i18n_set_language(LANG_COUNT);            // must be ignored
    CHECK(i18n_get_language() == LANG_FRENCH);
    i18n_set_language((language_t)255);       // must be ignored
    CHECK(i18n_get_language() == LANG_FRENCH);
    i18n_set_language(LANG_ENGLISH);
}

static void test_language_names(void) {
    // Native names are the same regardless of the current UI language.
    i18n_set_language(LANG_SPANISH);
    const char *fr_native = i18n_get_language_name(LANG_FRENCH);
    i18n_set_language(LANG_ENGLISH);
    CHECK_STR(i18n_get_language_name(LANG_FRENCH), fr_native);

    // ...whereas the localized name follows the current UI language.
    const char *en_view = i18n_get_language_name_localized(LANG_ENGLISH);
    CHECK_STR(en_view, "English");
    i18n_set_language(LANG_FRENCH);
    CHECK(std::strcmp(i18n_get_language_name_localized(LANG_ENGLISH), "English") != 0);
    i18n_set_language(LANG_ENGLISH);

    CHECK_STR(i18n_get_language_name(LANG_COUNT), "???");
    CHECK_STR(i18n_get_language_name_localized(LANG_COUNT), "???");
}

// --------------------------------------------------------------------------
// English <-> localized round-tripping
// --------------------------------------------------------------------------
static void test_find_by_english(void) {
    CHECK(i18n_find_by_english("Cancel") == STR_CANCEL);
    CHECK(i18n_find_by_english("no such string anywhere") == STR_COUNT);
    CHECK(i18n_find_by_english(nullptr) == STR_COUNT);
    // Lookup is exact, not prefix or case-insensitive.
    CHECK(i18n_find_by_english("cancel") == STR_COUNT);
    CHECK(i18n_find_by_english("Cance") == STR_COUNT);
}

static void test_translate_passes_unknown_text_through(void) {
    i18n_set_language(LANG_FRENCH);
    const char *unknown = "Serial number 12345";
    CHECK_STR(i18n_translate(unknown), unknown);
    CHECK(i18n_translate(unknown) == unknown);  // same pointer: no copy, no leak

    // A known string is translated away from English.
    CHECK(std::strcmp(i18n_translate("Cancel"), "Cancel") != 0);
    i18n_set_language(LANG_ENGLISH);
    CHECK_STR(i18n_translate("Cancel"), "Cancel");
}

static void test_get_english_round_trips(void) {
    i18n_set_language(LANG_FRENCH);
    const char *fr = i18n_get(STR_CANCEL);
    CHECK_STR(i18n_get_english(fr), "Cancel");

    i18n_set_language(LANG_SPANISH);
    const char *es = i18n_get(STR_CANCEL);
    CHECK_STR(i18n_get_english(es), "Cancel");

    // English in, English out.
    CHECK_STR(i18n_get_english("Cancel"), "Cancel");
    // Unknown text is reported as unknown rather than echoed, which is how
    // callers distinguish "not a UI string" from "already English".
    CHECK(i18n_get_english("not a ui string") == nullptr);
    CHECK(i18n_get_english(nullptr) == nullptr);
    i18n_set_language(LANG_ENGLISH);
}

// --------------------------------------------------------------------------
// Keyed (library-sourced) translations
// --------------------------------------------------------------------------
static void test_get_key_fallback_rules(void) {
    const char *fallback = "Frequency ratio K1";

    // English always resolves to the library's own text.
    i18n_set_language(LANG_ENGLISH);
    CHECK(i18n_get_key("ap.freq_ratio_k1.name", fallback) == fallback);

    // A NULL key means the library had no translatable id.
    i18n_set_language(LANG_FRENCH);
    CHECK(i18n_get_key(nullptr, fallback) == fallback);

    // An unknown key falls back rather than returning NULL or an empty string.
    CHECK(i18n_get_key("no.such.key.at.all", fallback) == fallback);

    // A NULL fallback is propagated, not turned into a bogus string: the
    // caller asked for something that does not exist.
    CHECK(i18n_get_key("no.such.key.at.all", nullptr) == nullptr);

    i18n_set_language(LANG_ENGLISH);
}

static void test_get_key_translates_a_known_key(void) {
    // Prove at least one key really is translated, otherwise every assertion
    // above would also pass against a function that only ever falls back.
    const char *fallback = "PLACEHOLDER-ENGLISH";
    int translated = 0;
    static const char *candidates[] = {
        "ap.freq_ratio_k1.name",
        "ap.freq_ratio_k1.detail",
    };
    for (int lang = LANG_FRENCH; lang <= LANG_SPANISH; ++lang) {
        i18n_set_language((language_t)lang);
        for (const char *key : candidates) {
            const char *t = i18n_get_key(key, fallback);
            if (t != nullptr && std::strcmp(t, fallback) != 0) {
                ++translated;
            }
        }
    }
    if (translated == 0) {
        std::printf("FAIL: no keyed translation resolved in FR or ES; the keyed "
                    "table is empty or its keys were renamed\n");
        ++g_failures;
    }
    i18n_set_language(LANG_ENGLISH);
}

// --------------------------------------------------------------------------
// Persistence
// --------------------------------------------------------------------------
static void test_language_choice_is_persisted(void) {
    nvs_fake::reset();
    i18n_set_language(LANG_SPANISH);

    uint8_t stored = 0xff;
    CHECK(nvs_fake::peek_u8(NS, KEY, &stored));
    CHECK(stored == (uint8_t)LANG_SPANISH);
    i18n_set_language(LANG_ENGLISH);
}

static void test_init_restores_the_saved_language(void) {
    // Order matters: i18n_set_language persists, so the in-memory language must
    // be put back to its static-init value BEFORE seeding, or the seed is
    // overwritten by the very call meant to simulate a fresh boot.
    i18n_set_language(LANG_ENGLISH);                   // as static init leaves it
    nvs_fake::reset();
    nvs_fake::seed_u8(NS, KEY, (uint8_t)LANG_FRENCH);  // as a previous boot left it

    i18n_init();
    CHECK(i18n_get_language() == LANG_FRENCH);
    i18n_set_language(LANG_ENGLISH);
}

static void test_init_survives_a_factory_fresh_device(void) {
    // Namespace never written: nvs_open(READONLY) returns NOT_FOUND. Booting
    // must fall back to English rather than failing or leaving it unset.
    nvs_fake::reset();
    i18n_set_language(LANG_ENGLISH);
    i18n_init();
    CHECK(i18n_get_language() == LANG_ENGLISH);
}

static void test_init_ignores_a_corrupt_stored_value(void) {
    // A value out of range (corrupt flash, or a downgrade after a language was
    // added) must not index past the end of string_tables.
    i18n_set_language(LANG_ENGLISH);
    nvs_fake::reset();
    nvs_fake::seed_u8(NS, KEY, 200);
    i18n_init();
    CHECK(i18n_get_language() == LANG_ENGLISH);
    CHECK_STR(i18n_get(STR_CANCEL), "Cancel");
}

static void test_nvs_failure_does_not_crash_but_does_lose_the_setting(void) {
    // Documents current behaviour rather than endorsing it: i18n_set_language
    // ignores the return of nvs_set_u8/nvs_commit, so when flash rejects the
    // write the UI switches language and the choice is silently lost at the
    // next boot. Low severity for a language preference, but this test exists
    // so that the day it is handled, the change is visible here.
    nvs_fake::reset();
    nvs_fake::fail_next(nvs_fake::Op::SetU8, ESP_ERR_NVS_NOT_ENOUGH_SPACE, -1);

    i18n_set_language(LANG_FRENCH);
    CHECK(i18n_get_language() == LANG_FRENCH);       // UI switched
    uint8_t stored = 0;
    CHECK(!nvs_fake::peek_u8(NS, KEY, &stored));      // ...but nothing persisted

    nvs_fake::clear_failures();
    i18n_set_language(LANG_ENGLISH);
    i18n_init();
    CHECK(i18n_get_language() == LANG_ENGLISH);       // the choice did not survive
    nvs_fake::reset();
}

static void test_open_failure_is_tolerated(void) {
    nvs_fake::reset();
    nvs_fake::fail_next(nvs_fake::Op::Open, ESP_FAIL, -1);
    i18n_set_language(LANG_SPANISH);      // must not crash
    CHECK(i18n_get_language() == LANG_SPANISH);
    i18n_init();                          // must not crash
    nvs_fake::clear_failures();
    i18n_set_language(LANG_ENGLISH);
    nvs_fake::reset();
}

int main(void) {
    nvs_fake::reset();

    test_get_returns_the_current_language();
    test_get_never_returns_null();
    test_out_of_range_id_is_reported_not_read();
    test_every_string_is_non_empty_in_every_language();

    test_set_language_rejects_out_of_range();
    test_language_names();

    test_find_by_english();
    test_translate_passes_unknown_text_through();
    test_get_english_round_trips();

    test_get_key_fallback_rules();
    test_get_key_translates_a_known_key();

    test_language_choice_is_persisted();
    test_init_restores_the_saved_language();
    test_init_survives_a_factory_fresh_device();
    test_init_ignores_a_corrupt_stored_value();
    test_nvs_failure_does_not_crash_but_does_lose_the_setting();
    test_open_failure_is_tolerated();

    if (g_failures) {
        std::printf("%d i18n check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("i18n: all checks passed\n");
    return 0;
}
