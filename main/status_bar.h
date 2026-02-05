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
 * @brief Status bar configuration
 */
typedef struct {
    lv_obj_t* parent;                       // Parent screen
    status_bar_wifi_click_cb_t on_wifi_click;  // WiFi button click callback
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
 * @brief Update time display
 *        Call this periodically or it will auto-update via timer
 */
void status_bar_update_time(void);

/**
 * @brief Delete the status bar and stop its timer
 */
void status_bar_delete(void);

#ifdef __cplusplus
}
#endif
