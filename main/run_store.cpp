/*
 * Arctic Heat Pump Controller
 * NVS persistence for per-run energy summaries. See run_store.h.
 */

#include "run_store.h"

#include <esp_crc.h>
#include <esp_log.h>
#include <nvs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace energy {

static const char* TAG = "run_store";

#define NVS_NAMESPACE "runs"

static constexpr uint32_t kRunsMagic = 0x534E5552;  // "RUNS"
static constexpr uint16_t kVersion = 1;

struct DayHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t record_size;
    uint32_t date;
    uint16_t count;
    uint16_t reserved;
    uint32_t crc32;  // over the header (with this field 0) and the records
};

// Written with only `count` records.
struct DayBlob {
    DayHeader h;
    RunRecord runs[kMaxRunsPerDay];
};

static size_t blob_size(uint16_t count) {
    return offsetof(DayBlob, runs) + (size_t)count * sizeof(RunRecord);
}

static uint32_t blob_crc(DayBlob* b) {
    const uint32_t saved = b->h.crc32;
    b->h.crc32 = 0;
    const uint32_t crc =
        esp_crc32_le(0, reinterpret_cast<const uint8_t*>(b), blob_size(b->h.count));
    b->h.crc32 = saved;
    return crc;
}

static void day_key(uint32_t date, char (&key)[16]) {
    snprintf(key, sizeof(key), "d%08u", (unsigned)date);
}

static bool load_day(nvs_handle_t nvs, uint32_t date, DayBlob* b) {
    char key[16];
    day_key(date, key);
    size_t len = sizeof(*b);
    if (nvs_get_blob(nvs, key, b, &len) != ESP_OK || len < sizeof(DayHeader)) return false;
    return b->h.magic == kRunsMagic && b->h.version == kVersion &&
           b->h.record_size == sizeof(RunRecord) && b->h.date == date &&
           b->h.count <= kMaxRunsPerDay && len == blob_size(b->h.count) &&
           b->h.crc32 == blob_crc(b);
}

bool store_append_run(const RunRecord& run) {
    if (run.date == 0) return false;
    // On the heap: callers run on 4 KB task stacks.
    DayBlob* b = static_cast<DayBlob*>(malloc(sizeof(DayBlob)));
    if (!b) return false;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        free(b);
        ESP_LOGW(TAG, "Failed to open run store: %s", esp_err_to_name(err));
        return false;
    }
    const bool existed = load_day(nvs, run.date, b);
    if (!existed) {
        memset(&b->h, 0, sizeof(b->h));
        b->h.magic = kRunsMagic;
        b->h.version = kVersion;
        b->h.record_size = sizeof(RunRecord);
        b->h.date = run.date;
    }

    RunRecord r = run;
    r.flags &= (uint8_t)~RUN_OPEN;
    size_t at = b->h.count;
    while (at > 0 && b->runs[at - 1].start_s > r.start_s) at--;
    if (at > 0 && b->runs[at - 1].start_s == r.start_s) {
        b->runs[at - 1] = r;
    } else if (b->h.count < kMaxRunsPerDay) {
        memmove(&b->runs[at + 1], &b->runs[at], (b->h.count - at) * sizeof(RunRecord));
        b->runs[at] = r;
        b->h.count++;
    } else {
        nvs_close(nvs);
        free(b);
        return true;
    }
    b->h.crc32 = blob_crc(b);

    char key[16];
    day_key(run.date, key);
    err = nvs_set_blob(nvs, key, b, blob_size(b->h.count));
    if (err == ESP_OK) err = nvs_commit(nvs);
    if (err == ESP_OK && !existed) {
        // A new day: drop the ones that have aged out. Days are skipped while
        // the unit is off, so sweep a range rather than one key.
        for (int i = kRunDaysKept; i < kRunDaysKept + 60; i++) {
            day_key(date_add_days(run.date, -i), key);
            if (nvs_erase_key(nvs, key) == ESP_OK) nvs_commit(nvs);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to store run for %u: %s", (unsigned)run.date,
                 esp_err_to_name(err));
    }
    nvs_close(nvs);
    free(b);
    return err == ESP_OK;
}

size_t store_read_runs(uint32_t date, RunRecord* out, size_t capacity) {
    DayBlob* b = static_cast<DayBlob*>(malloc(sizeof(DayBlob)));
    if (!b) return 0;
    size_t n = 0;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        if (load_day(nvs, date, b)) {
            for (; n < b->h.count && n < capacity; n++) out[n] = b->runs[n];
        }
        nvs_close(nvs);
    }
    free(b);
    return n;
}

void store_clear_runs() {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        if (nvs_erase_all(nvs) == ESP_OK) nvs_commit(nvs);
        nvs_close(nvs);
    }
}

}  // namespace energy
