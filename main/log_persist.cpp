/*
 * Arctic Heat Pump Controller
 * Persistent debug log implementation — see log_persist.h.
 *
 * On-flash layout: the region is divided into fixed 32 KB slots written
 * round-robin. Each slot begins with lp_header_t followed by up to
 * (SLOT_SIZE - header) bytes of NUL-free log text. A record is valid when the
 * magic matches and a CRC32 over the payload checks out; the newest valid slot
 * (highest seq) is the live snapshot. Writing a fresh slot never touches the
 * previous one, so a torn/interrupted write (e.g. a reboot mid-flush) at worst
 * produces one CRC-invalid slot that is ignored — the prior snapshot survives.
 */
#include "log_persist.h"

#include <string.h>
#include <stdio.h>

#include <esp_log.h>
#include <esp_partition.h>
#include <esp_random.h>
#include <esp_rom_crc.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include "log_buffer.h"
#include "net_diag.h"

static const char* TAG = "log_persist";

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------
#define LP_SLOT_SIZE        (32 * 1024)     // must be a multiple of the 4KB erase unit
#define LP_MAGIC            0xD10B0106u      // "dbglog" marker
#define LP_HEADER_PAD       0

#define LP_TASK_PERIOD_MS   5000
#define LP_NETDIAG_EVERY    6                // 6 * 5s = 30s netdiag cadence
#define LP_SEVERITY_DEBOUNCE_MS 10000        // min gap between severity-triggered flushes
#define LP_HEARTBEAT_MS     300000           // periodic flush even with no new warnings

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t seq;        // monotonic; higher = newer
    uint32_t boot_id;    // random id of the boot that wrote this record
    uint32_t len;        // payload text length (bytes, excludes header)
    uint32_t crc;        // esp_rom_crc32_le over the payload text
    uint8_t  reason;     // LOG_PERSIST_REASON_*
    uint8_t  pad[3];
} lp_header_t;

#define LP_HEADER_SIZE   (sizeof(lp_header_t))
#define LP_PAYLOAD_MAX   (LP_SLOT_SIZE - LP_HEADER_SIZE)

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static const esp_partition_t* s_part = NULL;
static int          s_n_slots = 0;
static SemaphoreHandle_t s_mutex = NULL;

static uint32_t     s_boot_id = 0;
static uint32_t     s_next_seq = 1;    // seq to assign to the next write
static int          s_next_slot = 0;   // slot index for the next write

// Snapshot captured at init from the PREVIOUS boot (never overwritten during
// this boot). Owned by this module.
static char*        s_prev_text = NULL;
static log_persist_prev_info_t s_prev_info = {0};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static inline size_t round_up4(size_t n) { return (n + 3u) & ~((size_t)3u); }

static const esp_partition_t* find_dbglog_partition(void)
{
    // Current layout uses label "dbglog"; devices that only received an app OTA
    // (not a full flash) still carry the legacy "human_face_det" label.
    const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "dbglog");
    if (!p) {
        p = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "human_face_det");
        if (p) {
            ESP_LOGW(TAG, "using legacy 'human_face_det' partition for debug log "
                          "(device predates the dbglog relabel)");
        }
    }
    return p;
}

// Build the log-tail text into a freshly-heap-allocated buffer. Returns the
// buffer (caller frees) and sets *out_len, or NULL on allocation failure.
// The result keeps the MOST RECENT lines when the ring exceeds the payload cap.
static char* build_tail_text(size_t* out_len)
{
    *out_len = 0;

    const int cap_entries = LOG_BUFFER_MAX_ENTRIES;
    log_entry_t* entries = (log_entry_t*)heap_caps_malloc(
        sizeof(log_entry_t) * cap_entries, MALLOC_CAP_SPIRAM);
    if (!entries) return NULL;

    int n = log_buffer_get(entries, cap_entries, 0, ESP_LOG_VERBOSE);  // all, oldest-first

    // Generous scratch buffer for the formatted text; trimmed to payload cap.
    const size_t scratch_sz = 96 * 1024;
    char* scratch = (char*)heap_caps_malloc(scratch_sz, MALLOC_CAP_SPIRAM);
    if (!scratch) { heap_caps_free(entries); return NULL; }

    size_t used = 0;
    for (int i = 0; i < n && used < scratch_sz - 1; i++) {
        const log_entry_t* e = &entries[i];
        int w = snprintf(scratch + used, scratch_sz - used,
                         "%8lu %s %s: %s\n",
                         (unsigned long)e->uptime_ms,
                         log_level_char(e->level),
                         e->tag, e->message);
        if (w < 0) break;
        if ((size_t)w >= scratch_sz - used) { used = scratch_sz - 1; break; }
        used += (size_t)w;
    }
    scratch[used] = '\0';
    heap_caps_free(entries);

    // Keep the newest lines that fit in a slot payload. If oversized, advance to
    // the first line boundary so we don't emit a half line at the top.
    const char* start = scratch;
    if (used > LP_PAYLOAD_MAX) {
        start = scratch + (used - LP_PAYLOAD_MAX);
        const char* nl = strchr(start, '\n');
        if (nl && (size_t)(nl + 1 - scratch) <= used) start = nl + 1;
    }
    size_t keep = used - (size_t)(start - scratch);

    char* result = (char*)heap_caps_malloc(keep + 1, MALLOC_CAP_SPIRAM);
    if (!result) { heap_caps_free(scratch); return NULL; }
    memcpy(result, start, keep);
    result[keep] = '\0';
    heap_caps_free(scratch);

    *out_len = keep;
    return result;
}

