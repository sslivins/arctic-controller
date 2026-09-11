/*
 * Hardware JPEG encoding for screenshots. See jpeg_screenshot.h.
 */
#include "jpeg_screenshot.h"

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "driver/jpeg_encode.h"
#include "esp_log.h"

static const char *TAG = "jpeg_shot";

/*
 * YUV444 keeps full chroma resolution. A screenshot is mostly thin text and
 * single-pixel borders on saturated backgrounds, which is precisely the content
 * that 4:2:0 chroma subsampling smears. The file is larger than 4:2:0 would be
 * and still an order of magnitude smaller than the uncompressed PNG, so the
 * trade is worth taking for an image whose whole purpose is to be read.
 */
#define JPEG_SHOT_SUBSAMPLING JPEG_DOWN_SAMPLING_YUV444

/*
 * Encoding 720x1280 takes single-digit milliseconds on the P4. A second is far
 * beyond any plausible encode time, so this trips only on a wedged peripheral
 * rather than masking slowness.
 */
#define JPEG_SHOT_TIMEOUT_MS 1000

/*
 * Output capacity, as bytes per pixel. A UI screenshot at quality 80 lands
 * nearer 0.1 bpp, so one byte per pixel is a wide margin; the peripheral fails
 * the encode rather than overrunning if it is ever not enough.
 */
#define JPEG_SHOT_OUT_BPP 1

esp_err_t jpeg_screenshot_begin(jpeg_screenshot_ctx_t *ctx, uint32_t w, uint32_t h)
{
    if (ctx == NULL || w == 0 || h == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(ctx, 0, sizeof(*ctx));

    if ((w % JPEG_SCREENSHOT_MCU) != 0 || (h % JPEG_SCREENSHOT_MCU) != 0) {
        ESP_LOGE(TAG, "%" PRIu32 "x%" PRIu32 " is not a multiple of %d",
                 w, h, JPEG_SCREENSHOT_MCU);
        return ESP_ERR_INVALID_SIZE;
    }

    jpeg_encode_memory_alloc_cfg_t in_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER,
    };
    ctx->in_buf = (uint8_t *)jpeg_alloc_encoder_mem((size_t)w * h * 3, &in_cfg,
                                                    &ctx->in_capacity);
    if (ctx->in_buf == NULL) {
        ESP_LOGE(TAG, "input buffer alloc failed (%" PRIu32 " bytes)", w * h * 3);
        return ESP_ERR_NO_MEM;
    }

    jpeg_encode_memory_alloc_cfg_t out_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    ctx->out_buf = (uint8_t *)jpeg_alloc_encoder_mem((size_t)w * h * JPEG_SHOT_OUT_BPP,
                                                     &out_cfg, &ctx->out_capacity);
    if (ctx->out_buf == NULL) {
        ESP_LOGE(TAG, "output buffer alloc failed");
        jpeg_screenshot_end(ctx);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t jpeg_screenshot_encode(jpeg_screenshot_ctx_t *ctx,
                                 uint32_t w, uint32_t h,
                                 uint8_t quality,
                                 uint32_t *out_len)
{
    if (ctx == NULL || ctx->in_buf == NULL || ctx->out_buf == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_len = 0;

    if (quality == 0) {
        quality = JPEG_SCREENSHOT_DEFAULT_QUALITY;
    } else if (quality > 100) {
        quality = 100;
    }

    jpeg_encode_engine_cfg_t eng_cfg = {
        .timeout_ms = JPEG_SHOT_TIMEOUT_MS,
    };
    jpeg_encoder_handle_t engine = NULL;
    esp_err_t err = jpeg_new_encoder_engine(&eng_cfg, &engine);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_new_encoder_engine failed: %s", esp_err_to_name(err));
        return err;
    }

    jpeg_encode_cfg_t enc_cfg = {
        .width = w,
        .height = h,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB888,  /* BGR24 byte order, see header */
        .sub_sample = JPEG_SHOT_SUBSAMPLING,
        .image_quality = quality,
    };

    err = jpeg_encoder_process(engine, &enc_cfg,
                               ctx->in_buf, (uint32_t)((size_t)w * h * 3),
                               ctx->out_buf, (uint32_t)ctx->out_capacity,
                               out_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_encoder_process failed: %s", esp_err_to_name(err));
    }

    /*
     * Destroy the engine even on failure. Leaking it would strand the
     * peripheral and make every subsequent screenshot fail, turning a single
     * bad encode into a permanent one.
     */
    esp_err_t del_err = jpeg_del_encoder_engine(engine);
    if (del_err != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_del_encoder_engine failed: %s", esp_err_to_name(del_err));
        if (err == ESP_OK) {
            err = del_err;
        }
    }

    return err;
}

void jpeg_screenshot_end(jpeg_screenshot_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    free(ctx->in_buf);
    free(ctx->out_buf);
    ctx->in_buf = NULL;
    ctx->out_buf = NULL;
    ctx->in_capacity = 0;
    ctx->out_capacity = 0;
}
