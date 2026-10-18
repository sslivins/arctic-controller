/*
 * Arctic Heat Pump Controller
 * JSON form of an event-log entry, shared by GET /api/events and the Home
 * Assistant diagnostics document.
 */
#pragma once

#include <cJSON.h>
#include "event_log.h"

// Returns a newly allocated object (caller frees), or nullptr on OOM.
cJSON* event_json_create(const event_entry_t& e);
