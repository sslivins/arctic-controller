/*
 * Arctic Heat Pump Controller
 * NVS storage for today's energy total (see daily_energy.h).
 */
#pragma once

#include <stdint.h>

// Load the saved total. False when nothing has been saved yet.
bool daily_energy_store_load(int32_t* day_key, uint64_t* watt_seconds);

// Save the total. False on an NVS error.
bool daily_energy_store_save(int32_t day_key, uint64_t watt_seconds);
