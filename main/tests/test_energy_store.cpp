// Energy store: the "today" checkpoint, closed days in month blobs, lifetime
// totals carried across months, and pruning. Over the NVS fake.

#include "energy_store.h"
#include "nvs_fake.h"

#include <cstdio>
#include <vector>

using namespace energy;

static int g_failures = 0;

#define CHECK_EQ_L(actual, expected)                                         \
    do {                                                                     \
        long long a_ = (long long)(actual);                                  \
        long long e_ = (long long)(expected);                                \
        if (a_ != e_) {                                                      \
            std::fprintf(stderr, "%s:%d: %s == %lld, expected %lld\n",       \
                         __FILE__, __LINE__, #actual, a_, e_);               \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static DayRecord day(uint32_t date, uint32_t elec_wh, uint32_t heat_wh = 0) {
    DayRecord r{};
    r.date = date;
    r.elec_wh[BUCKET_HEATING] = elec_wh;
    r.thermal_wh[BUCKET_HEATING] = heat_wh;
    r.covered_elec_wh[BUCKET_HEATING] = elec_wh;
    r.power_s = 86400;
    return r;
}

static bool has_month(uint32_t yyyymm) {
    char key[16];
    std::snprintf(key, sizeof(key), "m%06u", (unsigned)yyyymm);
    std::vector<uint8_t> blob;
    return nvs_fake::peek_blob("energy", key, &blob);
}

static void today_round_trip_and_corruption() {
    nvs_fake::reset();
    DayAccum a{};
    CHECK_EQ_L(store_load_today(&a), 0);

    a.date = 20270115;
    a.elec_ws[BUCKET_HEATING] = 123456789;
    a.outdoor_s = 42;
    CHECK_EQ_L(store_save_today(a), 1);
    DayAccum b{};
    CHECK_EQ_L(store_load_today(&b), 1);
    CHECK_EQ_L(b.date, 20270115);
    CHECK_EQ_L(b.elec_ws[BUCKET_HEATING], 123456789);
    CHECK_EQ_L(b.outdoor_s, 42);

    // A flipped bit fails the CRC.
    std::vector<uint8_t> blob;
    CHECK_EQ_L(nvs_fake::peek_blob("energy", "today", &blob), 1);
    blob[40] ^= 1;
    nvs_fake::seed_blob("energy", "today", blob);
    CHECK_EQ_L(store_load_today(&b), 0);

    // A blob of another size (an older/newer layout) is ignored.
    blob.resize(blob.size() - 8);
    nvs_fake::seed_blob("energy", "today", blob);
    CHECK_EQ_L(store_load_today(&b), 0);

    // An uncommitted write is lost with the power.
    nvs_fake::reset();
    nvs_fake::fail_next(nvs_fake::Op::Commit, ESP_FAIL);
    CHECK_EQ_L(store_save_today(a), 0);
    nvs_fake::reset_volatile();
    CHECK_EQ_L(store_load_today(&b), 0);
}

static void commit_and_lifetime() {
    nvs_fake::reset();
    Lifetime life{};
    CHECK_EQ_L(store_load_lifetime(20270115, &life), 0);

    CHECK_EQ_L(store_commit_day(day(20270114, 1000, 3000), &life), 1);
    const uint32_t id = life.reset_id;
    CHECK_EQ_L(id & 1, 1);
    CHECK_EQ_L(life.since_date, 20270114);
    CHECK_EQ_L(life.through_date, 20270114);
    CHECK_EQ_L(life.days, 1);
    CHECK_EQ_L(life.elec_wh, 1000);

    CHECK_EQ_L(store_commit_day(day(20270115, 500, 1500), &life), 1);
    CHECK_EQ_L(life.elec_wh, 1500);
    CHECK_EQ_L(life.thermal_wh[BUCKET_HEATING], 4500);
    CHECK_EQ_L(life.covered_elec_wh[BUCKET_HEATING], 1500);
    CHECK_EQ_L(life.days, 2);

    // Committing a counted day again (crash before the checkpoint) is a no-op.
    CHECK_EQ_L(store_commit_day(day(20270115, 9999), &life), 1);
    CHECK_EQ_L(life.elec_wh, 1500);
    CHECK_EQ_L(store_commit_day(day(20270110, 9999), &life), 1);
    CHECK_EQ_L(life.elec_wh, 1500);

    Lifetime loaded{};
    CHECK_EQ_L(store_load_lifetime(20270131, &loaded), 1);
    CHECK_EQ_L(loaded.elec_wh, 1500);
    CHECK_EQ_L(loaded.reset_id, id);

    // A new month (with a month skipped while the unit was off) carries the
    // totals and the reset id on.
    CHECK_EQ_L(store_commit_day(day(20270301, 200), &life), 1);
    CHECK_EQ_L(life.reset_id, id);
    CHECK_EQ_L(life.elec_wh, 1700);
    CHECK_EQ_L(life.since_date, 20270114);
    CHECK_EQ_L(life.days, 3);
    // Lifetime as of an earlier month is that month's.
    CHECK_EQ_L(store_load_lifetime(20270220, &loaded), 1);
    CHECK_EQ_L(loaded.elec_wh, 1500);
    CHECK_EQ_L(store_load_lifetime(20270320, &loaded), 1);
    CHECK_EQ_L(loaded.elec_wh, 1700);

    // A failed write leaves the totals as they were.
    nvs_fake::fail_next(nvs_fake::Op::SetBlob, ESP_FAIL);
    CHECK_EQ_L(store_commit_day(day(20270302, 300), &life), 0);
    CHECK_EQ_L(store_load_lifetime(20270320, &loaded), 1);
    CHECK_EQ_L(loaded.elec_wh, 1700);

    // Bad dates are refused.
    CHECK_EQ_L(store_commit_day(day(20270100, 1), &life), 0);
}

static void read_days_across_months() {
    nvs_fake::reset();
    Lifetime life{};
    store_commit_day(day(20261230, 1), &life);
    store_commit_day(day(20261231, 2), &life);
    store_commit_day(day(20270101, 3), &life);
    store_commit_day(day(20270215, 4), &life);

    DayRecord out[10];
    size_t n = store_read_days(20261231, 20270228, out, 10);
    CHECK_EQ_L(n, 3);
    CHECK_EQ_L(out[0].date, 20261231);
    CHECK_EQ_L(out[1].date, 20270101);
    CHECK_EQ_L(out[2].date, 20270215);
    CHECK_EQ_L(out[2].elec_wh[BUCKET_HEATING], 4);

    CHECK_EQ_L(store_read_days(20261201, 20271231, out, 2), 2);
    CHECK_EQ_L(store_read_days(20270102, 20270214, out, 10), 0);
    CHECK_EQ_L(store_read_days(20270215, 20270101, out, 10), 0);
}

static void old_months_are_pruned() {
    nvs_fake::reset();
    Lifetime life{};
    store_commit_day(day(20250105, 10), &life);
    store_commit_day(day(20260105, 20), &life);
    CHECK_EQ_L(has_month(202501), 1);
    // Only the 13 months up to Feb 2027 are kept, so Jan 2026 goes too.
    store_commit_day(day(20270210, 30), &life);
    CHECK_EQ_L(has_month(202501), 0);
    CHECK_EQ_L(has_month(202601), 0);
    CHECK_EQ_L(has_month(202702), 1);
    // Totals survive the months they came from.
    CHECK_EQ_L(life.elec_wh, 60);
    CHECK_EQ_L(life.since_date, 20250105);
}

int main() {
    today_round_trip_and_corruption();
    commit_and_lifetime();
    read_days_across_months();
    old_months_are_pruned();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("energy_store: all checks passed\n");
    return 0;
}
