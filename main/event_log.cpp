/*
 * Arctic Heat Pump Controller
 * Event Log Implementation - RAM ring backed by a raw-flash journal
 */

#include "event_log.h"
#include "history_storage.h"
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

static const char* TAG = "event_log";

static event_entry_t s_buffer[EVENT_LOG_MAX_ENTRIES];
static uint32_t s_event_ids[EVENT_LOG_MAX_ENTRIES];
static int s_head = 0;
static int s_count = 0;
static uint32_t s_next_event_id = 1;
static uint32_t s_boot_id = 0;
static uint32_t s_revision = 0;
static SemaphoreHandle_t s_mutex = nullptr;
static bool s_storage_ready = false;

static uint64_t get_uptime_ms(void) {
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

static uint32_t get_unix_timestamp(void) {
    time_t now;
    time(&now);
    return now < 1704067200 ? 0 : (uint32_t)now;
}

static void compact_locked(void) {
    if (!s_storage_ready) return;
    esp_err_t err = history_storage_replace_events(
        s_buffer, s_event_ids, EVENT_LOG_MAX_ENTRIES, s_head, s_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to compact event journal: %s", esp_err_to_name(err));
    }
}

static const char* s_event_names[] = {
    "system_start",
    "power_on",
    "power_off",
    "mode_changed",
    "setpoint_changed",
    "compressor_on",
    "compressor_off",
    "fan_on",
    "fan_off",
    "pump_on",
    "pump_off",
    "aux_heater_on",
    "aux_heater_off",
    "defrost_start",
    "defrost_end",
    "error_appeared",
    "error_cleared",
    "connected",
    "disconnected",
    "brownout_reset",
    "application_crash",
    "watchdog_reset",
};

_Static_assert(sizeof(s_event_names) / sizeof(s_event_names[0]) == EVENT_TYPE_COUNT,
               "s_event_names must match EVENT_TYPE_COUNT");

static const char* s_category_names[] = {
    "problems",
    "equipment",
    "changes",
    "system",
};

_Static_assert(sizeof(s_category_names) / sizeof(s_category_names[0]) == EVENT_CATEGORY_COUNT,
               "s_category_names must match EVENT_CATEGORY_COUNT");

void event_log_init(void) {
    if (s_mutex == nullptr) {
        s_mutex = xSemaphoreCreateMutex();
    }
    if (s_mutex == nullptr) {
        ESP_LOGE(TAG, "Failed to create event-log mutex");
        return;
    }

    s_head = 0;
    s_count = 0;
    s_next_event_id = 1;
    s_revision = 0;
    do {
        s_boot_id = esp_random();
    } while (s_boot_id == 0);
    memset(s_buffer, 0, sizeof(s_buffer));
    memset(s_event_ids, 0, sizeof(s_event_ids));

    esp_err_t err = history_storage_init();
    if (err == ESP_OK) {
        size_t restored = 0;
        err = history_storage_load_events(
            s_buffer, s_event_ids, EVENT_LOG_MAX_ENTRIES,
            &restored, &s_next_event_id);
        if (err == ESP_OK) {
            s_count = (int)restored;
            s_head = s_count % EVENT_LOG_MAX_ENTRIES;
            s_storage_ready = true;
        }
    }
    if (err != ESP_OK) {
        s_storage_ready = false;
        ESP_LOGE(TAG, "Event persistence unavailable: %s", esp_err_to_name(err));
    }

    int restored_count = s_count;
    event_log_record(EVENT_SYSTEM_START, 0);
    ESP_LOGI(TAG, "Event log initialized (%d entry capacity, %d restored)",
             EVENT_LOG_MAX_ENTRIES, restored_count);
}

void event_log_record(event_type_t type, uint32_t payload) {
    if (s_mutex == nullptr) {
        ESP_LOGE(TAG, "Cannot record event before initialization");
        return;
    }
    if (type >= EVENT_TYPE_COUNT) {
        ESP_LOGW(TAG, "Ignoring invalid event type %d", (int)type);
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    const int index = s_head;
    event_entry_t* entry = &s_buffer[index];
    entry->timestamp = get_unix_timestamp();
    entry->boot_id = s_boot_id;
    entry->uptime_ms = get_uptime_ms();
    entry->type = type;
    entry->payload = payload;

    uint32_t event_id = s_next_event_id++;
    if (s_next_event_id == 0) s_next_event_id = 1;
    s_event_ids[index] = event_id;

    s_head = (s_head + 1) % EVENT_LOG_MAX_ENTRIES;
    if (s_count < EVENT_LOG_MAX_ENTRIES) s_count++;
    s_revision++;

    if (s_storage_ready) {
        esp_err_t err = history_storage_append_event(event_id, entry);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Journal append failed (%s); compacting",
                     esp_err_to_name(err));
            compact_locked();
        }
    }

    xSemaphoreGive(s_mutex);

    if (payload != 0) {
        ESP_LOGI(TAG, "Event: %s (payload: 0x%08lx)",
                 event_type_name(type), (unsigned long)payload);
    } else {
        ESP_LOGI(TAG, "Event: %s", event_type_name(type));
    }
}

