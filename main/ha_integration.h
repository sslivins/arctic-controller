/*
 * Home Assistant integration identity and versioned state serialization.
 */
#pragma once

#include <stdbool.h>
#include <cJSON.h>

namespace arctic::ha {

constexpr int PROTOCOL_VERSION = 1;

/**
 * Initialize stable device identity, per-boot identity, and revision tracking.
 */
bool init();

const char* deviceId();
const char* bootId();

/**
 * Create caller-owned JSON objects for the versioned integration contract.
 */
cJSON* createCapabilities();
cJSON* createStateSnapshot();

}  // namespace arctic::ha
