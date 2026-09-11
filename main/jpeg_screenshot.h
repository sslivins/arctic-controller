/*
 * Hardware JPEG encoding for screenshots.
 *
 * The ESP32-P4 carries a hardware JPEG codec (CONFIG_SOC_JPEG_ENCODE_SUPPORTED),
 * so a screenshot can be compressed by the peripheral instead of being shipped
 * as a ~2.7 MB uncompressed PNG. That matters for more than wall-clock time:
 * every byte of the response is also a byte pushed through mbedTLS, and the
 * chunk storm from the uncompressed path is what made #234 visible.
 *
 * Usage is three calls, and jpeg_screenshot_end() is safe to call on a context
 * that begin() failed to populate, so the caller can use a single cleanup path:
 *
 *     jpeg_screenshot_ctx_t ctx;
 *     jpeg_screenshot_begin(&ctx, w, h);     // allocates DMA-capable buffers
 *     ... render BGR888 pixels into ctx.in_buf ...
 *     jpeg_screenshot_encode(&ctx, w, h, quality, &len);
 *     ... send ctx.out_buf[0..len) ...
 *     jpeg_screenshot_end(&ctx);
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default encode quality, if the caller does not specify one. */
#define JPEG_SCREENSHOT_DEFAULT_QUALITY 80

/** Encoder MCU size. Frame dimensions must be a multiple of this. */
#define JPEG_SCREENSHOT_MCU 8

typedef struct {
    uint8_t *in_buf;       /*!< BGR888 input, DMA-capable and cache-aligned. */
    size_t   in_capacity;  /*!< Actual allocated size, >= the requested size. */
    uint8_t *out_buf;      /*!< JPEG output, DMA-capable and cache-aligned. */
    size_t   out_capacity; /*!< Actual allocated size, >= the requested size. */
} jpeg_screenshot_ctx_t;

/**
 * Allocate the DMA-capable input and output buffers for one frame.
 *
 * The buffers come from jpeg_alloc_encoder_mem(), not plain heap_caps_malloc(),
 * because the peripheral requires specific address and size alignment. Both
 * land in PSRAM.
 *
 * @return ESP_ERR_INVALID_SIZE if w or h is not a multiple of
 *         JPEG_SCREENSHOT_MCU, ESP_ERR_NO_MEM if either allocation fails.
 */
esp_err_t jpeg_screenshot_begin(jpeg_screenshot_ctx_t *ctx, uint32_t w, uint32_t h);

/**
 * Encode ctx->in_buf into ctx->out_buf.
 *
 * The input must be BGR888 — byte order B, G, R — which is exactly what LVGL's
 * LV_COLOR_FORMAT_RGB888 already produces, so no channel swap is needed on the
 * way in. ESP-IDF names the format JPEG_ENCODE_IN_FORMAT_RGB888 but defines it
 * as ESP_COLOR_FOURCC_BGR24; the name refers to the channel set, not the byte
 * order.
 *
 * The encoder engine is created and destroyed per call. Screenshots are rare,
 * and holding the engine open would pin DMA descriptors for a peripheral that
 * is otherwise idle.
 *
 * @param quality 1-100. Clamped into range; 0 selects the default.
 * @param out_len Receives the encoded length in bytes.
 */
esp_err_t jpeg_screenshot_encode(jpeg_screenshot_ctx_t *ctx,
                                 uint32_t w, uint32_t h,
                                 uint8_t quality,
                                 uint32_t *out_len);

/** Release both buffers. Idempotent and null-safe. */
void jpeg_screenshot_end(jpeg_screenshot_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