void event_log_record_reset_reason(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_BROWNOUT:
            event_log_record(EVENT_BROWNOUT_RESET, 0);
            break;
        case ESP_RST_PANIC:
            event_log_record(EVENT_APPLICATION_CRASH, 0);
            break;
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
            event_log_record(EVENT_WATCHDOG_RESET, (uint32_t)reason);
            break;
        default:
            break;
    }
}

int event_log_count(void) {
    if (s_mutex == nullptr) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int count = s_count;
    xSemaphoreGive(s_mutex);
    return count;
}

int event_log_get(event_entry_t* out, int max_out, int offset) {
    if (s_mutex == nullptr || out == nullptr || max_out <= 0 || offset < 0) return 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int available = s_count - offset;
    if (available <= 0) {
        xSemaphoreGive(s_mutex);
        return 0;
    }

    int to_copy = available < max_out ? available : max_out;
    for (int i = 0; i < to_copy; i++) {
        int idx = (s_head - 1 - offset - i + EVENT_LOG_MAX_ENTRIES * 2) %
                  EVENT_LOG_MAX_ENTRIES;
        out[i] = s_buffer[idx];
    }
    xSemaphoreGive(s_mutex);
    return to_copy;
}

uint32_t event_log_revision(void) {
    if (s_mutex == nullptr) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t revision = s_revision;
    xSemaphoreGive(s_mutex);
    return revision;
}

uint32_t event_log_current_boot_id(void) {
    return s_boot_id;
}

void event_log_time_synced(const struct timeval* synced_time) {
    if (s_mutex == nullptr || synced_time == nullptr) return;

    uint64_t uptime_ms = get_uptime_ms();
    int64_t sync_ms = ((int64_t)synced_time->tv_sec * 1000) +
                      (synced_time->tv_usec / 1000);
    int64_t boot_epoch_ms = sync_ms - (int64_t)uptime_ms;
    int updated = 0;
    bool compact = false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        int idx = (s_head - 1 - i + EVENT_LOG_MAX_ENTRIES * 2) %
                  EVENT_LOG_MAX_ENTRIES;
        event_entry_t* entry = &s_buffer[idx];
        if (entry->boot_id != s_boot_id || entry->timestamp != 0) continue;

        int64_t event_epoch_ms = boot_epoch_ms + (int64_t)entry->uptime_ms;
        if (event_epoch_ms < 1704067200000LL) continue;

        entry->timestamp = (uint32_t)(event_epoch_ms / 1000);
        updated++;
        if (s_storage_ready && !compact) {
            esp_err_t err = history_storage_append_event(s_event_ids[idx], entry);
            if (err != ESP_OK) compact = true;
        }
    }
    if (compact) compact_locked();
    if (updated > 0) s_revision++;
    xSemaphoreGive(s_mutex);

    if (updated > 0) {
        ESP_LOGI(TAG, "Backfilled and persisted timestamps for %d event(s)", updated);
    }
}

void event_log_clear(void) {
    if (s_mutex == nullptr) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_head = 0;
    s_count = 0;
    memset(s_buffer, 0, sizeof(s_buffer));
    memset(s_event_ids, 0, sizeof(s_event_ids));
    s_revision++;
    compact_locked();
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Event log cleared");
}

void event_log_prepare_factory_reset(void) {
    if (s_mutex == nullptr) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_storage_ready = false;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Event persistence disabled for factory reset");
}

const char* event_type_name(event_type_t type) {
    if (type >= EVENT_TYPE_COUNT) return "unknown";
    return s_event_names[type];
}

event_category_t event_type_category(event_type_t type) {
    switch (type) {
        case EVENT_ERROR_APPEARED:
        case EVENT_ERROR_CLEARED:
        case EVENT_DISCONNECTED:
        case EVENT_BROWNOUT_RESET:
        case EVENT_APPLICATION_CRASH:
        case EVENT_WATCHDOG_RESET:
            return EVENT_CATEGORY_PROBLEMS;

        case EVENT_COMPRESSOR_ON:
        case EVENT_COMPRESSOR_OFF:
        case EVENT_FAN_ON:
        case EVENT_FAN_OFF:
        case EVENT_PUMP_ON:
        case EVENT_PUMP_OFF:
        case EVENT_AUX_HEATER_ON:
        case EVENT_AUX_HEATER_OFF:
        case EVENT_DEFROST_START:
        case EVENT_DEFROST_END:
            return EVENT_CATEGORY_EQUIPMENT;

        case EVENT_POWER_ON:
        case EVENT_POWER_OFF:
        case EVENT_MODE_CHANGED:
        case EVENT_SETPOINT_CHANGED:
            return EVENT_CATEGORY_CHANGES;

        case EVENT_SYSTEM_START:
        case EVENT_CONNECTED:
        default:
            return EVENT_CATEGORY_SYSTEM;
    }
}

const char* event_category_name(event_category_t category) {
    if (category >= EVENT_CATEGORY_COUNT) return "system";
    return s_category_names[category];
}
