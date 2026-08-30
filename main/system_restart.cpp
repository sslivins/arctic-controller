#include "system_restart.h"

#include <esp_log.h>
#include <esp_system.h>
#include <esp_rom_sys.h>   // esp_rom_software_reset_system()
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

    // Take (and never release) the global SPI-flash op lock. Every flash
    // operation - legacy spi_flash and the esp_flash/NVS path alike - must
    // acquire this mutex before it programs/erases flash, so once we hold it:
    //   - any in-flight op has already completed and released the mutex, and
    //   - any new op (arbitrary NVS commit: timezone, TLS certs, Wi-Fi creds,
    //     app prefs, boot_stats, ...) blocks on the mutex before touching flash.
    // That guarantees no flash program/erase is in progress when we pull the
    // trigger below, so the hard reset cannot corrupt a half-written sector.
    spi_flash_op_lock();

    // Why not simply call esp_restart()? On this dual-core, SPIRAM-enabled P4,
    // esp_restart() -> esp_restart_noos() stalls the *other* core and then runs
    // the ROM routine Cache_WriteBack_All(CACHE_MAP_L1_DCACHE) (compiled in only
    // because CONFIG_SPIRAM=y). Intermittently (~5% of reboots under load) that
    // write-back faults with a Store access fault to an unmapped address
    // (MCAUSE 0x7, MTVAL 0x3ff100a0, PC inside Cache_WriteBack_All at ROM
    // 0x4fc10a60): a dirty L1 data-cache line carries a bogus tag and the ROM
    // dutifully tries to write it back to invalid memory and panics. Holding the
    // flash-op lock does NOT prevent it (proven on hardware) - the faulting line
    // is not associated with a flash operation.
    //
    // We do not need any dirty PSRAM data to survive a reboot, so instead of
    // asking the ROM to write the cache back, we skip that step entirely and
    // trigger a full HP-digital-domain reset directly via the ROM's
    // software_reset primitive. This resets the CPUs and peripherals (incl. DMA)
    // in hardware - no cache maintenance, no core-stall dance - and reports a
    // clean RESET_REASON_CORE_SW -> ESP_RST_SW ("SOFTWARE") reset, so boot_stats
    // does not misclassify the reboot as a crash.
    ESP_LOGW(TAG, "Flash writers quiesced - restarting now");

    // Stop task switching and mask interrupts so nothing runs (and nothing can
    // start a new flash/cache access) between here and the reset. Neither call
    // returns meaningfully on this path; the chip is about to reset.
    vTaskSuspendAll();
    portDISABLE_INTERRUPTS();

    // Full software system reset. Does not run the ROM cache write-back, so it
    // cannot hit the Cache_WriteBack_All fault above.
    esp_rom_software_reset_system();

    // Not reached - the reset takes effect immediately.
    while (true) {
    }
}
