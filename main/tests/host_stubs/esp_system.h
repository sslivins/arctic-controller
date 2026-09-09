/*
 * Host stub for ESP-IDF's esp_system.h. Only what the host-built sources use.
 *
 * The reset-reason list is kept in ESP-IDF's exact declaration order, not
 * trimmed to the values this project happens to reference. event_log.cpp
 * stores the raw reason as an event payload, so the numeric values are part of
 * the persisted format and the API response -- a stub that renumbered them
 * would let a test assert the wrong number and still pass.
 */
#pragma once

#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_RST_UNKNOWN,
    ESP_RST_POWERON,
    ESP_RST_EXT,
    ESP_RST_SW,
    ESP_RST_PANIC,
    ESP_RST_INT_WDT,
    ESP_RST_TASK_WDT,
    ESP_RST_WDT,
    ESP_RST_DEEPSLEEP,
    ESP_RST_BROWNOUT,
    ESP_RST_SDIO,
    ESP_RST_USB,
    ESP_RST_JTAG,
    ESP_RST_EFUSE,
    ESP_RST_PWR_GLITCH,
    ESP_RST_CPU_LOCKUP,
} esp_reset_reason_t;

esp_reset_reason_t esp_reset_reason(void);
uint32_t esp_get_free_heap_size(void);
void esp_restart(void);

#ifdef __cplusplus
}
#endif
