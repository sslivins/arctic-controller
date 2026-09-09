/*
 * Host stub for ESP-IDF's nvs.h, backed by a real in-memory key/value store.
 *
 * This is deliberately NOT a no-op. Persistence is behaviour worth testing:
 * "the setting survived a reboot" and "the setting was silently lost" differ
 * only in what NVS did, so a stub that always reports success would make the
 * tests agree with any implementation.
 *
 * The store therefore honours namespaces, open modes (writing through a
 * NVS_READONLY handle fails, as on device), the commit boundary, and handle
 * validity. Test-only controls live in nvs_fake.h.
 */
#pragma once

#include "esp_err.h"

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t nvs_handle_t;

typedef enum {
    NVS_READONLY,
    NVS_READWRITE,
} nvs_open_mode_t;

esp_err_t nvs_open(const char *name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_commit(nvs_handle_t handle);

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value);
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value);
esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out_value);
esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value);
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out_value, size_t *length);
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);
esp_err_t nvs_erase_all(nvs_handle_t handle);

#ifdef __cplusplus
}
#endif
