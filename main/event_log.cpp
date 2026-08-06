/*
 * Arctic Heat Pump Controller
 * Event Log Implementation - RAM ring buffer for system events
 */

#include "event_log.h"
#include "time_manager.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdlib.h>
#include <string.h>

static const char* TAG = "event_log";

// ============================================================================
// Ring Buffer State
// ============================================================================

static event_entry_t s_buffer[EVENT_LOG_MAX_ENTRIES];
static int s_head = 0;       // Next write position
static int s_count = 0;      // Number of valid entries
static SemaphoreHandle_t s_mutex = nullptr;

// ============================================================================
// NVS Persistence
// ============================================================================
//
// The ring buffer is mirrored to NVS as a single versioned blob so that
// operational history (brownouts, errors, etc.) survives a reboot. Writes are
// throttled to protect flash: most events schedule a debounced flush, while
// critical events (brownout / error transitions) and clears flush immediately.

#define EVENT_LOG_NVS_NS       "event_log"
#define EVENT_LOG_NVS_KEY      "buffer"
#define EVENT_LOG_BLOB_MAGIC   0x41454C31u   // 'A''E''L''1'
#define EVENT_LOG_BLOB_VERSION 1u
#define EVENT_LOG_FLUSH_DELAY_US (5ULL * 1000000ULL)  // 5s debounce for normal events

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t capacity;   // guard against EVENT_LOG_MAX_ENTRIES changes
    uint16_t head;
    uint16_t count;
    event_entry_t entries[EVENT_LOG_MAX_ENTRIES];
} event_log_blob_t;

static event_log_blob_t* s_persist_blob = nullptr;    // heap scratch for NVS writes
static SemaphoreHandle_t s_persist_mutex = nullptr;   // serializes persists
static esp_timer_handle_t s_flush_timer = nullptr;    // debounce timer
static volatile bool s_flush_pending = false;

// ============================================================================
// Internal helpers
// ============================================================================

static uint32_t get_uptime_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint32_t get_unix_timestamp(void) {
    // Use time_manager if available
    time_t now;
    time(&now);
    // If time is before 2024, it's probably not synced
    if (now < 1704067200) return 0;
    return (uint32_t)now;
}

// Snapshot the ring buffer under the data mutex, then write it to NVS outside
// of it (flash writes can take tens of ms — don't block recorders that long).
static void event_log_persist(void) {
    if (s_persist_blob == nullptr || s_persist_mutex == nullptr || s_mutex == nullptr) {
        return;
    }

    xSemaphoreTake(s_persist_mutex, portMAX_DELAY);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_persist_blob->magic = EVENT_LOG_BLOB_MAGIC;
    s_persist_blob->version = EVENT_LOG_BLOB_VERSION;
    s_persist_blob->capacity = EVENT_LOG_MAX_ENTRIES;
    s_persist_blob->head = (uint16_t)s_head;
    s_persist_blob->count = (uint16_t)s_count;
    memcpy(s_persist_blob->entries, s_buffer, sizeof(s_buffer));
    xSemaphoreGive(s_mutex);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(EVENT_LOG_NVS_NS, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, EVENT_LOG_NVS_KEY, s_persist_blob, sizeof(*s_persist_blob));
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist event log to NVS: %s", esp_err_to_name(err));
    }

    xSemaphoreGive(s_persist_mutex);
}

static void flush_timer_cb(void* arg) {
    (void)arg;
    s_flush_pending = false;
    event_log_persist();
}

// Schedule a persist. immediate=true writes now (used for critical events and
// clears); otherwise a debounce timer coalesces bursts into a single write.
static void event_log_schedule_flush(bool immediate) {
    if (s_flush_timer == nullptr) {
        event_log_persist();
        return;
    }
    if (immediate) {
        esp_timer_stop(s_flush_timer);  // cancel any pending debounce
        s_flush_pending = false;
        event_log_persist();
        return;
    }
    if (!s_flush_pending) {
        s_flush_pending = true;
        esp_timer_start_once(s_flush_timer, EVENT_LOG_FLUSH_DELAY_US);
    }
}

// Restore the ring buffer from NVS. Silently starts empty on any mismatch.
static void event_log_load(void) {
    if (s_persist_blob == nullptr) return;

    nvs_handle_t nvs;
    if (nvs_open(EVENT_LOG_NVS_NS, NVS_READONLY, &nvs) != ESP_OK) {
        return;  // namespace doesn't exist yet (first boot)
    }

    size_t required = 0;
    esp_err_t err = nvs_get_blob(nvs, EVENT_LOG_NVS_KEY, nullptr, &required);
    if (err == ESP_OK && required == sizeof(event_log_blob_t)) {
        if (nvs_get_blob(nvs, EVENT_LOG_NVS_KEY, s_persist_blob, &required) == ESP_OK &&
            s_persist_blob->magic == EVENT_LOG_BLOB_MAGIC &&
            s_persist_blob->version == EVENT_LOG_BLOB_VERSION &&
            s_persist_blob->capacity == EVENT_LOG_MAX_ENTRIES &&
            s_persist_blob->head < EVENT_LOG_MAX_ENTRIES &&
            s_persist_blob->count <= EVENT_LOG_MAX_ENTRIES) {
            memcpy(s_buffer, s_persist_blob->entries, sizeof(s_buffer));
            s_head = s_persist_blob->head;
            s_count = s_persist_blob->count;
            ESP_LOGI(TAG, "Restored %d event(s) from NVS", s_count);
        } else {
            ESP_LOGW(TAG, "Persisted event log invalid/incompatible — starting fresh");
        }
    }

    nvs_close(nvs);
}

