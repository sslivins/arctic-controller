/*
 * Arctic Heat Pump Controller
 * Time Manager - NTP time synchronization
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize time manager and configure NTP
 *        Call once at startup, NTP sync will happen when WiFi connects
 */
void time_mgr_init(void);

/**
 * @brief Start NTP synchronization
 *        Called automatically when WiFi gets IP, but can be called manually
 */
void time_mgr_start_sync(void);

/**
 * @brief Stop NTP synchronization
 */
void time_mgr_stop_sync(void);

/**
 * @brief Check if time has been synchronized
 * @return true if time is valid (synced with NTP)
 */
bool time_mgr_is_synced(void);

/**
 * @brief Get current time as formatted string
 * @param buf Buffer to store formatted time
 * @param buf_len Buffer size
 * @param format strftime format string (e.g., "%Y-%m-%d %H:%M:%S")
 * @return true if time is valid and formatted
 */
bool time_mgr_get_time_str(char* buf, size_t buf_len, const char* format);

/**
 * @brief Get current local time
 * @param tm_info Pointer to tm struct to fill
 * @return true if time is valid
 */
bool time_mgr_get_local_time(struct tm* tm_info);

/**
 * @brief Set timezone (saves to NVS for persistence)
 * @param tz_str POSIX timezone string (e.g., "EST5EDT,M3.2.0,M11.1.0" for US Eastern)
 * 
 * Common timezone strings:
 *   US Eastern:    "EST5EDT,M3.2.0,M11.1.0"
 *   US Central:    "CST6CDT,M3.2.0,M11.1.0"
 *   US Mountain:   "MST7MDT,M3.2.0,M11.1.0"
 *   US Pacific:    "PST8PDT,M3.2.0,M11.1.0"
 *   UK/London:     "GMT0BST,M3.5.0/1,M10.5.0"
 *   Central Europe: "CET-1CEST,M3.5.0,M10.5.0/3"
 */
void time_mgr_set_timezone(const char* tz_str);

/**
 * @brief Get current timezone string
 * @return Current POSIX timezone string
 */
const char* time_mgr_get_timezone(void);

#ifdef __cplusplus
}
#endif
