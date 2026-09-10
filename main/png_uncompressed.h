/*
 * Minimal uncompressed PNG encoder for screenshots.
 *
 * Produces valid PNG files using DEFLATE stored (uncompressed) blocks.
 * Extremely lightweight on CPU — no zlib compression, just memcpy +
 * CRC32/Adler32 checksums.  Output is ~2.77 MB for a 720×1280 RGB888
 * image (vs ~800 KB compressed), but encoding takes <50 ms instead of
 * several seconds.
 *
 * Designed for streaming over HTTP chunked transfer — the write callback
 * is invoked incrementally so no second copy of the image is needed.
 *
 * Output is coalesced through a ~16 KB PSRAM staging buffer so that the many
 * small pieces the format requires (a 5-byte block header and a 1-byte filter
 * byte per scanline) do not each become their own HTTP chunk and TLS record.
 * Without it a 720x1280 screenshot cost ~3840 write callbacks and ~40 s over
 * HTTPS, and the resulting internal-RAM pressure was implicated in device-wide
 * TCP wedges (issue #234).
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Write callback — called repeatedly with chunks of PNG data.
 *
 * @param ctx   Opaque context (e.g. httpd_req_t*)
 * @param buf   Data to write
 * @param len   Number of bytes
 * @return ESP_OK to continue, any other value to abort encoding
 */
typedef esp_err_t (*png_write_fn_t)(void *ctx, const void *buf, size_t len);

/**
 * Stream an uncompressed PNG of tightly-packed RGB888 pixel data.
 *
 * Pixels must be row-major, no stride padding, 3 bytes per pixel (R,G,B).
 * The write callback is called multiple times with chunks of the PNG file.
 *
 * Memory usage: a single ~16 KB staging buffer allocated from PSRAM for the
 * duration of the call (plus a few bytes of stack). If that allocation fails
 * the encoder still succeeds, falling back to unbuffered writes.
 *
 * @param pixels    w×h×3 bytes of RGB888 data
 * @param w         Image width in pixels
 * @param h         Image height in pixels
 * @param write_fn  Callback to emit PNG bytes
 * @param ctx       Opaque context passed to write_fn
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for bad params,
 *         or the first error returned by write_fn
 */
esp_err_t png_encode_uncompressed_rgb888(const uint8_t *pixels, uint32_t w, uint32_t h,
                                          png_write_fn_t write_fn, void *ctx);

#ifdef __cplusplus
}
#endif
