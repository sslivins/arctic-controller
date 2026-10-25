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
#include <stddef.h>
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
    EVENT_CONNECTED,            // Heat pump telemetry connected
    EVENT_DISCONNECTED,         // Heat pump telemetry disconnected
    EVENT_BROWNOUT_RESET,       // Last boot was caused by a brownout (supply sag)
    EVENT_APPLICATION_CRASH,    // Previous boot ended in an unhandled panic
    EVENT_WATCHDOG_RESET,       // Previous boot was reset by a watchdog
    EVENT_NETWORK_UNREACHABLE,  // Gateway unreachable while WiFi still associated (network-stack wedge)
    EVENT_NETWORK_RECOVERED,    // Network auto-recovery ran (payload: 0=self/keepalive, 1=WiFi bounce, 2=reboot)
    EVENT_WEB_SERVER_DOWN,      // Web/API servers found down while online (payload: event_web_server_cause_t)
    EVENT_WEB_SERVER_RECOVERED, // Web/API server recovery ran (payload: event_web_server_recovery_t)
    EVENT_WIFI_LOST,            // WiFi down for a minute (payload: wifi_err_reason_t of the disconnect)
    EVENT_WIFI_RESTORED,        // WiFi back after an EVENT_WIFI_LOST (payload: seconds down)
    EVENT_WIFI_UNSTABLE,        // Repeated short WiFi drops (payload: drops in the last hour)
    EVENT_WIFI_ROAMED,          // Moved to a stronger access point (payload: wifi_roam_pack)
    EVENT_FIRMWARE_UPDATED,     // Firmware version changed since the last boot (payload: from << 16 | to, see event_firmware_version_pack)
    EVENT_HTTPS_FALLBACK,       // HTTPS failed to start with the administrator certificate; serving HTTP (payload: event_web_server_cause_t)
    EVENT_TYPE_COUNT
} event_type_t;

typedef enum {
    EVENT_CATEGORY_PROBLEMS = 0,
    EVENT_CATEGORY_EQUIPMENT,
    EVENT_CATEGORY_CHANGES,
    EVENT_CATEGORY_SYSTEM,
    EVENT_CATEGORY_COUNT
} event_category_t;

// Which setpoint an EVENT_SETPOINT_CHANGED payload refers to (bits 16..23).
typedef enum {
    EVENT_SETPOINT_COOLING = 0,
    EVENT_SETPOINT_HEATING = 1,
    EVENT_SETPOINT_HOT_WATER = 2,
} event_setpoint_t;

// How the network came back, as the EVENT_NETWORK_RECOVERED payload.
typedef enum {
    EVENT_NETWORK_RECOVERED_SELF = 0,        // came back on its own (keepalive)
    EVENT_NETWORK_RECOVERED_WIFI_BOUNCE = 1, // a WiFi link bounce restored it
    EVENT_NETWORK_RECOVERED_REBOOT = 2,      // gave up and rebooted
} event_network_recovery_t;

// Why the web/API servers were down (EVENT_WEB_SERVER_DOWN payload), or why
// HTTPS could not start (EVENT_HTTPS_FALLBACK payload).
typedef enum {
    EVENT_WEB_SERVER_CAUSE_UNKNOWN = 0,      // stopped for an unknown reason
    EVENT_WEB_SERVER_CAUSE_NO_MEMORY = 1,    // a server task could not get memory
    EVENT_WEB_SERVER_CAUSE_NO_IDENTITY = 2,  // no TLS certificate for HTTPS
    EVENT_WEB_SERVER_CAUSE_START_FAILED = 3, // any other startup failure
    EVENT_WEB_SERVER_CAUSE_TEST = 4,         // injected by a test
    EVENT_WEB_SERVER_CAUSE_BAD_CERT = 5,     // HTTPS certificate/key will not load
    EVENT_WEB_SERVER_CAUSE_UNRESPONSIVE = 6, // running but not answering requests
} event_web_server_cause_t;

// How the web/API servers came back, as the EVENT_WEB_SERVER_RECOVERED payload.
typedef enum {
    EVENT_WEB_SERVER_RECOVERED_RESTARTED = 0, // restarted in place
    EVENT_WEB_SERVER_RECOVERED_REBOOT = 1,    // gave up and rebooted
} event_web_server_recovery_t;

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
 *        A watchdog reset's payload is (raw_reason << 8) | reason, so the ROM
 *        reset code is kept alongside the ESP-IDF reason.
 * @param raw_reason Value from esp_rom_get_reset_reason(0); 0 if unknown.
 */
