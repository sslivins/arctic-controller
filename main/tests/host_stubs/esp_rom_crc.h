/*
 * Host stub for ESP-IDF's esp_rom_crc.h.
 *
 * Delegates to esp_crc32_le (see esp_crc.h): on the device esp_crc32_le() is
 * itself a thin wrapper over the ROM routine, so the two must agree by
 * definition. Keeping one implementation here means a record written by
 * history_storage and one written by log_persist are checksummed identically
 * on the host, exactly as they are on hardware.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len);

#ifdef __cplusplus
}
#endif