// Write a snapshot to the next slot. Must hold s_mutex.
static void flush_locked(uint8_t reason)
{
    if (!s_part) return;

    size_t text_len = 0;
    char* text = build_tail_text(&text_len);
    if (!text) {
        ESP_LOGW(TAG, "flush: failed to build tail text (out of PSRAM?)");
        return;
    }
    if (text_len > LP_PAYLOAD_MAX) text_len = LP_PAYLOAD_MAX;

    const size_t rec_len = round_up4(LP_HEADER_SIZE + text_len);
    uint8_t* rec = (uint8_t*)heap_caps_malloc(rec_len, MALLOC_CAP_SPIRAM);
    if (!rec) {
        ESP_LOGW(TAG, "flush: failed to alloc record buffer");
        heap_caps_free(text);
        return;
    }
    memset(rec, 0, rec_len);

    lp_header_t* h = (lp_header_t*)rec;
    h->magic   = LP_MAGIC;
    h->seq     = s_next_seq;
    h->boot_id = s_boot_id;
    h->len     = (uint32_t)text_len;
    h->crc     = esp_rom_crc32_le(0, (const uint8_t*)text, text_len);
    h->reason  = reason;
    memcpy(rec + LP_HEADER_SIZE, text, text_len);
    heap_caps_free(text);

    const size_t slot_off = (size_t)s_next_slot * LP_SLOT_SIZE;
    esp_err_t err = esp_partition_erase_range(s_part, slot_off, LP_SLOT_SIZE);
    if (err == ESP_OK) {
        err = esp_partition_write(s_part, slot_off, rec, rec_len);
    }
    heap_caps_free(rec);

    if (err == ESP_OK) {
        ESP_LOGD(TAG, "snapshot seq=%lu slot=%d bytes=%u reason=%u",
                 (unsigned long)s_next_seq, s_next_slot,
                 (unsigned)text_len, (unsigned)reason);
        s_next_seq++;
        s_next_slot = (s_next_slot + 1) % s_n_slots;
    } else {
        ESP_LOGW(TAG, "snapshot write failed: %s", esp_err_to_name(err));
    }
}

// Scan all slots, locate the newest valid record, load it as the "previous"
// snapshot, and set the next write position past it.
static void scan_and_load_previous(void)
{
    uint32_t best_seq = 0;
    int best_slot = -1;
    lp_header_t best_hdr = {0};

    for (int slot = 0; slot < s_n_slots; slot++) {
        const size_t off = (size_t)slot * LP_SLOT_SIZE;
        lp_header_t hdr;
        if (esp_partition_read(s_part, off, &hdr, sizeof(hdr)) != ESP_OK) continue;
        if (hdr.magic != LP_MAGIC) continue;
        if (hdr.len == 0 || hdr.len > LP_PAYLOAD_MAX) continue;

        char* payload = (char*)heap_caps_malloc(hdr.len, MALLOC_CAP_SPIRAM);
        if (!payload) continue;
        if (esp_partition_read(s_part, off + LP_HEADER_SIZE, payload, hdr.len) != ESP_OK) {
            heap_caps_free(payload);
            continue;
        }
        uint32_t crc = esp_rom_crc32_le(0, (const uint8_t*)payload, hdr.len);
        heap_caps_free(payload);
        if (crc != hdr.crc) continue;

        if (hdr.seq >= best_seq) {   // >= so ties resolve to a real slot
            best_seq = hdr.seq;
            best_slot = slot;
            best_hdr = hdr;
        }
    }

    if (best_slot < 0) {
        ESP_LOGI(TAG, "no valid previous snapshot found");
        s_next_seq = 1;
        s_next_slot = 0;
        return;
    }

    // Load the winning payload for the API.
    const size_t off = (size_t)best_slot * LP_SLOT_SIZE;
    s_prev_text = (char*)heap_caps_malloc(best_hdr.len + 1, MALLOC_CAP_SPIRAM);
    if (s_prev_text) {
        if (esp_partition_read(s_part, off + LP_HEADER_SIZE, s_prev_text, best_hdr.len) == ESP_OK) {
            s_prev_text[best_hdr.len] = '\0';
            s_prev_info.present = true;
            s_prev_info.seq     = best_hdr.seq;
            s_prev_info.boot_id = best_hdr.boot_id;
            s_prev_info.len     = best_hdr.len;
            s_prev_info.reason  = best_hdr.reason;
        } else {
            heap_caps_free(s_prev_text);
            s_prev_text = NULL;
        }
    }

    s_next_seq = best_hdr.seq + 1;
    s_next_slot = (best_slot + 1) % s_n_slots;

    ESP_LOGI(TAG, "previous snapshot: seq=%lu boot_id=%08lx len=%lu reason=%u (slot %d)",
             (unsigned long)best_hdr.seq, (unsigned long)best_hdr.boot_id,
             (unsigned long)best_hdr.len, (unsigned)best_hdr.reason, best_slot);
}

