/*
 * Host stub for ESP-IDF's esp_err.h.
 *
 * Only the error codes the host-testable sources under main/ actually
 * reference. Values match the real IDF definitions so that a test asserting on
 * a specific code stays meaningful when the same source is built for the
 * device.
 */
#pragma once

#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1

#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_INVALID_CRC 0x10c

#define ESP_ERR_NVS_BASE 0x1100
#define ESP_ERR_NVS_NOT_FOUND (ESP_ERR_NVS_BASE + 0x02)
#define ESP_ERR_NVS_INVALID_HANDLE (ESP_ERR_NVS_BASE + 0x03)
#define ESP_ERR_NVS_READ_ONLY (ESP_ERR_NVS_BASE + 0x04)
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE (ESP_ERR_NVS_BASE + 0x05)
#define ESP_ERR_NVS_INVALID_LENGTH (ESP_ERR_NVS_BASE + 0x0a)

#ifdef __cplusplus
extern "C" {
#endif

/* Returns a stable, human-readable name. Sources pass the result to a log
 * call, so it must never be NULL. */
const char *esp_err_to_name(esp_err_t code);

#ifdef __cplusplus
}
#endif
