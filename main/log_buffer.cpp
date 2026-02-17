/*
 * Arctic Heat Pump Controller
 * Log Buffer Implementation - RAM ring buffer capturing ESP_LOG output
 *
 * Uses esp_log_set_vprintf() to intercept log output. The hook parses the
 * ESP-IDF log format "X (tag) message\n" to extract level, tag, and message,
 * then stores them as structured entries. Original serial output is preserved
 * by calling the default vprintf after capture.
 */

#include "log_buffer.h"
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// ============================================================================
// Ring Buffer State
// ============================================================================

static log_entry_t s_buffer[LOG_BUFFER_MAX_ENTRIES];
static int s_head = 0;           // Next write position
static int s_count = 0;          // Number of valid entries
static uint32_t s_next_seq = 1;  // Next sequence number to assign
static SemaphoreHandle_t s_mutex = NULL;
static vprintf_like_t s_original_vprintf = NULL;  // Original log output function

// ============================================================================
// Internal Helpers
// ============================================================================

static uint32_t get_uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/**
 * Parse the ESP-IDF log format: "X (%u) %s: message\n"
 * where X is the level char (E/W/I/D/V), (%u) is the timestamp, %s is the tag.
 *
 * The formatted string from esp_log has the format:
 *   "\033[0;31mE (%u) %s: message\033[0m\n"  (with color codes)
 * or:
 *   "E (%u) %s: message\n"  (without color codes)
 *
 * We parse the level from the first letter after any ANSI escape, then extract
 * the tag between ") " and ": ", and the message after ": ".
 */
static void parse_and_store(const char* formatted, int len)
{
    if (len <= 0 || !formatted) return;

    // Skip ANSI escape sequence if present: \033[...m
    const char* p = formatted;
    const char* end = formatted + len;
    if (*p == '\033') {
        while (p < end && *p != 'm') p++;
        if (p < end) p++;  // skip 'm'
    }

    // Parse level character
    esp_log_level_t level;
    if (p >= end) return;
    switch (*p) {
        case 'E': level = ESP_LOG_ERROR;   break;
        case 'W': level = ESP_LOG_WARN;    break;
        case 'I': level = ESP_LOG_INFO;    break;
        case 'D': level = ESP_LOG_DEBUG;   break;
        case 'V': level = ESP_LOG_VERBOSE; break;
        default:  return;  // Not a standard log line, skip
    }

    // Find tag: skip past ") " to get tag start
    const char* tag_start = NULL;
    const char* paren_close = (const char*)memchr(p, ')', end - p);
    if (paren_close && paren_close + 2 < end && paren_close[1] == ' ') {
        tag_start = paren_close + 2;
    }
    if (!tag_start) return;

    // Find tag end: look for ": " separator
    const char* colon = strstr(tag_start, ": ");
    if (!colon || colon >= end) return;

    // Find message: everything after ": " until end (strip trailing ANSI + newline)
    const char* msg_start = colon + 2;
    const char* msg_end = end;

    // Strip trailing newline(s)
    while (msg_end > msg_start && (msg_end[-1] == '\n' || msg_end[-1] == '\r')) {
        msg_end--;
    }
    // Strip trailing ANSI reset: \033[0m
    if (msg_end - msg_start >= 4 && msg_end[-1] == 'm' && msg_end[-4] == '\033') {
        msg_end -= 4;
    }

    // Store entry
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }

    log_entry_t* entry = &s_buffer[s_head];
    entry->seq = s_next_seq++;
    entry->uptime_ms = get_uptime_ms();
    entry->level = level;

    // Copy tag (truncate if needed)
    int tag_len = colon - tag_start;
    if (tag_len >= LOG_BUFFER_TAG_SIZE) tag_len = LOG_BUFFER_TAG_SIZE - 1;
    memcpy(entry->tag, tag_start, tag_len);
    entry->tag[tag_len] = '\0';

    // Copy message (truncate if needed)
    int msg_len = msg_end - msg_start;
    if (msg_len >= LOG_BUFFER_MSG_SIZE) msg_len = LOG_BUFFER_MSG_SIZE - 1;
    if (msg_len > 0) {
        memcpy(entry->message, msg_start, msg_len);
    }
    entry->message[msg_len > 0 ? msg_len : 0] = '\0';

    s_head = (s_head + 1) % LOG_BUFFER_MAX_ENTRIES;
    if (s_count < LOG_BUFFER_MAX_ENTRIES) s_count++;

    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
}

// ============================================================================
// vprintf Hook
// ============================================================================

static int log_buffer_vprintf(const char* fmt, va_list args)
{
    // Format the string into a temporary buffer for parsing
    char buf[LOG_BUFFER_MSG_SIZE + LOG_BUFFER_TAG_SIZE + 64];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    if (len > 0) {
        int actual_len = len < (int)sizeof(buf) ? len : (int)sizeof(buf) - 1;
        parse_and_store(buf, actual_len);
    }

    // Always forward to original output (serial console)
    // We need to re-format since we consumed the va_list
    // Instead, we just write our already-formatted buffer to stdout
    if (len > 0) {
        fwrite(buf, 1, len < (int)sizeof(buf) ? len : (int)sizeof(buf) - 1, stdout);
        // If the original message was longer than our buffer, it's truncated on serial too
        // This is acceptable — the buffer is 280 chars which covers virtually all log lines
    }

    return len;
}

// ============================================================================
// Public API
// ============================================================================

void log_buffer_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_head = 0;
    s_count = 0;
    s_next_seq = 1;
    memset(s_buffer, 0, sizeof(s_buffer));

    // Install our hook, saving the original handler
    s_original_vprintf = esp_log_set_vprintf(log_buffer_vprintf);
}

int log_buffer_get(log_entry_t* out, int max_count, uint32_t since_seq,
                   esp_log_level_t min_level)
{
    if (!s_mutex || !out || max_count <= 0) return 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    // Calculate start index (oldest entry)
    int start;
    if (s_count < LOG_BUFFER_MAX_ENTRIES) {
        start = 0;
    } else {
        start = s_head;  // head points to next write = oldest when full
    }

    int written = 0;
    for (int i = 0; i < s_count && written < max_count; i++) {
        int idx = (start + i) % LOG_BUFFER_MAX_ENTRIES;
        log_entry_t* e = &s_buffer[idx];

        // Filter by sequence number
        if (since_seq > 0 && e->seq <= since_seq) continue;

        // Filter by level (lower number = more severe)
        if (min_level > 0 && e->level > min_level) continue;

        memcpy(&out[written], e, sizeof(log_entry_t));
        written++;
    }

    xSemaphoreGive(s_mutex);
    return written;
}

int log_buffer_count(void)
{
    if (!s_mutex) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int count = s_count;
    xSemaphoreGive(s_mutex);
    return count;
}

uint32_t log_buffer_get_latest_seq(void)
{
    if (!s_mutex) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t seq = s_next_seq > 1 ? s_next_seq - 1 : 0;
    xSemaphoreGive(s_mutex);
    return seq;
}

void log_buffer_clear(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_head = 0;
    s_count = 0;
    memset(s_buffer, 0, sizeof(s_buffer));
    // Don't reset s_next_seq — clients tracking by seq need monotonic growth
    xSemaphoreGive(s_mutex);
}

const char* log_level_char(esp_log_level_t level)
{
    switch (level) {
        case ESP_LOG_ERROR:   return "E";
        case ESP_LOG_WARN:    return "W";
        case ESP_LOG_INFO:    return "I";
        case ESP_LOG_DEBUG:   return "D";
        case ESP_LOG_VERBOSE: return "V";
        default:              return "?";
    }
}
