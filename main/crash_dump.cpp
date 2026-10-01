/*
 * Arctic Heat Pump Controller
 * Core dump access - see crash_dump.h
 */
#include "crash_dump.h"

#include <esp_core_dump.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <inttypes.h>

static const char* TAG = "crash_dump";

static const esp_partition_t* coredump_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
}

size_t crash_dump_size(void)
{
    // Checked first so devices without the partition don't log an IDF error
    // on every call.
    if (!coredump_partition()) return 0;
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK) return 0;
    return size;
}

esp_err_t crash_dump_read(size_t offset, void* buf, size_t len)
{
    const esp_partition_t* part = coredump_partition();
    if (!part) return ESP_ERR_NOT_FOUND;
    size_t size = crash_dump_size();
    if (offset > size || len > size - offset) return ESP_ERR_INVALID_SIZE;
    return esp_partition_read(part, offset, buf, len);
}

esp_err_t crash_dump_erase(void)
{
    if (!coredump_partition() || crash_dump_size() == 0) return ESP_OK;
    return esp_core_dump_image_erase();
}

void crash_dump_report_at_boot(void)
{
    if (!coredump_partition()) {
        ESP_LOGI(TAG, "No coredump partition (old partition table); crashes are not saved");
        return;
    }
    size_t size = crash_dump_size();
    if (size == 0) {
        ESP_LOGI(TAG, "No core dump from a previous crash");
        return;
    }
    if (esp_core_dump_image_check() != ESP_OK) {
        ESP_LOGW(TAG, "Core dump present (%u bytes) but its checksum is bad", (unsigned)size);
        return;
    }

    esp_core_dump_summary_t summary = {};
    if (esp_core_dump_get_summary(&summary) == ESP_OK) {
        ESP_LOGW(TAG, "Core dump present (%u bytes): task '%s' crashed at PC 0x%08" PRIx32,
                 (unsigned)size, summary.exc_task, summary.exc_pc);
    } else {
        ESP_LOGW(TAG, "Core dump present (%u bytes)", (unsigned)size);
    }
    char reason[128] = {};
    if (esp_core_dump_get_panic_reason(reason, sizeof(reason)) == ESP_OK) {
        ESP_LOGW(TAG, "Core dump panic reason: %s", reason);
    }
    ESP_LOGW(TAG, "Fetch it with GET /api/logs/coredump");
}
