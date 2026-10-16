/*
 * Arctic Heat Pump Controller
 * NVS persistence for the daily energy ledger. See energy_store.h.
 */

#include "energy_store.h"

#include <esp_crc.h>
#include <esp_log.h>
#include <esp_random.h>
#include <nvs.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace energy {

static const char* TAG = "energy_store";

#define NVS_NAMESPACE "energy"
#define NVS_KEY_TODAY "today"

static constexpr uint32_t kTodayMagic = 0x59444F54;  // "TODY"
static constexpr uint32_t kMonthMagic = 0x48544E4D;  // "MNTH"
static constexpr uint16_t kVersion = 1;

struct TodayBlob {
    uint32_t magic;
    uint16_t version;
    uint16_t accum_size;
    uint32_t generation;
    uint32_t reserved;
    DayAccum accum;
    uint32_t crc32;
    uint32_t reserved2;
};

struct MonthBlob {
    uint32_t magic;
    uint16_t version;
    uint16_t record_size;
    uint32_t yyyymm;
    uint32_t reserved;
    Lifetime life;
    DayRecord days[31];
    uint32_t crc32;
};

template <typename T>
static uint32_t blob_crc(const T& b) {
    return esp_crc32_le(0, reinterpret_cast<const uint8_t*>(&b), offsetof(T, crc32));
}

static void month_key(uint32_t yyyymm, char (&key)[16]) {
    snprintf(key, sizeof(key), "m%06u", (unsigned)yyyymm);
}

static bool load_month(nvs_handle_t nvs, uint32_t yyyymm, MonthBlob* m) {
    char key[16];
    month_key(yyyymm, key);
    size_t len = sizeof(*m);
    if (nvs_get_blob(nvs, key, m, &len) != ESP_OK || len != sizeof(*m)) return false;
    return m->magic == kMonthMagic && m->version == kVersion &&
           m->record_size == sizeof(DayRecord) && m->yyyymm == yyyymm &&
           m->crc32 == blob_crc(*m);
}

// Newest month blob at or before `yyyymm`, looking back as far as one could
// still be kept. Returns its month, or 0.
static uint32_t find_month_at_or_before(nvs_handle_t nvs, uint32_t yyyymm, MonthBlob* m) {
    for (int i = 0; i <= kMonthsKept + 1; i++, yyyymm = prev_month(yyyymm)) {
        if (load_month(nvs, yyyymm, m)) return yyyymm;
    }
    return 0;
}

bool store_load_today(DayAccum* out) {
    TodayBlob* bp = static_cast<TodayBlob*>(malloc(sizeof(TodayBlob)));
    if (!bp) return false;
    const TodayBlob& b = *bp;
    nvs_handle_t nvs;
    bool ok = false;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(b);
        ok = nvs_get_blob(nvs, NVS_KEY_TODAY, bp, &len) == ESP_OK && len == sizeof(b) &&
             b.magic == kTodayMagic && b.version == kVersion &&
             b.accum_size == sizeof(DayAccum) && b.crc32 == blob_crc(b);
        nvs_close(nvs);
    }
    if (ok) *out = b.accum;
    free(bp);
    return ok;
}

bool store_save_today(const DayAccum& accum) {
    static uint32_t s_generation = 0;
    // On the heap: callers run on 4 KB task stacks, and NVS writes are deep.
    TodayBlob* bp = static_cast<TodayBlob*>(malloc(sizeof(TodayBlob)));
    if (!bp) return false;
    TodayBlob& b = *bp;
    memset(&b, 0, sizeof(b));
    b.magic = kTodayMagic;
    b.version = kVersion;
    b.accum_size = sizeof(DayAccum);
    b.generation = ++s_generation;
    b.accum = accum;
    b.crc32 = blob_crc(b);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, NVS_KEY_TODAY, &b, sizeof(b));
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    free(bp);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save today's energy: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

