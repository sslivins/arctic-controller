/*
 * Arctic Heat Pump Controller
 * Settings Screen - Firmware Panel
 */
#pragma once

#include "settings_common.h"

/**
 * @brief Create the firmware/OTA panel UI
 * @param parent Parent object (content area)
 */
void firmware_panel_create(lv_obj_t* parent);

/**
 * @brief Clean up firmware panel resources
 */
void firmware_panel_cleanup(void);

/**
 * @brief Start checking for firmware updates
 */
void firmware_check_for_updates(void);

/**
 * @brief Update the firmware UI state
 * @param new_state New update state
 */
void firmware_update_ui_state(update_ui_state_t new_state);
