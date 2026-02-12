/*
 * Arctic Heat Pump Controller
 * Event Log - Records significant system events in a RAM ring buffer
 * 
 * Events are operational records (power changes, mode switches, component
 * state changes, errors) — not debug logs. Lost on reboot by design.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

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
    EVENT_TYPE_COUNT
} event_type_t;

// ============================================================================
// Event Entry
// ============================================================================

typedef struct {
    uint32_t timestamp;       // Unix timestamp (seconds since epoch), 0 if time not synced
    uint32_t uptime_ms;       // Uptime in milliseconds (always valid)
    event_type_t type;        // Event type
    uint32_t payload;         // Type-specific data (mode value, error code, etc.)
} event_entry_t;

// ============================================================================
// Configuration
// ============================================================================

#define EVENT_LOG_MAX_ENTRIES  128   // Max events in ring buffer (~2.5 KB)

// ============================================================================
// API
// ============================================================================

/**
 * @brief Initialize the event log. Call once at startup.
 *        Automatically logs EVENT_SYSTEM_START.
 */
void event_log_init(void);

/**
 * @brief Record an event.
 * @param type    Event type
 * @param payload Optional data (0 if unused)
 */
void event_log_record(event_type_t type, uint32_t payload);

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
 * @brief Clear all events from the log.
 */
void event_log_clear(void);

/**
 * @brief Get a short English description for an event type.
 */
const char* event_type_name(event_type_t type);

#ifdef __cplusplus
}
#endif
