/*
 * Arctic Heat Pump Controller
 * Log Buffer - Captures ESP_LOG output into a RAM ring buffer
 *
 * Hooks into esp_log_set_vprintf() to intercept all ESP_LOG* output,
 * storing structured entries for retrieval via the REST API.
 * Serial output continues unaffected.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <esp_log.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Configuration
// ============================================================================

#define LOG_BUFFER_MAX_ENTRIES   256     // Max entries in ring buffer
#define LOG_BUFFER_MSG_SIZE     192     // Max message length per entry (truncated if longer)
#define LOG_BUFFER_TAG_SIZE     24      // Max tag length

// ============================================================================
// Log Entry
// ============================================================================

typedef struct {
    uint32_t seq;                           // Monotonically increasing sequence number
    uint32_t uptime_ms;                     // Uptime in milliseconds when logged
    esp_log_level_t level;                  // Log level (E/W/I/D/V)
    char tag[LOG_BUFFER_TAG_SIZE];          // Component tag
    char message[LOG_BUFFER_MSG_SIZE];      // Log message (without level/tag prefix)
} log_entry_t;

// ============================================================================
// Public API
// ============================================================================

/**
 * @brief Initialize the log buffer and install the vprintf hook
 *
 * Should be called as early as possible in app_main() to capture boot logs.
 * Safe to call before most other subsystems are initialized.
 */
void log_buffer_init(void);

/**
 * @brief Get log entries from the buffer
 *
 * Returns entries in chronological order (oldest first).
 *
 * @param out        Output array to fill
 * @param max_count  Maximum entries to return
 * @param since_seq  Only return entries with seq > since_seq (0 = all)
 * @param min_level  Minimum log level (ESP_LOG_ERROR=1 most restrictive,
 *                   ESP_LOG_VERBOSE=5 least restrictive). Pass 0 or
 *                   ESP_LOG_VERBOSE to get all.
 * @return Number of entries written to out
 */
int log_buffer_get(log_entry_t* out, int max_count, uint32_t since_seq,
                   esp_log_level_t min_level);

/**
 * @brief Get the number of entries currently in the buffer
 */
int log_buffer_count(void);

/**
 * @brief Get the sequence number of the latest entry
 *
 * Useful for clients to track their position for incremental fetching.
 * Returns 0 if no entries have been recorded.
 */
uint32_t log_buffer_get_latest_seq(void);

/**
 * @brief Clear all entries from the buffer
 */
void log_buffer_clear(void);

/**
 * @brief Get single-character representation of a log level
 *
 * @param level  ESP log level
 * @return "E", "W", "I", "D", "V", or "?"
 */
const char* log_level_char(esp_log_level_t level);

#ifdef __cplusplus
}
#endif
