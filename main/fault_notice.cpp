/*
 * Arctic Heat Pump Controller
 * Heat pump fault notice - implementation
 */
#include "fault_notice.h"

#include <esp_log.h>
#include <nvs.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "macon_faults.h"

static const char* TAG = "fault_notice";

#define NVS_NAMESPACE "fault_notice"
#define NVS_KEY       "notice"
#define BLOB_VERSION  1

// 2024-01-01T00:00:00Z. Anything earlier means the clock has not been set yet
// (same rule as the event log).
#define MIN_VALID_TIME ((time_t)1704067200)

// On-flash layout. Fixed-width fields only, and versioned, so a later firmware
// can tell an old record from a new one.
typedef struct {
    uint8_t  version;
    uint8_t  reserved;
    uint16_t latest_site;
    uint32_t count;
    int64_t  first;
    int64_t  latest;
} notice_blob_t;

static fault_notice_t   s_notice;
static uint32_t         s_revision;
static bool             s_dirty;
static uint32_t         s_last_write_mono;
static StaticSemaphore_t s_mutex_buf;
static SemaphoreHandle_t s_mutex;

namespace {
struct Lock {
    Lock() {
        if (!s_mutex) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buf);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
    ~Lock() { xSemaphoreGive(s_mutex); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};
}  // namespace

// Caller holds the lock.
static void persist(uint32_t mono_s) {
    s_last_write_mono = mono_s;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed (%s); notice kept in RAM", esp_err_to_name(err));
        return;  // stay dirty so the next tick retries
    }
    if (s_notice.pending) {
        notice_blob_t b = {};
        b.version = BLOB_VERSION;
        b.latest_site = s_notice.latest_site;
        b.count = s_notice.count;
        b.first = s_notice.first;
        b.latest = s_notice.latest;
        err = nvs_set_blob(nvs, NVS_KEY, &b, sizeof(b));
    } else {
        err = nvs_erase_key(nvs, NVS_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "persist failed (%s); will retry", esp_err_to_name(err));
        return;
    }
    s_dirty = false;
}

void fault_notice_init(void) {
    Lock lock;
    memset(&s_notice, 0, sizeof(s_notice));
    s_dirty = false;
    s_last_write_mono = 0;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;  // namespace not created yet: nothing saved
    }
    notice_blob_t b = {};
    size_t len = sizeof(b);
    esp_err_t err = nvs_get_blob(nvs, NVS_KEY, &b, &len);
    nvs_close(nvs);
    if (err != ESP_OK) return;
    if (len != sizeof(b) || b.version != BLOB_VERSION || b.count == 0) {
        ESP_LOGW(TAG, "ignoring unrecognised saved notice (len %u, version %u)",
                 (unsigned)len, (unsigned)b.version);
        return;
    }
    s_notice.pending = true;
    s_notice.count = b.count > FAULT_NOTICE_COUNT_MAX ? FAULT_NOTICE_COUNT_MAX : b.count;
    s_notice.first = b.first;
    s_notice.latest = b.latest;
    s_notice.latest_site = b.latest_site;
    s_revision++;
    ESP_LOGI(TAG, "restored notice: %lu fault(s)", (unsigned long)s_notice.count);
}

void fault_notice_record(uint16_t site, time_t now, uint32_t mono_s, bool at_boot) {
    Lock lock;
    if (at_boot && s_notice.pending) {
        return;  // most likely the same fault, already counted before the reboot
    }
    const int64_t when = (now >= MIN_VALID_TIME) ? (int64_t)now : 0;
    const bool first_one = !s_notice.pending;
    if (first_one) {
        s_notice.pending = true;
        s_notice.count = 0;
        s_notice.first = when;
    }
    if (s_notice.count < FAULT_NOTICE_COUNT_MAX) s_notice.count++;
    s_notice.latest = when;
    s_notice.latest_site = site;
    s_revision++;
    s_dirty = true;

    if (first_one || mono_s - s_last_write_mono >= FAULT_NOTICE_FLUSH_INTERVAL_S) {
        persist(mono_s);
    }
}

void fault_notice_tick(uint32_t mono_s) {
    Lock lock;
    if (s_dirty && mono_s - s_last_write_mono >= FAULT_NOTICE_FLUSH_INTERVAL_S) {
        persist(mono_s);
    }
}

void fault_notice_clear(void) {
    Lock lock;
    const bool had = s_notice.pending;
    memset(&s_notice, 0, sizeof(s_notice));
    s_revision++;
    // Always erase, even if RAM was empty: a failed earlier erase must not let
    // an acknowledged notice come back on the next boot.
    s_dirty = true;
    persist(s_last_write_mono);
    if (had) ESP_LOGI(TAG, "notice acknowledged");
}

bool fault_notice_get(fault_notice_t* out) {
    Lock lock;
    if (out) *out = s_notice;
    return s_notice.pending;
}

uint32_t fault_notice_revision(void) {
    Lock lock;
    return s_revision;
}

size_t fault_notice_format(char* buf, size_t buf_size, const fault_notice_t* notice,
                           language_t lang) {
    if (!buf || buf_size == 0) return 0;
    buf[0] = '\0';
    if (!notice || !notice->pending || notice->count == 0) return 0;

    const char* code = "?";
    const char* name = "";
    const arctic::MaconFaultBit* bit = arctic::macon_fault_bit_for_site(notice->latest_site);
    if (bit) {
        code = bit->code;
        name = i18n_get_key_lang(bit->label_msg_id, bit->label, lang);
    }

    int n;
    if (notice->count == 1) {
        n = snprintf(buf, buf_size, i18n_get_lang(STR_NOTIFY_HP_FAULT_ONE, lang), code, name);
    } else {
        char count_str[12];
        snprintf(count_str, sizeof(count_str), "%lu", (unsigned long)notice->count);
        n = snprintf(buf, buf_size, i18n_get_lang(STR_NOTIFY_HP_FAULT_MANY, lang), count_str, code);
    }
    if (n < 0) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)n < buf_size ? (size_t)n : buf_size - 1;
}