// ---------------------------------------------------------------------------
// Background task
// ---------------------------------------------------------------------------
static void log_persist_task(void* arg)
{
    (void)arg;
    uint32_t tick = 0;
    uint32_t last_flushed_warn_seq = 0;
    int64_t  last_flush_us = 0;

    // Baseline: whatever warnings are already in the ring at task start should
    // not immediately trigger a "new severity" flush.
    last_flushed_warn_seq = log_buffer_latest_seq_at_level(ESP_LOG_WARN);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(LP_TASK_PERIOD_MS));
        tick++;

        if ((tick % LP_NETDIAG_EVERY) == 0) {
            net_diag_log_snapshot();
        }

        const int64_t now_us = esp_timer_get_time();
        uint32_t warn_seq = log_buffer_latest_seq_at_level(ESP_LOG_WARN);

        bool do_flush = false;
        uint8_t reason = LOG_PERSIST_REASON_HEARTBEAT;

        if (warn_seq > last_flushed_warn_seq &&
            (last_flush_us == 0 ||
             (now_us - last_flush_us) >= (int64_t)LP_SEVERITY_DEBOUNCE_MS * 1000)) {
            do_flush = true;
            reason = LOG_PERSIST_REASON_SEVERITY;
        } else if (last_flush_us == 0 ||
                   (now_us - last_flush_us) >= (int64_t)LP_HEARTBEAT_MS * 1000) {
            do_flush = true;
            reason = LOG_PERSIST_REASON_HEARTBEAT;
        }

        if (do_flush) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            flush_locked(reason);
            xSemaphoreGive(s_mutex);
            last_flush_us = now_us;
            last_flushed_warn_seq = warn_seq;
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool log_persist_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_boot_id = esp_random();

    s_part = find_dbglog_partition();
    if (!s_part) {
        ESP_LOGW(TAG, "no dbglog/human_face_det partition — persistence disabled");
        return false;
    }
    s_n_slots = (int)(s_part->size / LP_SLOT_SIZE);
    if (s_n_slots < 1) {
        ESP_LOGE(TAG, "partition too small (%lu bytes) for a %d-byte slot",
                 (unsigned long)s_part->size, LP_SLOT_SIZE);
        s_part = NULL;
        return false;
    }

    ESP_LOGI(TAG, "dbglog at 0x%08lx size=%lu slots=%d boot_id=%08lx",
             (unsigned long)s_part->address, (unsigned long)s_part->size,
             s_n_slots, (unsigned long)s_boot_id);

    scan_and_load_previous();
    return true;
}

void log_persist_start(void)
{
    if (!s_part) return;
    xTaskCreate(log_persist_task, "log_persist", 4096, NULL, 2, NULL);
}

void log_persist_flush_now(uint8_t reason)
{
    if (!s_part || !s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    flush_locked(reason);
    xSemaphoreGive(s_mutex);
}

size_t log_persist_get_previous(char* out, size_t cap)
{
    if (!s_prev_text || !out || cap == 0) {
        if (out && cap) out[0] = '\0';
        return 0;
    }
    size_t len = s_prev_info.len;
    size_t copy = (len < cap - 1) ? len : cap - 1;
    memcpy(out, s_prev_text, copy);
    out[copy] = '\0';
    return len;
}

void log_persist_get_previous_info(log_persist_prev_info_t* out)
{
    if (!out) return;
    *out = s_prev_info;
}
