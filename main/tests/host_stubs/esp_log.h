/*
 * Host stub for ESP-IDF's esp_log.h.
 *
 * Logging is a side effect the unit tests do not assert on, so the macros
 * expand to nothing rather than to printf: a chatty source file would
 * otherwise bury the one line that matters (a CHECK failure) in noise.
 *
 * The arguments are still consumed by a discarded sizeof expression so that a
 * malformed ESP_LOGx call -- wrong argument count, undeclared variable -- is
 * still a compile error on the host, which is a large part of the value of
 * compiling these files at all.
 */
#pragma once

#include <stdio.h>

typedef enum {
    ESP_LOG_NONE,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE,
} esp_log_level_t;

#define ARCTIC_HOST_LOG_SINK(tag, fmt, ...) \
    do { (void)sizeof(tag); (void)sizeof(printf(fmt, ##__VA_ARGS__)); } while (0)

#define ESP_LOGE(tag, fmt, ...) ARCTIC_HOST_LOG_SINK(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) ARCTIC_HOST_LOG_SINK(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) ARCTIC_HOST_LOG_SINK(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ARCTIC_HOST_LOG_SINK(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) ARCTIC_HOST_LOG_SINK(tag, fmt, ##__VA_ARGS__)

#define esp_log_level_set(tag, level) ((void)0)

// The log-capture hook. log_buffer.cpp replaces the sink and stores what it
// parses, so the host build needs the real signature (a test installs its own
// sink and drives the hook directly).
#include <stdarg.h>

typedef int (*vprintf_like_t)(const char *, va_list);

#ifdef __cplusplus
extern "C" {
#endif

vprintf_like_t esp_log_set_vprintf(vprintf_like_t func);
vprintf_like_t esp_log_get_vprintf(void);

#ifdef __cplusplus
}
#endif
