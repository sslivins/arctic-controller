/**
 * @file lv_mem_psram.c
 *
 * Custom LVGL memory backend that allocates widget/draw metadata from external
 * PSRAM instead of the fixed builtin pool in internal RAM.
 *
 * Why: the persistent tab shell keeps all four tab trees (Home/Status/Control/
 * Events) resident at once. The default builtin allocator uses a 128 KB static
 * pool in internal RAM (~180 KB total on this ESP32-P4), which nearly fills up
 * with the resident tabs. Loading the Settings screen on top then requested
 * more memory (gradient caches, etc.) than remained, tripping LVGL's malloc
 * assert and wedging the LVGL task (watchdog reboot / screen flashing).
 *
 * Selecting CONFIG_LV_USE_CUSTOM_MALLOC=y routes every lv_malloc/lv_realloc/
 * lv_free through the functions below. Backing them with MALLOC_CAP_SPIRAM
 * gives LVGL effectively the whole 32 MB PSRAM heap. Display DMA/draw buffers
 * are allocated separately by the panel driver and are unaffected.
 */

#include "lvgl.h"

#include "esp_heap_caps.h"

/* LVGL widget allocations are small and numerous; keep them 8-bit accessible
 * in PSRAM. They are not DMA targets, so internal RAM is not required. */
#define LV_PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void)
{
    return; /*Nothing to init - the ESP-IDF heap is already up.*/
}

void lv_mem_deinit(void)
{
    return; /*Nothing to deinit.*/
}

lv_mem_pool_t lv_mem_add_pool(void * mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL; /*Extra pools not supported - PSRAM heap is the single pool.*/
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
    return;
}

void * lv_malloc_core(size_t size)
{
    return heap_caps_malloc(size, LV_PSRAM_CAPS);
}

void * lv_realloc_core(void * p, size_t new_size)
{
    return heap_caps_realloc(p, new_size, LV_PSRAM_CAPS);
}

void lv_free_core(void * p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t * mon_p)
{
    if(mon_p == NULL) return;

    lv_memzero(mon_p, sizeof(lv_mem_monitor_t));

    multi_heap_info_t info;
    heap_caps_get_info(&info, LV_PSRAM_CAPS);

    mon_p->total_size = info.total_free_bytes + info.total_allocated_bytes;
    mon_p->free_cnt   = info.free_blocks;
    mon_p->free_size  = info.total_free_bytes;
    mon_p->free_biggest_size = info.largest_free_block;
    mon_p->used_cnt   = info.allocated_blocks;
    if(mon_p->total_size > 0) {
        mon_p->used_pct = (uint8_t)((info.total_allocated_bytes * 100) / mon_p->total_size);
        mon_p->frag_pct = (uint8_t)(mon_p->free_biggest_size * 100 /
                                    (mon_p->free_size ? mon_p->free_size : 1));
        mon_p->frag_pct = (uint8_t)(100 - mon_p->frag_pct);
    }
}

lv_result_t lv_mem_test_core(void)
{
    return heap_caps_check_integrity(LV_PSRAM_CAPS, false) ? LV_RESULT_OK : LV_RESULT_INVALID;
}
