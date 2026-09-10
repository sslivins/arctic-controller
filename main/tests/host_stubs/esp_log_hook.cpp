/*
 * Host stub for esp_log_set_vprintf() and esp_timer_get_time().
 *
 * log_buffer.cpp installs a vprintf hook and stamps every entry with the
 * uptime. Both are real behaviour the tests need to steer: the hook so a test
 * can feed exact log lines through the parser, and the clock so uptime
 * assertions are deterministic rather than whatever the host happened to
 * report.
 */
#include "esp_log.h"
#include "esp_timer.h"

namespace {
vprintf_like_t g_sink = nullptr;
int64_t g_now_us = 0;
}  // namespace

extern "C" vprintf_like_t esp_log_set_vprintf(vprintf_like_t func) {
    vprintf_like_t previous = g_sink;
    g_sink = func;
    return previous;
}

extern "C" vprintf_like_t esp_log_get_vprintf(void) { return g_sink; }

extern "C" int64_t esp_timer_get_time(void) { return g_now_us; }

namespace timer_fake {
void set_us(int64_t us) { g_now_us = us; }
void advance_ms(int64_t ms) { g_now_us += ms * 1000; }
void reset() { g_now_us = 0; }
}  // namespace timer_fake
