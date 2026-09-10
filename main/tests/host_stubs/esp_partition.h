/*
 * Host stub for ESP-IDF's esp_partition.h, backed by a RAM image with NOR
 * flash semantics. See esp_partition_fake.h for the test-facing controls.
 *
 * The point of modelling NOR rather than plain memory: on real flash a write
 * can only clear bits, so writing over a region that was not erased first
 * silently ANDs into whatever was there. That bug is invisible against a
 * memcpy-style fake -- the data reads back exactly as written -- and shows up
 * on the device as corrupt records much later.
 */
#pragma once

#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_PARTITION_TYPE_APP = 0x00,
    ESP_PARTITION_TYPE_DATA = 0x01,
    ESP_PARTITION_TYPE_ANY = 0xff,
} esp_partition_type_t;

typedef enum {
    ESP_PARTITION_SUBTYPE_ANY = 0xff,
} esp_partition_subtype_t;

typedef struct {
    esp_partition_type_t type;
    esp_partition_subtype_t subtype;
    uint32_t address;
    uint32_t size;
    uint32_t erase_size;
    char label[17];
    bool encrypted;
} esp_partition_t;

const esp_partition_t *esp_partition_find_first(esp_partition_type_t type,
                                                esp_partition_subtype_t subtype,
                                                const char *label);

esp_err_t esp_partition_read(const esp_partition_t *partition, size_t src_offset,
                             void *dst, size_t size);
esp_err_t esp_partition_write(const esp_partition_t *partition, size_t dst_offset,
                              const void *src, size_t size);
esp_err_t esp_partition_erase_range(const esp_partition_t *partition,
                                    size_t offset, size_t size);

#ifdef __cplusplus
}
#endif
