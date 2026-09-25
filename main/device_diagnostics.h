/*
 * Arctic Heat Pump Controller
 * Controller (not heat-pump) health diagnostics for the Home Assistant
 * integration: boot/reset history, memory, WiFi, time sync, OTA and RS485
 * link health.
 *
 * Built on demand (GET /api/v1/diagnostics), never on the 250 ms state-push
 * path, so fast-changing values cannot churn the state revision.
 */
#pragma once

#include <cJSON.h>

// Returns a newly allocated object (caller frees), or nullptr on OOM.
cJSON* device_diagnostics_create(void);
