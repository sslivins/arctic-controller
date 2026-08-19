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

/**
 * @brief Number of consecutive crash-induced reboots (panic / task WDT /
 *        interrupt WDT) recorded across boots (NVS). Reset to zero once the
 *        device proves healthy via boot_stats_note_healthy(). A brief power
 *        cycle or a clean software reset does not clear it — only a healthy
 *        boot does — so a genuine crash loop is distinguishable from a one-off.
 */
uint32_t boot_stats_panic_streak(void);

/**
 * @brief True if this boot entered SAFE MODE because the consecutive
 *        crash-reboot streak reached the safe-mode threshold. In safe mode the
 *        firmware disables optional/risky subsystems (e.g. demo mode) so a
 *        crashing optional path cannot hold the device in an unrecoverable
 *        boot loop. The value is fixed for the duration of the current boot.
 */
bool boot_stats_in_safe_mode(void);

/**
 * @brief Clear the consecutive crash-reboot streak in NVS. Call once the device
 *        has proven healthy (up for a stability window) so a later isolated
 *        crash does not immediately re-enter safe mode. Does not change the
 *        current boot's safe-mode state.
 * @return true if the streak is now cleared (or was already zero); false if the
 *         NVS persist failed and the caller should retry later.
 */
bool boot_stats_note_healthy(void);

#ifdef __cplusplus
}
#endif
