/*
 * Arctic Heat Pump Controller
 * Today's energy total as saved by firmware older than the energy ledger
 * (keys "day"/"ws" in the "energy" namespace). Read once so the ledger can
 * carry the day on after the upgrade; nothing writes it any more.
 */
#pragma once

#include <stdint.h>

// Load the saved total. False when nothing was saved.
// day_key is (year * 1000 + tm_yday) of the local day it belongs to.
bool daily_energy_store_load(int32_t* day_key, uint64_t* watt_seconds);