void event_log_record_reset_reason(esp_reset_reason_t reason, uint32_t raw_reason);

/**
 * @brief Record EVENT_FIRMWARE_UPDATED if the running firmware version differs
 *        from the one that ran last boot (kept in NVS), then remember the
 *        running version. Call once at boot, after NVS and the event log are up.
 *        A rollback shows as an update to an older version.
 * @param running_version  Version of the running image, e.g. "3.4.5".
 * @param previous_hint    Version to assume ran before when NVS has none yet
 *                         (e.g. the other OTA slot on the first boot after an
 *                         OTA); NULL to record nothing in that case.
 * @return true if an event was recorded.
 */
bool event_log_record_firmware_change(const char* running_version, const char* previous_hint);

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

/**
 * @brief Get the display category for an event type.
 */
event_category_t event_type_category(event_type_t type);

/**
 * @brief Get the stable API name for an event category.
 */
const char* event_category_name(event_category_t category);

// ----------------------------------------------------------------------------
// Payload decoding (shared by the device screen and /api/events)
// ----------------------------------------------------------------------------

/** "Before" value of a mode/setpoint change payload (bits 8..15). */
static inline int event_payload_from(uint32_t payload) { return (int)((payload >> 8) & 0xFF); }

/** "After" value of a mode/setpoint change payload (bits 0..7). */
static inline int event_payload_to(uint32_t payload) { return (int)(payload & 0xFF); }

/** Setpoint kind of an EVENT_SETPOINT_CHANGED payload (bits 16..23). */
static inline int event_payload_setpoint(uint32_t payload) { return (int)((payload >> 16) & 0xFF); }

/**
 * @brief Stable API key ("cooling", "heating", "hot_water") for the setpoint
 *        an EVENT_SETPOINT_CHANGED payload refers to, or NULL if unknown.
 */
const char* event_setpoint_key(uint32_t payload);

/**
 * @brief Stable API key for an EVENT_WATCHDOG_RESET payload
 *        ("interrupt", "task", "startup" or "other"). "startup" is an RTC
 *        watchdog reset during startup, most likely a power interruption.
 */
const char* event_watchdog_key(uint32_t payload);

/**
 * @brief Stable API key for an EVENT_NETWORK_RECOVERED payload
 *        ("self", "wifi_bounce", "reboot"), or NULL if unknown.
 */
const char* event_network_recovery_key(uint32_t payload);

/**
 * @brief Stable API key for an EVENT_WEB_SERVER_DOWN payload
 *        ("unknown", "no_memory", "no_identity", "start_failed", "test",
 *        "bad_certificate", "unresponsive").
 */
const char* event_web_server_cause_key(uint32_t payload);

/**
 * @brief Stable API key for an EVENT_WEB_SERVER_RECOVERED payload
 *        ("restarted", "reboot"), or NULL if unknown.
 */
const char* event_web_server_recovery_key(uint32_t payload);

/**
 * @brief Stable API key for the disconnect reason in an EVENT_WIFI_LOST
 *        payload ("signal_lost", "network_not_found", "auth_failed",
 *        "ap_disconnected", "ap_restarted" or "other").
 */
const char* event_wifi_reason_key(uint32_t payload);

/**
 * @brief Pack a "major.minor.patch" version (optional leading 'v', any suffix
 *        ignored) into 16 bits: major 4 bits, minor 5, patch 7. Returns 0
 *        ("unknown") if it does not parse or a part does not fit.
 */
uint16_t event_firmware_version_pack(const char* version);

/**
 * @brief Write a packed version as "3.4.5", or "?" if it is 0 (unknown).
 * @return false if the version is unknown.
 */
bool event_firmware_version_format(uint16_t packed, char* buf, size_t size);

/** "Before" version of an EVENT_FIRMWARE_UPDATED payload. */
static inline uint16_t event_firmware_from(uint32_t payload) { return (uint16_t)(payload >> 16); }

/** "After" version of an EVENT_FIRMWARE_UPDATED payload. */
static inline uint16_t event_firmware_to(uint32_t payload) { return (uint16_t)(payload & 0xFFFF); }

#ifdef __cplusplus
}
#endif
