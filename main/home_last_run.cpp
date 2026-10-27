#include "home_last_run.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

static const char* TAG = "home_last_run";

static constexpr uint32_t RETENTION_S = HISTORY_TELEMETRY_RETENTION_DAYS * 24 * 60 * 60;
static constexpr size_t OLDER_CAP = RETENTION_S / HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC + 64;

void home_last_run_resolve(home_run_summary_t* sum,
                           const history_telemetry_sample_t* window, size_t n,
                           uint32_t window_start, uint32_t end) {
    if (!home_stats_needs_older(sum, window, n)) return;
    const uint32_t oldest = end > RETENTION_S ? end - RETENTION_S : 0;
    if (oldest >= window_start) return;

    auto* older = static_cast<history_telemetry_sample_t*>(heap_caps_malloc(
        sizeof(history_telemetry_sample_t) * OLDER_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (older == nullptr) {
        ESP_LOGW(TAG, "no memory to look past the summary window");
        return;
    }
    size_t m = 0;
    if (history_storage_query_telemetry(oldest, window_start, older, OLDER_CAP, &m) == ESP_OK) {
        home_stats_merge_older(sum, window, n, older, m);
    }
    heap_caps_free(older);
}
