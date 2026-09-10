/*
 * Host stub for esp_crc.h. See the header for why this matches IDF's
 * algorithm rather than being any convenient hash.
 */

#include "esp_crc.h"

namespace {

uint32_t table[256];
bool built = false;

void build_table() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    built = true;
}

}  // namespace

extern "C" uint32_t esp_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len) {
    if (!built) {
        build_table();
    }
    uint32_t c = ~crc;
    for (uint32_t i = 0; i < len; ++i) {
        c = table[(c ^ buf[i]) & 0xff] ^ (c >> 8);
    }
    return ~c;
}
