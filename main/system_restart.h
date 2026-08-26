#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Reboot the device safely. Quiesces application flash writers (the periodic
 * telemetry recorder and the history/event storage) and lets any in-flight NVS
 * commit settle, then calls esp_restart().
 *
 * This avoids an intermittent crash: esp_restart() -> esp_restart_noos() stalls
 * the other core and runs Cache_WriteBack_All() to flush the PSRAM data cache.
 * If a SPI-flash operation is in progress on the stalled core (cache disabled),
 * that cache write-back faults with a Store access fault inside the ROM
 * Cache_WriteBack_All_Gid(). ESP-IDF's esp_restart() does not take the flash
 * operation lock, so callers must ensure flash is idle first.
 *
 * Does not return.
 */
void system_safe_restart(void);

#ifdef __cplusplus
}
#endif
