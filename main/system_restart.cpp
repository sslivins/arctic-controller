#include "system_restart.h"

#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "history_storage.h"
#include "telemetry_history.h"

static const char* TAG = "system_restart";

void system_safe_restart(void) {
    ESP_LOGW(TAG, "Safe restart requested - quiescing flash writers");

    // Stop the periodic telemetry recorder so it launches no further partition
    // writes, then wait for the recorder task to actually exit.
    telemetry_history_stop();

    // Permanently gate all event/telemetry partition writes and wait out any
    // in-flight write. After this returns, none of our writers will touch flash.
    history_storage_begin_reboot();

    // Give any in-flight NVS commit (e.g. timezone write on NTP sync, cert/wifi
    // saves) time to finish. NVS operations are short; this is a safety margin
    // so no SPI-flash op is running when esp_restart_noos() stalls the other
    // core and flushes the cache.
    vTaskDelay(pdMS_TO_TICKS(400));

    ESP_LOGW(TAG, "Flash writers quiesced - restarting now");
    esp_restart();
}
