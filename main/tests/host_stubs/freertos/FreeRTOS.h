/*
 * Host stub for freertos/FreeRTOS.h.
 *
 * The host tests are single-threaded, so the scheduler types exist only to let
 * the sources compile and to make mutex misuse visible (see semphr.h).
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
