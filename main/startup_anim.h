/*
 * Arctic Heat Pump Controller - Startup Animation
 * A cool Arctic-themed boot animation using smooth_ui_toolkit
 */
#pragma once

#ifdef __cplusplus

/**
 * @brief Initialize and run the startup animation
 * @param on_complete Callback function when animation completes (can be NULL)
 * @return true if animation started successfully
 */
bool startup_anim_init(void (*on_complete)(void));

/**
 * @brief Update the startup animation (call from main loop)
 * @return true if animation is still running
 */
bool startup_anim_update(void);

/**
 * @brief Check if startup animation is still running
 * @return true if animation is in progress
 */
bool startup_anim_is_running(void);

/**
 * @brief Force stop the startup animation (cleanup)
 */
void startup_anim_stop(void);

#endif
