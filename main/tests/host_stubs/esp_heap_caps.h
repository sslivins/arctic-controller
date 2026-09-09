/*
 * Host stub for ESP-IDF's esp_heap_caps.h.
 *
 * Capability flags are accepted and ignored -- there is no PSRAM on the host,
 * and none of the host-tested sources depend on which pool they landed in.
 * What IS modelled is allocation failure, because code that does not check a
 * NULL from heap_caps_malloc crashes on a device under memory pressure and
 * nowhere else. See heap_fake.h.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_DMA (1 << 3)
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_DEFAULT (1 << 12)

#ifdef __cplusplus
extern "C" {
#endif

void *heap_caps_malloc(size_t size, uint32_t caps);
void *heap_caps_calloc(size_t n, size_t size, uint32_t caps);
void heap_caps_free(void *ptr);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_minimum_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);

#ifdef __cplusplus
}
#endif
