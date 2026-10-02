// Native unit tests for main/fault_notice.cpp -- the "heat pump problems"
// notification bell entry.
//
// What matters here is the flash policy as much as the text: the notice must
// survive a reboot (that is why it is saved at all), but a fault that flaps
// all day must not turn into a flash write per flap. The first fault after an
// acknowledge is written immediately; the rest are batched to at most one
// write per FAULT_NOTICE_FLUSH_INTERVAL_S. The count stops at
// FAULT_NOTICE_COUNT_MAX so a bell nobody looks at for months stays bounded.
//
// fault_notice_init() is a restore from NVS, so a "reboot" is modelled by
// forking a child that calls init against whatever the parent left in the
// fake NVS (the fake lives in process memory, so the child inherits it).

#include "nvs_fake.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "fault_notice.h"
#include "i18n.h"
#include "macon_faults.h"

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
        long long a_ = (long long)(actual);                                 \
        long long e_ = (long long)(expected);                               \
        if (a_ != e_) {                                                     \
            std::fprintf(stderr, "  FAIL %s:%d: %s -> %lld, expected %lld\n", \
                         __FILE__, __LINE__, #actual, a_, e_);              \
            ++g_failures;                                                   \
        }                                                                   \
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

// Must match fault_notice.cpp. Duplicated on purpose: renaming either one
// silently drops every saved notice on every deployed device.
constexpr const char* NS = "fault_notice";
constexpr const char* KEY = "notice";

// 2026-10-02 08:14:00 UTC and 2026-10-03 17:05:00 UTC.
constexpr time_t T_OCT02_0814 = 1790928840;
constexpr time_t T_OCT03_1705 = 1791047100;
constexpr time_t T_UNSET = 1000;  // RTC not set yet

// Faults are named by semantic id; codes come from the library at runtime so
// the controller tree never bakes in OEM codes (see test_opaque_macon_controller).
constexpr arctic::MaconFaultId LOW_PRESSURE = arctic::MaconFaultId::LowPressureProtection;
constexpr arctic::MaconFaultId DISCHARGE_SENSOR = arctic::MaconFaultId::DischargeSensor;

uint16_t site_of(arctic::MaconFaultId id) {
    arctic::MaconFaultSiteId site = 0;
    if (arctic::macon_fault_sites_for_id(id, &site, 1) == 0 || site == 0) {
        std::fprintf(stderr, "  no site for fault id %u\n", (unsigned)id);
        std::exit(2);
    }
    return site;
}

std::string code_of(arctic::MaconFaultId id) {
    return arctic::macon_fault_bit_for_site(site_of(id))->code;
}

size_t writes() { return (size_t)nvs_fake::call_count(nvs_fake::Op::SetBlob); }

bool saved(std::vector<uint8_t>* out = nullptr) {
    std::vector<uint8_t> tmp;
    return nvs_fake::peek_blob(NS, KEY, out ? out : &tmp);
}

// Fresh device: empty flash, notice module freshly booted.
void fresh() {
    nvs_fake::reset();
    fault_notice_init();
}

// Run `fn` in a child as if the device had just rebooted. Returns its failure
// count (or 1 if it crashed).
template <typename Fn>
int after_reboot(Fn fn) {
    std::fflush(nullptr);
    pid_t pid = fork();
    if (pid == 0) {
        g_failures = 0;
        fault_notice_init();
        fn();
        std::fflush(nullptr);
        _exit(g_failures);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

void test_first_fault_written_immediately() {
    std::puts("first fault: pending, count 1, saved at once");
    fresh();
    const uint32_t rev0 = fault_notice_revision();
    CHECK(!fault_notice_get(nullptr));
    fault_notice_record(site_of(LOW_PRESSURE), T_OCT02_0814, 100, false);

    fault_notice_t n;
    CHECK(fault_notice_get(&n));
    CHECK_EQ_INT(n.count, 1);
    CHECK_EQ_INT(n.first, T_OCT02_0814);
    CHECK_EQ_INT(n.latest, T_OCT02_0814);
    CHECK_EQ_INT(n.latest_site, site_of(LOW_PRESSURE));
    CHECK(fault_notice_revision() != rev0);
    CHECK_EQ_INT(writes(), 1);
    CHECK(saved());
}

void test_later_faults_are_throttled() {
    std::puts("later faults: batched to one write per flush interval");
    fresh();
    fault_notice_record(site_of(LOW_PRESSURE), T_OCT02_0814, 100, false);
    CHECK_EQ_INT(writes(), 1);

    // A flapping fault: 50 more onsets within the interval -> no writes.
    for (uint32_t i = 1; i <= 50; ++i) {
        fault_notice_record(site_of(LOW_PRESSURE), T_OCT02_0814 + i, 100 + i, false);
        fault_notice_tick(100 + i);
    }
    CHECK_EQ_INT(writes(), 1);

    // The tick after the interval flushes the batch once.
    fault_notice_tick(100 + FAULT_NOTICE_FLUSH_INTERVAL_S - 1);
    CHECK_EQ_INT(writes(), 1);
    fault_notice_tick(100 + FAULT_NOTICE_FLUSH_INTERVAL_S);
    CHECK_EQ_INT(writes(), 2);
    // Nothing new: later ticks do not write again.
    fault_notice_tick(100 + 5 * FAULT_NOTICE_FLUSH_INTERVAL_S);
    CHECK_EQ_INT(writes(), 2);

    // A fault long after the last write is written straight away.
    fault_notice_record(site_of(DISCHARGE_SENSOR), T_OCT03_1705, 100 + 6 * FAULT_NOTICE_FLUSH_INTERVAL_S,
                        false);
    CHECK_EQ_INT(writes(), 3);

    fault_notice_t n;
    fault_notice_get(&n);
    CHECK_EQ_INT(n.count, 52);
    CHECK_EQ_INT(n.first, T_OCT02_0814);
    CHECK_EQ_INT(n.latest, T_OCT03_1705);
    CHECK_EQ_INT(n.latest_site, site_of(DISCHARGE_SENSOR));
}

void test_count_saturates() {
    std::puts("count stops at FAULT_NOTICE_COUNT_MAX");
    fresh();
    for (uint32_t i = 0; i < FAULT_NOTICE_COUNT_MAX + 25; ++i) {
        fault_notice_record(site_of(LOW_PRESSURE), T_OCT02_0814, 100, false);
    }
    fault_notice_t n;
    fault_notice_get(&n);
    CHECK_EQ_INT(n.count, FAULT_NOTICE_COUNT_MAX);
    // All within one interval: still only the first write.
    CHECK_EQ_INT(writes(), 1);
}

void test_unset_clock_stored_as_zero() {
    std::puts("faults before the clock is set are stored without a time");
    fresh();
    fault_notice_record(site_of(LOW_PRESSURE), T_UNSET, 5, false);
    fault_notice_t n;
    fault_notice_get(&n);
    CHECK_EQ_INT(n.first, 0);
    CHECK_EQ_INT(n.latest, 0);
}

void test_clear_erases_flash() {
    std::puts("clear: forgets the notice in RAM and in flash");
    fresh();
    fault_notice_record(site_of(LOW_PRESSURE), T_OCT02_0814, 100, false);
    CHECK(saved());
    const uint32_t rev = fault_notice_revision();
    fault_notice_clear();
    CHECK(!fault_notice_get(nullptr));
    CHECK(!saved());
    CHECK(fault_notice_revision() != rev);

    // Clearing an empty notice is harmless (missing key is fine).
    fault_notice_clear();
    CHECK(!saved());

    // The next fault starts a fresh notice and is written at once, even
    // though the previous write was moments ago.
    const size_t before = writes();
    fault_notice_record(site_of(DISCHARGE_SENSOR), T_OCT03_1705, 110, false);
    fault_notice_t n;
    fault_notice_get(&n);
    CHECK_EQ_INT(n.count, 1);
    CHECK_EQ_INT(n.first, T_OCT03_1705);
    CHECK_EQ_INT(writes(), before + 1);
}

void test_failed_write_is_retried() {
    std::puts("a failed write stays dirty and is retried by tick");
    fresh();
    nvs_fake::fail_next(nvs_fake::Op::SetBlob, ESP_FAIL);
    fault_notice_record(site_of(LOW_PRESSURE), T_OCT02_0814, 100, false);
    CHECK(fault_notice_get(nullptr));  // still shown in RAM
    CHECK(!saved());
    fault_notice_tick(100 + FAULT_NOTICE_FLUSH_INTERVAL_S);
    CHECK(saved());
}

void test_survives_reboot() {
    std::puts("reboot: a saved notice is restored with its count and times");
    fresh();
    fault_notice_record(site_of(LOW_PRESSURE), T_OCT02_0814, 100, false);
    fault_notice_record(site_of(DISCHARGE_SENSOR), T_OCT03_1705, 200, false);
    fault_notice_tick(100 + FAULT_NOTICE_FLUSH_INTERVAL_S);

    int child = after_reboot([] {
        fault_notice_t n;
        CHECK(fault_notice_get(&n));
        CHECK_EQ_INT(n.count, 2);
        CHECK_EQ_INT(n.first, T_OCT02_0814);
        CHECK_EQ_INT(n.latest, T_OCT03_1705);
        CHECK_EQ_INT(n.latest_site, site_of(DISCHARGE_SENSOR));
    });
    CHECK_EQ_INT(child, 0);
}

void test_unflushed_faults_lost_on_reboot_but_notice_kept() {
    std::puts("reboot before a flush: the notice survives (count may lag)");
    fresh();
    fault_notice_record(site_of(LOW_PRESSURE), T_OCT02_0814, 100, false);
    fault_notice_record(site_of(LOW_PRESSURE), T_OCT02_0814 + 60, 160, false);  // not yet written

    int child = after_reboot([] {
        fault_notice_t n;
        CHECK(fault_notice_get(&n));
        CHECK_EQ_INT(n.count, 1);
    });
    CHECK_EQ_INT(child, 0);
}

void test_at_boot_fault_not_double_counted() {
    std::puts("a fault still active at boot is not counted again");
    fresh();
    fault_notice_record(site_of(LOW_PRESSURE), T_OCT02_0814, 100, false);

    int child = after_reboot([] {
        const size_t w = writes();
        fault_notice_record(site_of(LOW_PRESSURE), T_OCT03_1705, 3, true);
        fault_notice_t n;
        fault_notice_get(&n);
        CHECK_EQ_INT(n.count, 1);
        CHECK_EQ_INT(n.latest, T_OCT02_0814);
        CHECK_EQ_INT(writes(), w);
    });
    CHECK_EQ_INT(child, 0);

    // But with nothing pending (acknowledged before the reboot), a fault that
    // is active at boot is new to the user and does raise the bell.
    fault_notice_clear();
    child = after_reboot([] {
        fault_notice_record(site_of(LOW_PRESSURE), T_OCT03_1705, 3, true);
        fault_notice_t n;
        CHECK(fault_notice_get(&n));
        CHECK_EQ_INT(n.count, 1);
    });
    CHECK_EQ_INT(child, 0);
}

void test_bad_blob_ignored() {
    std::puts("a saved notice from an unknown layout is ignored");
    nvs_fake::reset();
    std::vector<uint8_t> junk(24, 0xAB);  // version 0xAB
    nvs_fake::seed_blob(NS, KEY, junk);
    fault_notice_init();
    CHECK(!fault_notice_get(nullptr));

    nvs_fake::reset();
    std::vector<uint8_t> short_blob(7, 1);
    nvs_fake::seed_blob(NS, KEY, short_blob);
    fault_notice_init();
    CHECK(!fault_notice_get(nullptr));
}

const char* fmt(const fault_notice_t& n, language_t lang) {
    static char buf[160];
    fault_notice_format(buf, sizeof(buf), &n, lang);
    return buf;
}

void test_format() {
    std::puts("format: one / many, EN/FR/ES, no dates");
    const arctic::MaconFaultBit* bit = arctic::macon_fault_bit_for_site(site_of(LOW_PRESSURE));
    const std::string code = bit->code;
    const std::string name_en = bit->label;
    const std::string name_fr = i18n_get_key_lang(bit->label_msg_id, bit->label, LANG_FRENCH);
    const std::string name_es = i18n_get_key_lang(bit->label_msg_id, bit->label, LANG_SPANISH);
    CHECK(name_fr != name_en);  // label is actually translated
    CHECK(name_es != name_en);

    fault_notice_t one = {};
    one.pending = true;
    one.count = 1;
    one.first = one.latest = T_OCT02_0814;
    one.latest_site = site_of(LOW_PRESSURE);

    CHECK_EQ_STR(fmt(one, LANG_ENGLISH), "Heat pump problem: " + code + " " + name_en);
    CHECK_EQ_STR(fmt(one, LANG_FRENCH), "Probl\xC3\xA8me de pompe \xC3\xA0 chaleur : " + code + " " + name_fr);
    CHECK_EQ_STR(fmt(one, LANG_SPANISH), "Problema de la bomba de calor: " + code + " " + name_es);

    // Text does not depend on whether the clock was set.
    fault_notice_t one_no_time = one;
    one_no_time.first = one_no_time.latest = 0;
    CHECK_EQ_STR(fmt(one_no_time, LANG_ENGLISH), "Heat pump problem: " + code + " " + name_en);

    fault_notice_t many = one;
    many.count = 3;
    many.latest = T_OCT03_1705;
    CHECK_EQ_STR(fmt(many, LANG_ENGLISH), "3 heat pump problems \xE2\x80\x93 latest " + code);
    CHECK_EQ_STR(fmt(many, LANG_SPANISH),
                 "3 problemas de la bomba de calor \xE2\x80\x93 \xC3\xBAltimo: " + code);

    // Nothing pending -> empty; tiny buffers are truncated, not overrun.
    fault_notice_t none = {};
    CHECK_EQ_STR(fmt(none, LANG_ENGLISH), "");
    char tiny[8];
    size_t n = fault_notice_format(tiny, sizeof(tiny), &one, LANG_ENGLISH);
    CHECK_EQ_INT(n, sizeof(tiny) - 1);
    CHECK_EQ_INT(std::strlen(tiny), sizeof(tiny) - 1);
}

}  // namespace

int main() {
    setenv("TZ", "UTC0", 1);
    tzset();

    test_first_fault_written_immediately();
    test_later_faults_are_throttled();
    test_count_saturates();
    test_unset_clock_stored_as_zero();
    test_clear_erases_flash();
    test_failed_write_is_retried();
    test_survives_reboot();
    test_unflushed_faults_lost_on_reboot_but_notice_kept();
    test_at_boot_fault_not_double_counted();
    test_bad_blob_ignored();
    test_format();

    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("all fault_notice tests passed");
    return 0;
}
