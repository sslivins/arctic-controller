/*
 * Host stub for freertos/semphr.h.
 *
 * Single-threaded, so a mutex cannot actually contend -- but it still counts
 * depth, so double-take and give-without-take are detectable rather than
 * silently fine. Those are exactly the mistakes that deadlock or corrupt state
 * on the device and are invisible in a stub that does nothing.
 */
#pragma once

#include "FreeRTOS.h"

#include <stdlib.h>

typedef struct sem_impl {
    int depth;
    int takes;
    int gives;
    int max_depth;
} *SemaphoreHandle_t;

#ifdef __cplusplus
extern "C" {
#endif

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    SemaphoreHandle_t s = (SemaphoreHandle_t)calloc(1, sizeof(struct sem_impl));
    return s;
}

static inline void vSemaphoreDelete(SemaphoreHandle_t s) { free(s); }

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks) {
    (void)ticks;
    if (!s) {
        return pdFALSE;
    }
    // A recursive take on a plain mutex blocks forever on the device. Report
    // failure here so a test can assert on it instead of hanging.
    if (s->depth > 0) {
        return pdFALSE;
    }
    s->depth++;
    s->takes++;
    if (s->depth > s->max_depth) {
        s->max_depth = s->depth;
    }
    return pdTRUE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
    if (!s || s->depth == 0) {
        return pdFALSE;
    }
    s->depth--;
    s->gives++;
    return pdTRUE;
}

#ifdef __cplusplus
}
#endif
