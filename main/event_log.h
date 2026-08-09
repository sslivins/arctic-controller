/*
 * Arctic Heat Pump Controller
 * Event Log - Records significant system events in a ring buffer
 * 
 * Events are operational records (power changes, mode switches, component
 * state changes, errors) — not debug logs. The buffer is persisted in the
 * dedicated history partition so it survives reboots without consuming NVS.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <esp_system.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Event Types
// ============================================================================

typedef enum {
    EVENT_SYSTEM_START = 0,     // Controller booted
    EVENT_POWER_ON,             // Heat pump powered on
    EVENT_POWER_OFF,            // Heat pump powered off
    EVENT_MODE_CHANGED,         // Working mode changed (payload: old<<8 | new)
    EVENT_SETPOINT_CHANGED,     // Setpoint changed (payload: type<<16 | old<<8 | new)
    EVENT_COMPRESSOR_ON,        // Compressor started
    EVENT_COMPRESSOR_OFF,       // Compressor stopped
    EVENT_FAN_ON,               // Fan started
    EVENT_FAN_OFF,              // Fan stopped
    EVENT_PUMP_ON,              // Water pump started
    EVENT_PUMP_OFF,             // Water pump stopped
    EVENT_AUX_HEATER_ON,       // Auxiliary heater activated
    EVENT_AUX_HEATER_OFF,      // Auxiliary heater deactivated
    EVENT_DEFROST_START,        // Defrost cycle started
    EVENT_DEFROST_END,          // Defrost cycle ended
    EVENT_ERROR_APPEARED,       // Error code appeared (payload: error code)
    EVENT_ERROR_CLEARED,        // Error code cleared (payload: error code)
    EVENT_CONNECTED,            // Heat pump connected (Modbus OK)
    EVENT_DISCONNECTED,         // Heat pump disconnected (Modbus lost)
    EVENT_BROWNOUT_RESET,       // Last boot was caused by a brownout (supply sag)
    EVENT_APPLICATION_CRASH,    // Previous boot ended in an unhandled panic
    EVENT_WATCHDOG_RESET,       // Previous boot was reset by a watchdog
    EVENT_TYPE_COUNT
} event_type_t;

// ============================================================================
// Event Entry
// ============================================================================

typedef struct {
    uint32_t timestamp;       // Unix timestamp (seconds since epoch), 0 if time not synced
    uint32_t boot_id;         // Random ID shared by all events from the same boot
    uint64_t uptime_ms;       // Uptime in milliseconds (always valid)
    event_type_t type;        // Event type
    uint32_t payload;         // Type-specific data (mode value, error code, etc.)
} event_entry_t;

// ============================================================================
// Configuration
// ============================================================================

#define EVENT_LOG_MAX_ENTRIES  1000

// ============================================================================
// API
// ============================================================================

/**
 * @brief Initialize the event log. Call once at startup.
 *        Restores persisted events, then logs EVENT_SYSTEM_START.
 */
void event_log_init(void);

/**
 * @brief Record an event.
 * @param type    Event type
 * @param payload Optional data (0 if unused)
 */
void event_log_record(event_type_t type, uint32_t payload);

/**
 * @brief Record an abnormal previous-boot reset reason, if applicable.
 *        Normal software, power-on, external, and deep-sleep resets are ignored.
 */
void event_log_record_reset_reason(esp_reset_reason_t reason);

/**
 * @brief Get count of events currently in the buffer.
 */
int event_log_count(void);

/**
 * @brief Get events from the log, newest first.
 * @param out     Output buffer
 * @param max_out Maximum entries to copy
 * @param offset  Skip this many newest entries (for pagination)
 * @return Number of entries actually copied
 */
int event_log_get(event_entry_t* out, int max_out, int offset);

/**
 * @brief Get the event-log revision, incremented whenever entries change.
 */
uint32_t event_log_revision(void);

/**
 * @brief Get the random identifier assigned to the current controller boot.
 */
uint32_t event_log_current_boot_id(void);

/**
 * @brief Backfill untimed events from this boot after NTP synchronization.
 * @param synced_time Accurate wall-clock time at the synchronization callback.
 */
void event_log_time_synced(const struct timeval* synced_time);

/**
 * @brief Clear all events from the log.
 */
void event_log_clear(void);

/**
 * @brief Stop persistent writes before the history partition is erased.
 *        Intended only for the factory-reset path immediately before reboot.
 */
void event_log_prepare_factory_reset(void);

/**
 * @brief Get a short English description for an event type.
 */
const char* event_type_name(event_type_t type);

#ifdef __cplusplus
}
#endif
