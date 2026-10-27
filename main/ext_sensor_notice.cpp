/*
 * Arctic Heat Pump Controller
 * External temperature sensor notice - implementation
 */
#include "ext_sensor_notice.h"

#include <esp_log.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char* TAG = "ext_sensor_notice";

static ext_sensor_notice_t s_notice;
static uint8_t           s_lost_mask;      // slots down right now
static uint32_t          s_outage_down_s;  // longest slot outage in the current outage
static uint32_t          s_revision;
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

void ext_sensor_notice_record_lost(int slot) {
    if (slot < 0 || slot > 7) return;
    Lock lock;
    const uint8_t bit = (uint8_t)(1u << slot);
    if (s_lost_mask & bit) return;
    if (s_lost_mask == 0) {
        // A new outage. Supply and return dropping together is one outage.
        if (!s_notice.pending) {
            s_notice.pending = true;
            s_notice.count = 0;
        }
        if (s_notice.count < EXT_SENSOR_NOTICE_COUNT_MAX) s_notice.count++;
        s_outage_down_s = 0;
    }
    s_lost_mask |= bit;
    if (s_notice.pending) s_notice.ongoing = true;
    s_revision++;
    ESP_LOGI(TAG, "sensor slot %d lost", slot);
}

void ext_sensor_notice_record_restored(int slot, uint32_t down_s) {
    if (slot < 0 || slot > 7) return;
    Lock lock;
    const uint8_t bit = (uint8_t)(1u << slot);
    if (!(s_lost_mask & bit)) return;
    s_lost_mask &= (uint8_t)~bit;
    if (down_s > s_outage_down_s) s_outage_down_s = down_s;
    if (s_lost_mask == 0) {
        s_notice.ongoing = false;
        // An outage acknowledged while it was going on stays acknowledged.
        if (s_notice.pending) s_notice.last_down_s = s_outage_down_s;
    }
    s_revision++;
    ESP_LOGI(TAG, "sensor slot %d restored after %lu s", slot, (unsigned long)down_s);
}

void ext_sensor_notice_settings_changed(void) {
    Lock lock;
    if (s_lost_mask == 0) return;
    s_lost_mask = 0;
    if (s_notice.ongoing) {
        // The outage may have been the old settings: drop it rather than
        // report a recovery that never happened.
        memset(&s_notice, 0, sizeof(s_notice));
    }
    s_revision++;
}

void ext_sensor_notice_clear(void) {
    Lock lock;
    memset(&s_notice, 0, sizeof(s_notice));
    s_revision++;
}

bool ext_sensor_notice_get(ext_sensor_notice_t* out) {
    Lock lock;
    if (out) *out = s_notice;
    return s_notice.pending;
}

uint32_t ext_sensor_notice_revision(void) {
    Lock lock;
    return s_revision;
}

static void format_duration(char* buf, size_t len, uint32_t s) {
    uint32_t min = (s + 30) / 60;
    if (min == 0) min = 1;
    if (min >= 60) {
        snprintf(buf, len, "%lu h %02lu min", (unsigned long)(min / 60), (unsigned long)(min % 60));
    } else {
        snprintf(buf, len, "%lu min", (unsigned long)min);
    }
}

size_t ext_sensor_notice_format(char* buf, size_t buf_size, const ext_sensor_notice_t* notice,
                                language_t lang) {
    if (!buf || buf_size == 0) return 0;
    buf[0] = '\0';
    if (!notice || !notice->pending || notice->count == 0) return 0;

    int n;
    if (notice->ongoing) {
        n = snprintf(buf, buf_size, "%s", i18n_get_lang(STR_NOTIFY_EXT_SENSOR_DOWN, lang));
    } else {
        char dur[24];
        format_duration(dur, sizeof(dur), notice->last_down_s);
        if (notice->count == 1) {
            n = snprintf(buf, buf_size, i18n_get_lang(STR_NOTIFY_EXT_SENSOR_WAS_DOWN_FMT, lang), dur);
        } else {
            n = snprintf(buf, buf_size, i18n_get_lang(STR_NOTIFY_EXT_SENSOR_MANY_FMT, lang),
                         (unsigned)notice->count, dur);
        }
    }
    if (n < 0) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)n < buf_size ? (size_t)n : buf_size - 1;
}
