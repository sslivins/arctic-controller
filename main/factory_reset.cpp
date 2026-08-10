#include "factory_reset.h"

#include "event_log.h"
#include "telemetry_history.h"
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

static const char* TAG = "factory_reset";

static esp_err_t erase_data_partition(const char* label)
{
    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    if (!partition) {
        ESP_LOGE(TAG, "Partition '%s' not found", label);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "Erasing %s partition (%lu bytes)", label,
             (unsigned long)partition->size);
    return esp_partition_erase_range(partition, 0, partition->size);
}

static void factory_reset_task(void* arg)
{
    (void)arg;
    telemetry_history_prepare_factory_reset();
    event_log_prepare_factory_reset();

    esp_err_t storage_err = erase_data_partition("storage");
    esp_err_t history_err = erase_data_partition("history");
    esp_err_t nvs_err = nvs_flash_erase();

    if (storage_err != ESP_OK || history_err != ESP_OK || nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "Factory reset incomplete: storage=%s history=%s nvs=%s",
                 esp_err_to_name(storage_err), esp_err_to_name(history_err),
                 esp_err_to_name(nvs_err));
    } else {
        ESP_LOGI(TAG, "Factory reset complete");
    }

    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
}

bool factory_reset_start(void)
{
    BaseType_t created = xTaskCreate(
        factory_reset_task, "factory_reset", 4096, NULL, 5, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create factory-reset task");
        return false;
    }
    return true;
}
