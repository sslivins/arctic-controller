#include "telemetry_history.h"

#include "heatpump_controller.h"
#include "history_storage.h"
#include "time_manager.h"
#include <atomic>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <time.h>

static const char* TAG = "telemetry_history";
static TaskHandle_t s_task = nullptr;
static SemaphoreHandle_t s_stopped = nullptr;
static std::atomic_bool s_stop_requested{false};

static uint8_t storage_mode(arctic::TelemetryOperation operation) {
    switch (operation) {
        case arctic::TelemetryOperation::HEATING:
            return HISTORY_TELEMETRY_MODE_HEATING;
        case arctic::TelemetryOperation::COOLING:
            return HISTORY_TELEMETRY_MODE_COOLING;
        case arctic::TelemetryOperation::HOT_WATER:
            return HISTORY_TELEMETRY_MODE_HOT_WATER;
        default:
            return HISTORY_TELEMETRY_MODE_UNKNOWN;
    }
}

static void recorder_task(void*) {
    uint32_t last_timestamp = 0;
    history_storage_latest_telemetry_timestamp(&last_timestamp);

    while (!s_stop_requested.load()) {
        ulTaskNotifyTake(pdTRUE,
                         pdMS_TO_TICKS(HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC * 1000));
        if (s_stop_requested.load()) break;
        if (!time_mgr_is_synced()) continue;

        time_t now;
        time(&now);
        if (now <= 0 || (uint32_t)now <= last_timestamp) continue;

        const arctic::TelemetrySnapshot snapshot =
            arctic::getTelemetrySnapshot();
        history_telemetry_sample_t sample = {};
        sample.timestamp = (uint32_t)now;
        sample.inlet_deci_c = (int16_t)(snapshot.inlet_c * 10);
        sample.outlet_deci_c = (int16_t)(snapshot.outlet_c * 10);
        sample.setpoint_deci_c = (int16_t)(snapshot.active_setpoint_c * 10);
        sample.mode = storage_mode(snapshot.operation);
        if (snapshot.connected) sample.flags |= HISTORY_TELEMETRY_CONNECTED;
        if (snapshot.compressor_valid) {
            sample.flags |= HISTORY_TELEMETRY_COMPRESSOR_VALID;
        }
        if (snapshot.compressor_running) {
            sample.flags |= HISTORY_TELEMETRY_COMPRESSOR_RUNNING;
        }
        if (snapshot.inlet_valid) sample.flags |= HISTORY_TELEMETRY_INLET_VALID;
        if (snapshot.outlet_valid) sample.flags |= HISTORY_TELEMETRY_OUTLET_VALID;
        if (snapshot.setpoint_valid) {
            sample.flags |= HISTORY_TELEMETRY_SETPOINT_VALID;
        }

        esp_err_t err = history_storage_append_telemetry(&sample);
        if (err == ESP_OK) {
            last_timestamp = sample.timestamp;
        } else if (err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Failed to append telemetry sample: %s",
                     esp_err_to_name(err));
        }
    }

    s_task = nullptr;
    xSemaphoreGive(s_stopped);
    vTaskDelete(nullptr);
}

esp_err_t telemetry_history_init(void) {
    if (s_task != nullptr) return ESP_OK;
    if (s_stopped == nullptr) {
        s_stopped = xSemaphoreCreateBinary();
        if (s_stopped == nullptr) return ESP_ERR_NO_MEM;
    }
    s_stop_requested.store(false);
    BaseType_t created = xTaskCreate(
        recorder_task, "telemetry_history", 4096, nullptr, 3, &s_task);
    if (created != pdPASS) {
        s_task = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Telemetry recorder started (%ds interval)",
             HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC);
    return ESP_OK;
}

void telemetry_history_prepare_factory_reset(void) {
    s_stop_requested.store(true);
    TaskHandle_t task = s_task;
    if (task != nullptr) {
        xTaskNotifyGive(task);
        if (xSemaphoreTake(s_stopped, pdMS_TO_TICKS(5000)) != pdTRUE) {
            ESP_LOGE(TAG, "Timed out waiting for telemetry recorder to stop");
        }
    }
    history_storage_prepare_factory_reset();
}
