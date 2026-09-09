/*
 * esp_rom_crc32_le on the host. See esp_rom_crc.h.
 */

#include "esp_rom_crc.h"

#include "esp_crc.h"

extern "C" uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len) {
    return esp_crc32_le(crc, buf, len);
}
