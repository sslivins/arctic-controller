#include "system_restart.h"

#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_private/cache_utils.h"

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

    // The real hazard is esp_restart_noos(): on this dual-core, SPIRAM-enabled
    // target it stalls the *other* core and then executes the ROM routine
    // Cache_WriteBack_All(). If that other core happened to be in the middle of
    // a SPI-flash operation at the moment it was stalled, the cache is disabled
    // and in an inconsistent state, so the ROM cache write-back dereferences an
    // invalid address and panics (Store access fault, MCAUSE 0x7, inside
    // Cache_WriteBack_All at ROM 0x4fc10a60). Gating only *our* partition
    // writers above is not enough, because arbitrary NVS commits (timezone on
    // NTP sync, TLS certs, Wi-Fi creds, app prefs, boot_stats, ...) can still be
    // mid-flight and are not under our control.
    //
    // Every flash operation - legacy spi_flash and the esp_flash/NVS path alike
    // - must first acquire spi_flash_op_lock() before it disables the cache
    // (see spi_flash_disable_interrupts_caches_and_other_cpu()). By taking that
    // same lock here and never releasing it, we deterministically guarantee that
    // once we proceed, no core is inside a flash op with the cache disabled:
    //   - any in-flight op has completed and released the mutex before we
    //     acquire it, and
    //   - any new op blocks on the mutex *before* touching the cache.
    // esp_restart() then runs with the cache in a normal, enabled state and the
    // ROM write-back can no longer fault. We intentionally never unlock - the
    // chip is about to reset.
    spi_flash_op_lock();

    ESP_LOGW(TAG, "Flash writers quiesced - restarting now");
    esp_restart();
}
