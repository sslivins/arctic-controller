/*
 * Minimal uncompressed PNG encoder for screenshots.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PNG_ENCODE_OK = 0,
    PNG_ENCODE_INVALID_ARG,
    PNG_ENCODE_OOM,
    PNG_ENCODE_TOO_WIDE,
} png_encode_result_t;

/**
 * Encode tightly-packed RGB888 pixel data as an uncompressed PNG.
 *
 * @param pixels  Pointer to w×h×3 bytes of RGB888 data (row-major, no stride padding)
 * @param w       Image width in pixels
 * @param h       Image height in pixels
 * @param out_png Receives heap-allocated PNG data (caller must free with heap_caps_free)
 * @param out_size Receives the size of the PNG data in bytes
 * @return PNG_ENCODE_OK on success
 */
png_encode_result_t png_encode_rgb888(const uint8_t *pixels, uint32_t w, uint32_t h,
                                       uint8_t **out_png, size_t *out_size);

#ifdef __cplusplus
}
#endif
