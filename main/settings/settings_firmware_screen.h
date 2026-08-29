/*
 * Arctic Heat Pump Controller
 * Settings - Firmware Screen (iOS-style full screen)
 * 
 * Full-screen firmware/OTA update with back navigation.
 */
#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration for Firmware screen
 */
typedef struct {
    void (*on_back)(void);  // Called when back button is pressed
} firmware_screen_config_t;

/**
 * @brief Create the Firmware settings screen
 * @param config Configuration callbacks
 */
void firmware_screen_create(const firmware_screen_config_t* config);

/**
 * @brief Close the Firmware settings screen
 */
void firmware_screen_close(void);

/**
 * @brief Check if Firmware screen is visible
 */
bool firmware_screen_is_visible(void);

/**
 * @brief Callback for async update check
 * @param update_available True if a newer version is available
 * @param new_version Version string of the new release (or empty)
 */
typedef void (*firmware_update_check_cb_t)(bool update_available, const char* new_version);

/**
 * @brief Check for firmware updates in the background
 * @param callback Called when check completes (may be called from LVGL context)
 */
void firmware_screen_check_for_updates_async(firmware_update_check_cb_t callback);

/**
 * @brief Suppress or re-enable automatic (boot + periodic) firmware update checks.
 *
 * Test-only hook (issue #164, F-07). When suppressed,
 * firmware_screen_check_for_updates_async() becomes a no-op and any check that
 * was already in flight has its completion callback dropped, so the async
 * GitHub check can't race a test-mocked notification and clear it. Not used in
 * production.
 *
 * @param suppressed True to suppress automatic checks, false to re-enable.
 */
void firmware_screen_set_auto_check_suppressed(bool suppressed);

/**
 * @brief Query whether automatic firmware update checks are suppressed.
 * @return True if suppressed (test mode), false otherwise.
 */
bool firmware_screen_auto_check_suppressed(void);

/**
 * @brief Publish or clear the firmware-update status-bar notification badge.
 *
 * Adds the STATUS_BAR_NOTIFY_FIRMWARE_UPDATE badge (with a
 * "Firmware vX available" message) when @p update_available is true, or
 * clears it otherwise. Shared by the automatic check, the manual "Check for
 * Updates" button, and the mock path so the badge logic can't drift (issue #145).
 *
 * @note The caller MUST hold the LVGL/display lock (bsp_display_lock).
 * @param update_available True if a newer version is available
 * @param new_version      Version string of the new release (may be NULL/empty)
 */
void firmware_screen_apply_update_notification(bool update_available, const char* new_version);

/**
 * @brief Inject a mock firmware check result (for testing).
 * 
 * Sets the latest version and UI state directly, bypassing the GitHub check.
 * The mock stays active until firmware_screen_clear_mock() is called.
 * Must be called from LVGL thread (within bsp_display_lock).
 * 
 * @param latest_version  Version string to show (e.g. "99.0.0")
 * @param update_available  If true, shows update-available state with Install button
 */
void firmware_screen_set_mock_result(const char* latest_version, bool update_available);

/**
 * @brief Clear mock firmware state.
 * 
 * Resets the screen to idle. The next time the screen is opened,
 * it will perform a real GitHub check.
 * Must be called from LVGL thread (within bsp_display_lock).
 */
void firmware_screen_clear_mock(void);

#ifdef __cplusplus
}
#endif
