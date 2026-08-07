/*
 * Arctic Heat Pump Controller
 * Heat Pump Temperatures Screen
 * 
 * Full-screen display of all temperature sensors with large, readable fonts.
 */
#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback when screen is closed
 */
typedef void (*heatpump_temps_close_cb_t)(void);

/**
 * @brief Build the temperatures (Status) tab content into the given panel.
 *        Used by the tab shell; the panel is persistent (never torn down on
 *        navigation).
 * @param parent The tab panel to build into.
 */
void heatpump_temps_create_in(lv_obj_t* parent);

/**
 * @brief Hide/close the temperatures screen
 */
void heatpump_temps_hide(void);

/**
 * @brief Pause/resume the update timer (tab shell drives this so only the
 *        visible tab polls). Refreshes immediately when activated.
 */
void heatpump_temps_set_active(bool active);

/**
 * @brief Check if temperatures screen is visible
 */
bool heatpump_temps_is_shown(void);

#ifdef __cplusplus
}
#endif
