/*
 * Host stub for ESP-IDF's nvs_flash.h. See nvs.h for the backing store.
 */
#pragma once

#include "esp_err.h"
#include "nvs.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);
esp_err_t nvs_flash_deinit(void);

#ifdef __cplusplus
}
#endif
