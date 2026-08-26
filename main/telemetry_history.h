#pragma once

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t telemetry_history_init(void);
void telemetry_history_prepare_factory_reset(void);

/**
 * Stop the periodic telemetry recorder task and wait for it to exit. After this
 * returns the recorder will not start any further esp_partition flash writes.
 * Used before a reboot to quiesce background flash activity.
 */
void telemetry_history_stop(void);

#ifdef __cplusplus
}
#endif
