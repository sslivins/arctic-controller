/*
 * Arctic Heat Pump Controller
 * Status Bar - Top bar with time and WiFi status
 */
#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback when WiFi button is clicked
 */
typedef void (*status_bar_wifi_click_cb_t)(void);

/**
 * @brief Callback when time is clicked
 */
typedef void (*status_bar_time_click_cb_t)(void);

/**
 * @brief Callback when settings is clicked
 */
typedef void (*status_bar_settings_click_cb_t)(void);

/**
 * @brief Notification types
 */
typedef enum {
    STATUS_BAR_NOTIFY_FIRMWARE_UPDATE = 0,  // Firmware update available
    STATUS_BAR_NOTIFY_WIFI_UNSTABLE,        // WiFi connection is unstable
    STATUS_BAR_NOTIFY_LOW_BATTERY,          // Low battery warning
    STATUS_BAR_NOTIFY_BROWNOUT,             // Brownout reset detected (power sag)
    STATUS_BAR_NOTIFY_MAX
} status_bar_notify_type_t;

/**
 * @brief Callback when a notification item is clicked
 * @param type The notification type that was clicked
 */
typedef void (*status_bar_notify_item_cb_t)(status_bar_notify_type_t type);

/**
 * @brief Status bar configuration
 */
typedef struct {
    lv_obj_t* parent;                           // Parent screen
    status_bar_wifi_click_cb_t on_wifi_click;   // WiFi button click callback
    status_bar_time_click_cb_t on_time_click;   // Time click callback
    status_bar_settings_click_cb_t on_settings_click;  // Settings button click callback
    status_bar_notify_item_cb_t on_notify_item_click;  // Notification item click callback
} status_bar_config_t;

/**
 * @brief Create status bar on the given parent
 * @param config Status bar configuration
 * @return Status bar container object
 */
lv_obj_t* status_bar_create(const status_bar_config_t* config);

/**
 * @brief Update WiFi connection state in status bar
 * @param connected true if connected to WiFi
 * @param ssid Connected SSID (can be NULL)
 */
void status_bar_set_wifi_state(bool connected, const char* ssid);

/**
 * @brief Set WiFi connecting animation (pulsing icon)
 * @param connecting true to start animation, false to stop
 */
void status_bar_set_wifi_connecting(bool connecting);

/**
 * @brief Add a notification
 * @param type The notification type to add
 * @param message The notification message to display
 */
void status_bar_add_notification(status_bar_notify_type_t type, const char* message);

/**
 * @brief Clear a specific notification
 * @param type The notification type to clear
 */
void status_bar_clear_notification(status_bar_notify_type_t type);

/**
 * @brief Clear all notifications
 */
void status_bar_clear_all_notifications(void);

/**
 * @brief Check if a notification exists
 * @param type The notification type to check
 * @return true if the notification exists
 */
bool status_bar_has_notification(status_bar_notify_type_t type);

/**
 * @brief Get current notification count
 * @return Number of active notifications
 */
uint8_t status_bar_get_notify_count(void);

/**
 * @brief A snapshot of one active notification (for the web API / diagnostics).
 */
typedef struct {
    status_bar_notify_type_t type;  // Notification type
    char message[64];               // Human-readable message
} status_bar_notification_snapshot_t;

/**
 * @brief Copy the currently-active notifications into caller-provided storage.
 *
 * Lets non-UI code (e.g. the web API) mirror the on-device notification bell.
 *
 * @param out Destination array (may be NULL when @p max is 0)
 * @param max Capacity of @p out
 * @return Number of active notifications written into @p out (<= max)
 */
uint8_t status_bar_get_notifications(status_bar_notification_snapshot_t* out, uint8_t max);

/**
 * @brief Update time display
 *        Call this periodically or it will auto-update via timer
 */
void status_bar_update_time(void);

/**
 * @brief Set the current weather shown next to the time.
 * @param valid        false hides the weather display (no data / no location).
 * @param temp_c       Current temperature in degrees Celsius.
 * @param weather_code WMO weather interpretation code (selects the icon).
 *
 * The temperature is cached in Celsius and rendered in the user-selected unit
 * (°C/°F per app_prefs_get_temp_unit()), so a unit change re-renders instantly
 * from the cached value without a network refetch.
 */
void status_bar_set_weather(bool valid, float temp_c, int weather_code);

/**
 * @brief Re-render the cached weather in the currently-selected temperature
 *        unit. Call after the °C/°F preference changes.
 */
void status_bar_refresh_weather(void);

/**
 * @brief Close the notification dropdown if it is open
 *
 * Safe to call when the dropdown is already closed. Used to dismiss the
 * expanded notification panel when the display turns off, so the user does
 * not wake the screen to a stale open dropdown.
 */
void status_bar_close_notifications(void);

/**
 * @brief Delete the status bar and stop its timer
 */
void status_bar_delete(void);

#ifdef __cplusplus
}
#endif
