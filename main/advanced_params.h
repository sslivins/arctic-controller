/*
 * Arctic Heat Pump Controller
 * Advanced ("AP") technician-parameter access.
 *
 * Thin controller-side wrapper over the shared arctic-macon advanced-param
 * table (components/arctic-macon). The library owns all metadata, the confirmed
 * AP->register map, the write guardrail and the pure write-plan builder; this
 * module adds live bus IO (arctic::readRegister/writeRegister) plus demo-mode
 * and connection handling, mirroring heatpump_params.cpp for the P-parameters.
 *
 * SAFETY: only parameters whose register was confirmed by change-and-capture
 * are readable, and only the writable subset (validate_advanced_write == OK)
 * can be written. Unverified / write-locked params are refused by the library.
 */
#pragma once

#include <stdint.h>
#include "macon_advanced_params.h"  // arctic::AdvancedParam, AdvWriteResult, ...

// Read the live user-facing value of advanced parameter AP `ap`.
// Display scaling is owned by arctic-macon (for example AP28 returns minutes,
// not the vendor's raw count of 10-minute units).
// Fails (returns false) if: unknown AP, register not change-and-capture
// verified (reg == ADV_REG_UNKNOWN), not connected, or the bus read fails.
// In demo mode returns the vendor-doc default for known-register params.
bool advanced_param_read(uint8_t ap, int16_t* value_out);

// Attempt to write user-facing `value` to advanced parameter AP `ap`.
// arctic-macon validates the display increment and translates to the wire value.
// Runs the full library guardrail first (range/enum + register-known +
// write-unlocked). Returns the arctic::AdvWriteResult:
//   OK              -> value was written to the confirmed register
//   OUT_OF_RANGE / NOT_IN_ENUM / UNKNOWN_PARAM
//   REG_UNKNOWN     -> register not yet verified (refused)
//   NEEDS_SIM_CONFIRM -> register known but write-locked (manual block / safety)
// A non-OK result means NOTHING was written. If the guardrail passes but the
// bus write fails, returns REG_UNKNOWN-free failure via *bus_ok (see below).
arctic::AdvWriteResult advanced_param_write(uint8_t ap, int16_t value, bool* bus_ok = nullptr);
