/*
 * Host stub for ESP-IDF's esp_timer.h.
 *
 * Only the monotonic microsecond clock is needed. It is steerable (see
 * timer_fake in esp_log_hook.cpp) so uptime assertions are deterministic
 * instead of depending on how fast the host happens to be.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int64_t esp_timer_get_time(void);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
namespace timer_fake {
void set_us(int64_t us);
void advance_ms(int64_t ms);
void reset();
}  // namespace timer_fake
#endif
