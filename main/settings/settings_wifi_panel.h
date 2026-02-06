/*
 * Arctic Heat Pump Controller
 * Settings Screen - WiFi Panel
 */
#pragma once

#include "settings_common.h"

/**
 * @brief Create the WiFi panel UI
 * @param parent Parent object (content area)
 */
void wifi_panel_create(lv_obj_t* parent);

/**
 * @brief Clean up WiFi panel resources
 */
void wifi_panel_cleanup(void);

/**
 * @brief Trigger a WiFi network scan
 */
void wifi_trigger_scan(void);

/**
 * @brief Update the connected section display
 */
void wifi_update_connected_section(void);

/**
 * @brief Show the password dialog for a network
 * @param ssid Network SSID
 * @param is_open True if network is open (no password)
 */
void wifi_show_password_dialog(const char* ssid, bool is_open);

/**
 * @brief Hide the password dialog
 */
void wifi_hide_password_dialog(void);

/**
 * @brief Start the periodic scan timer
 */
void wifi_start_scan_timer(void);

/**
 * @brief Stop the periodic scan timer
 */
void wifi_stop_scan_timer(void);
