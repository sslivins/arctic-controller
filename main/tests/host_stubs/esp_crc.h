/*
 * Host stub for ESP-IDF's esp_crc.h.
 *
 * Implements the same CRC-32 that IDF's esp_crc32_le() computes: reflected,
 * polynomial 0xEDB88320, with the input crc inverted on entry and the result
 * inverted on exit -- so esp_crc32_le(0, ...) is the usual zlib crc32.
 *
 * Exactness matters here in one direction only. The host tests both write and
 * verify records with this function, so they would be self-consistent under
 * any hash; what they would NOT catch is a change that makes the host and the
 * device disagree. Matching the real algorithm keeps a stored CRC comparable
 * with one computed on the device, which is what makes a test like "a record
 * written by an older firmware still validates" meaningful.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t esp_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len);

#ifdef __cplusplus
}
#endif
