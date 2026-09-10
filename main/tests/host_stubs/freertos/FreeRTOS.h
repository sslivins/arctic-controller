/*
 * Host stub for freertos/FreeRTOS.h.
 *
 * The scheduler types exist to let the sources compile. Locking primitives are
 * real (see semphr.h and portMUX_TYPE below) so that concurrency tests using
 * std::thread actually exercise mutual exclusion instead of passing by virtue
 * of the stub doing nothing.
 */
#pragma once

#include <stdint.h>

typedef uint32_t TickType_t;
typedef int BaseType_t;

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0

#define portMAX_DELAY ((TickType_t)0xffffffffUL)
#define portTICK_PERIOD_MS 1U
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

#define configTICK_RATE_HZ 1000

#ifdef __cplusplus
#include <atomic>

/* A spinlock, as on the device. Real mutual exclusion so a test that races two
 * threads through portENTER_CRITICAL is testing something. Kept as a spin
 * rather than a std::mutex to match the device's non-blocking semantics: code
 * that takes it twice on one thread hangs in both places, rather than working
 * on the host and deadlocking on hardware. */
struct portMUX_TYPE {
    std::atomic_flag flag = ATOMIC_FLAG_INIT;
};

#define portMUX_INITIALIZER_UNLOCKED {}

static inline void portENTER_CRITICAL(portMUX_TYPE *mux) {
    while (mux->flag.test_and_set(std::memory_order_acquire)) {
    }
}

static inline void portEXIT_CRITICAL(portMUX_TYPE *mux) {
    mux->flag.clear(std::memory_order_release);
}
#endif