// ============================================================================
// Event type names (for API/logging)
// ============================================================================

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
};

_Static_assert(sizeof(s_event_names) / sizeof(s_event_names[0]) == EVENT_TYPE_COUNT,
               "s_event_names must match EVENT_TYPE_COUNT");

// ============================================================================
// Public API
// ============================================================================

void event_log_init(void) {
    if (s_mutex == nullptr) {
        s_mutex = xSemaphoreCreateMutex();
    }
    if (s_persist_mutex == nullptr) {
        s_persist_mutex = xSemaphoreCreateMutex();
    }
    if (s_persist_blob == nullptr) {
        s_persist_blob = (event_log_blob_t*)malloc(sizeof(event_log_blob_t));
        if (s_persist_blob == nullptr) {
            ESP_LOGW(TAG, "No memory for persist buffer — event log will be RAM-only");
        }
    }

    s_head = 0;
    s_count = 0;
    memset(s_buffer, 0, sizeof(s_buffer));

    // Restore persisted history (if any) before recording this boot's events.
    event_log_load();

    // Debounced flush timer (coalesces bursts of non-critical events).
    if (s_flush_timer == nullptr) {
        const esp_timer_create_args_t args = {
            .callback = flush_timer_cb,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "evlog_flush",
            .skip_unhandled_events = false,
        };
        esp_timer_create(&args, &s_flush_timer);
    }

    // Record the boot, then durably persist (restored history + this boot).
    event_log_record(EVENT_SYSTEM_START, 0);
    event_log_schedule_flush(true);

    ESP_LOGI(TAG, "Event log initialized (%d entry capacity)", EVENT_LOG_MAX_ENTRIES);
}

void event_log_record(event_type_t type, uint32_t payload) {
    if (s_mutex == nullptr) return;
    if (type >= EVENT_TYPE_COUNT) return;
    
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    
    event_entry_t* entry = &s_buffer[s_head];
    entry->timestamp = get_unix_timestamp();
    entry->uptime_ms = get_uptime_ms();
    entry->type = type;
    entry->payload = payload;
    
    s_head = (s_head + 1) % EVENT_LOG_MAX_ENTRIES;
    if (s_count < EVENT_LOG_MAX_ENTRIES) {
        s_count++;
    }
    
    xSemaphoreGive(s_mutex);
    
    // Also log to serial for debugging
    if (payload != 0) {
        ESP_LOGI(TAG, "Event: %s (payload: 0x%08lx)", event_type_name(type), (unsigned long)payload);
    } else {
        ESP_LOGI(TAG, "Event: %s", event_type_name(type));
    }

    // Persist to NVS. Critical events (brownout / error transitions) can't wait
    // for the debounce window — they're exactly what must survive a hard reboot.
    bool critical = (type == EVENT_BROWNOUT_RESET ||
                     type == EVENT_ERROR_APPEARED ||
                     type == EVENT_ERROR_CLEARED);
    event_log_schedule_flush(critical);
}

int event_log_count(void) {
    if (s_mutex == nullptr) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int count = s_count;
    xSemaphoreGive(s_mutex);
    return count;
}

int event_log_get(event_entry_t* out, int max_out, int offset) {
    if (s_mutex == nullptr || out == nullptr || max_out <= 0) return 0;
    
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    
    int available = s_count - offset;
    if (available <= 0) {
        xSemaphoreGive(s_mutex);
        return 0;
    }
    
    int to_copy = (available < max_out) ? available : max_out;
    
    // Read newest first: head-1 is the newest entry
    for (int i = 0; i < to_copy; i++) {
        int idx = (s_head - 1 - offset - i + EVENT_LOG_MAX_ENTRIES * 2) % EVENT_LOG_MAX_ENTRIES;
        out[i] = s_buffer[idx];
    }
    
    xSemaphoreGive(s_mutex);
    return to_copy;
}

void event_log_clear(void) {
    if (s_mutex == nullptr) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_head = 0;
    s_count = 0;
    memset(s_buffer, 0, sizeof(s_buffer));
    xSemaphoreGive(s_mutex);
    // Persist the empty log immediately so a clear survives a reboot too.
    event_log_schedule_flush(true);
    ESP_LOGI(TAG, "Event log cleared");
}

const char* event_type_name(event_type_t type) {
    if (type >= EVENT_TYPE_COUNT) return "unknown";
    return s_event_names[type];
}