void fold(Lifetime* life, const DayRecord& d) {
    if (life->days == 0) life->since_date = d.date;
    life->through_date = d.date;
    life->days++;
    life->elec_wh += total_elec_wh(d);
    for (int i = 0; i < kModes; i++) {
        life->thermal_wh[i] += d.thermal_wh[i];
        life->covered_elec_wh[i] += d.covered_elec_wh[i];
    }
    life->power_s += d.power_s;
    life->thermal_s += d.thermal_s;
}

bool store_commit_day(const DayRecord& day, Lifetime* life) {
    const uint32_t yyyymm = day.date / 100;
    const uint32_t dom = day.date % 100;
    if (dom < 1 || dom > 31) return false;

    MonthBlob* m = static_cast<MonthBlob*>(malloc(sizeof(MonthBlob)));
    if (!m) return false;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        free(m);
        ESP_LOGW(TAG, "Failed to open energy store: %s", esp_err_to_name(err));
        return false;
    }

    if (!load_month(nvs, yyyymm, m)) {
        // A new month carries the totals on from the newest earlier one.
        Lifetime prev;
        bool have_prev = false;
        if (find_month_at_or_before(nvs, prev_month(yyyymm), m)) {
            prev = m->life;
            have_prev = true;
        }
        memset(m, 0, sizeof(*m));
        m->magic = kMonthMagic;
        m->version = kVersion;
        m->record_size = sizeof(DayRecord);
        m->yyyymm = yyyymm;
        if (have_prev) {
            m->life = prev;
        } else {
            m->life.reset_id = esp_random() | 1;
        }
    }

    if (day.date <= m->life.through_date) {
        // Already counted (a crash between the commit and the next
        // checkpoint); the stored record is the newer one.
        if (life) *life = m->life;
        nvs_close(nvs);
        free(m);
        return true;
    }

    m->days[dom - 1] = day;
    fold(&m->life, day);
    m->crc32 = blob_crc(*m);

    char key[16];
    month_key(yyyymm, key);
    err = nvs_set_blob(nvs, key, m, sizeof(*m));
    if (err == ESP_OK) err = nvs_commit(nvs);
    if (err == ESP_OK) {
        if (life) *life = m->life;
        // Drop months that have aged out. Months are skipped while the unit
        // is off, so sweep a range rather than one key.
        uint32_t old = yyyymm;
        for (int i = 0; i < kMonthsKept; i++) old = prev_month(old);
        for (int i = 0; i < 36; i++, old = prev_month(old)) {
            month_key(old, key);
            if (nvs_erase_key(nvs, key) == ESP_OK) nvs_commit(nvs);
        }
    } else {
        ESP_LOGW(TAG, "Failed to store energy for %u: %s", (unsigned)day.date,
                 esp_err_to_name(err));
    }
    nvs_close(nvs);
    free(m);
    return err == ESP_OK;
}

bool store_load_lifetime(uint32_t date, Lifetime* out) {
    MonthBlob* m = static_cast<MonthBlob*>(malloc(sizeof(MonthBlob)));
    if (!m) return false;
    nvs_handle_t nvs;
    bool ok = false;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        if (find_month_at_or_before(nvs, date / 100, m)) {
            *out = m->life;
            ok = true;
        }
        nvs_close(nvs);
    }
    free(m);
    return ok;
}

size_t store_read_days(uint32_t from_date, uint32_t to_date, DayRecord* out, size_t capacity) {
    if (from_date > to_date || capacity == 0) return 0;
    MonthBlob* m = static_cast<MonthBlob*>(malloc(sizeof(MonthBlob)));
    if (!m) return 0;
    size_t n = 0;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        for (uint32_t ym = from_date / 100; ym <= to_date / 100 && n < capacity;
             ym = next_month(ym)) {
            if (!load_month(nvs, ym, m)) continue;
            for (int d = 0; d < 31 && n < capacity; d++) {
                const DayRecord& r = m->days[d];
                if (r.date != 0 && r.date >= from_date && r.date <= to_date) out[n++] = r;
            }
        }
        nvs_close(nvs);
    }
    free(m);
    return n;
}

}  // namespace energy
