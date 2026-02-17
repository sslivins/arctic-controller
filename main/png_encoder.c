/*
 * PNG encoder for screenshots — uses lodepng with PSRAM-backed allocators.
 *
 * LVGL's built-in lodepng routes malloc through lv_malloc() which uses LVGL's
 * small internal memory pool — far too small for zlib deflate on a 720×1280
 * image.  This file provides png_encode_rgb888() which wraps lodepng compiled
 * with custom allocators that use stdlib malloc() → PSRAM via ESP-IDF.
 *
 * The uncompressed fallback is kept for safety but shouldn't be needed now.
 */

#include "png_encoder.h"
#include <string.h>
#include <stdlib.h>
#include <esp_heap_caps.h>

/*
 * Tell lodepng to NOT compile its built-in allocators — we provide our own
 * that use stdlib malloc (→ PSRAM).
 */
#define LODEPNG_NO_COMPILE_ALLOCATORS
#define LODEPNG_NO_COMPILE_DECODER
#define LODEPNG_NO_COMPILE_DISK
#define LODEPNG_NO_COMPILE_ANCILLARY_CHUNKS
#define LODEPNG_NO_COMPILE_CPP

/* Prevent lvgl.h from being included by lodepng.h */
#ifndef LVGL_H
#define LVGL_H
#endif

/* Provide the minimal defines that lodepng.h expects from lvgl.h */
#define LV_USE_LODEPNG 1
#define LV_STDDEF_INCLUDE <stddef.h>
#define LV_ATTRIBUTE_EXTERN_DATA

/*
 * Include lodepng implementation directly.  All symbols become static-like
 * in this translation unit (except the allocators which are extern).
 * This avoids clashing with LVGL's own lodepng symbols because LVGL's copy
 * uses lv_malloc-based allocators in its own translation unit.
 *
 * Note: we include the .c file so all internal lodepng functions are
 * compiled here with our defines.
 */
#include "../dependencies/lvgl/src/libs/lodepng/lodepng.c"

/* --- Custom allocators routed to stdlib (→ PSRAM) --- */

void *lodepng_malloc(size_t size)
{
    return malloc(size);
}

void *lodepng_realloc(void *ptr, size_t new_size)
{
    return realloc(ptr, new_size);
}

void lodepng_free(void *ptr)
{
    free(ptr);
}

/* --- Public API --- */

png_encode_result_t png_encode_rgb888(const uint8_t *pixels, uint32_t w, uint32_t h,
                                       uint8_t **out_png, size_t *out_size)
{
    if (!pixels || !out_png || !out_size || w == 0 || h == 0) {
        return PNG_ENCODE_INVALID_ARG;
    }

    unsigned char *png_data = NULL;
    size_t png_size = 0;
    unsigned err = lodepng_encode24(&png_data, &png_size, pixels, w, h);
    if (err) {
        if (png_data) free(png_data);
        return PNG_ENCODE_OOM;  /* most likely cause on embedded */
    }

    /*
     * Move the output to a PSRAM-backed buffer so the caller can use
     * heap_caps_free() consistently.  lodepng used our malloc which already
     * goes to PSRAM for large allocs, so this is fine as-is.
     */
    *out_png = png_data;
    *out_size = png_size;
    return PNG_ENCODE_OK;
}
