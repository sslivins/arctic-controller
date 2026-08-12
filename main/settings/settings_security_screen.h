/*
 * Arctic Heat Pump Controller
 * Settings - Security screen
 *
 * Home-Assistant-independent home for the physical setup code used to replace
 * the factory administrator credentials from the web interface.
 */
#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*on_back)(void);
} security_screen_config_t;

void security_screen_create(const security_screen_config_t* config);
void security_screen_close(void);
bool security_screen_is_visible(void);

#ifdef __cplusplus
}
#endif
