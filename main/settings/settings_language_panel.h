/*
 * Arctic Heat Pump Controller
 * Settings Screen - Language Panel
 */
#pragma once

#include "settings_common.h"

/**
 * @brief Create the language panel UI
 * @param parent Parent object (content area)
 */
void language_panel_create(lv_obj_t* parent);

/**
 * @brief Clean up language panel resources
 */
void language_panel_cleanup(void);

/**
 * @brief Refresh the language panel UI (after language change)
 */
void language_panel_refresh(void);
