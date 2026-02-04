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
 * @brief Set timezone
 * @param tz_str POSIX timezone string (e.g., "EST5EDT,M3.2.0,M11.1.0" for US Eastern)
 */
void time_mgr_set_timezone(const char* tz_str);

#ifdef __cplusplus
}
#endif
