/*
 * Arctic Heat Pump Controller
 * Boot / reset statistics
 *
 * Persists across reboots (NVS) the number of brownout resets, so power-supply
 * sags survive the reboot they cause and can be inspected after the fact. The
 * RAM event log is wiped on every boot, so it alone cannot tell you how often
 * the board has browned out over a test session — this module can.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <esp_system.h>   // esp_reset_reason_t

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize persistent boot/reset statistics. Call once at startup,
 *        AFTER NVS has been initialized. Pass the value from
 *        esp_reset_reason(). If the reason is a brownout, the persistent
 *        brownout counter (in NVS) is incremented and committed.
 */
void boot_stats_init(esp_reset_reason_t reason);

/**
 * @brief Total number of brownout resets recorded across all boots (NVS).
 */
uint32_t boot_stats_brownout_count(void);

/**
 * @brief The reset reason for the current boot (as passed to boot_stats_init).
 */
esp_reset_reason_t boot_stats_last_reset_reason(void);

/**
 * @brief Short human-readable name for a reset reason ("BROWNOUT", ...).
 */
const char* boot_stats_reset_reason_name(esp_reset_reason_t reason);

/**
 * @brief Reset the persistent brownout counter to zero (e.g. before a test).
 */
void boot_stats_clear(void);

#ifdef __cplusplus
}
#endif
