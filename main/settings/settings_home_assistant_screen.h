/*
 * Arctic Heat Pump Controller
 * Settings - Home Assistant pairing screen
 */
#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*on_back)(void);
} home_assistant_screen_config_t;

void home_assistant_screen_create(
    const home_assistant_screen_config_t* config);
void home_assistant_screen_close(void);
bool home_assistant_screen_is_visible(void);

#ifdef __cplusplus
}
#endif
