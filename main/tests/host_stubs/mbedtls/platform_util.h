/*
 * Host stub for mbedtls/platform_util.h.
 *
 * mbedtls_platform_zeroize must not be elidable by the optimiser -- that is
 * the whole reason the firmware calls it instead of memset when wiping token
 * and hash buffers. The volatile write below preserves that property.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline void mbedtls_platform_zeroize(void *buf, size_t len) {
    if (buf == NULL || len == 0) {
        return;
    }
    volatile unsigned char *p = (volatile unsigned char *)buf;
    while (len--) {
        *p++ = 0;
    }
}

#ifdef __cplusplus
}
#endif
