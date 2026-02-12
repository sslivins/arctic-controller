/*
 * Arctic Heat Pump Controller
 * Event Log Implementation - RAM ring buffer for system events
 */

#include "event_log.h"
#include "time_manager.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
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
    s_head = 0;
    s_count = 0;
    memset(s_buffer, 0, sizeof(s_buffer));
    
    // First event
    event_log_record(EVENT_SYSTEM_START, 0);
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
    ESP_LOGI(TAG, "Event log cleared");
}

const char* event_type_name(event_type_t type) {
    if (type >= EVENT_TYPE_COUNT) return "unknown";
    return s_event_names[type];
}
