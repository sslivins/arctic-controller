#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file ota_commit.h
 * @brief Commit criteria for a newly OTA'd image (#252 T-05 / F-06).
 *
 * With rollback enabled an OTA image boots in PENDING_VERIFY and must be
 * explicitly committed, or the bootloader reverts to the other slot on the
 * next reboot. Committing is therefore an assertion that the new firmware
 * actually works — and the bar for that assertion is the whole safety
 * property, because a fielded controller has no serial console.
 *
 * Historically the bar was "the LVGL UI was created". That catches an image
 * which crashes early (proven on hardware by the rollback-validation job) but
 * not one which boots, renders, commits itself and is then permanently
 * unreachable — a WiFi/SDIO regression, say. That image is a truck roll.
 *
 * So a commissioned device must also have been reachable at least once on
 * this boot before it is allowed to commit.
 *
 * There is deliberately NO deadline and no forced reboot. Reachability latches
 * for the whole uptime, so a device whose router is down for hours still
 * commits the moment the network returns. An image that never connects simply
 * stays pending, and the next reboot — power cut, watchdog, user — rolls it
 * back. That keeps the mechanism free of a timer racing a slow-recovering
 * network, which is the main way this kind of policy rolls back good firmware.
 */

/** Read the commissioned latch. Call once, after nvs_flash_init(). */
void ota_commit_init(void);

/** Latch that the UI came up. Safe to call repeatedly. */
void ota_commit_note_ui_ready(void);

/**
 * Evaluate the criteria and commit if they are met.
 *
 * Cheap and idempotent; call from the main loop. Must NOT be called only once
 * at UI creation: reachability normally arrives seconds later, so a one-shot
 * check would find the device unreachable, never re-evaluate, and leave every
 * unit uncommitted.
 */
void ota_commit_eval(void);

/**
 * Why the image has not been committed yet, for telemetry and tests.
 *
 * One of: "committed", "not_pending" (nothing to commit), "waiting_for_ui",
 * "waiting_for_network", or "waiting_for_network_grace".
 */
const char* ota_commit_status(void);

/** True once this device has ever reached the network, persisted across boots. */
bool ota_commit_is_commissioned(void);

#ifdef __cplusplus
}
#endif
