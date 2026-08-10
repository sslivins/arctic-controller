#pragma once

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t telemetry_history_init(void);
void telemetry_history_prepare_factory_reset(void);

#ifdef __cplusplus
}
#endif
