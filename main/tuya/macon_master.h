/*
 * Active Tuya master runtime for the Arctic/Macon heat pump.
 *
 * In this mode the Tab5 is the SOLE bus master: it periodically polls the two
 * telemetry register windows over fc=0x03 reads (feeding the decoded values
 * into the same HeatPumpState the passive listener populates) and issues
 * fc=0x06 verified writes on demand via the shared-library MaconLink layer.
 *
 * SAFETY: two masters on one RS485 bus collide, so active mode is only safe
 * when the OEM controller is physically disconnected. start() enforces this by
 * refusing to activate if it observes any valid bus traffic during a preflight
 * listen window; on a quiet bus it becomes master, otherwise it stays inactive
 * (no bus TX) and reports the condition.
 *
 * All bus access (polls and writes) is serialised through a single
 * mutex so the half-duplex UART is only ever driven by one transaction at a
 * time. The mutex is held per transaction, not per poll cycle, so a UI/REST
 * setpoint write waits at most one in-flight transaction.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

namespace macon_master {

// Configure the RS485 transport and MaconLink. Does NOT start polling and does
// NOT transmit. Idempotent. Returns ESP_OK on success.
esp_err_t init();

// Run the bus-idle preflight and, only if the bus is quiet, start the polling
// task and mark the master ACTIVE. Returns ESP_OK when activated; returns
// ESP_ERR_INVALID_STATE (and leaves the master inactive, never transmitting)
// if init() has not run or the bus is not idle (OEM controller still present).
esp_err_t start();

// True once start() has confirmed an idle bus and begun active mastering.
bool is_active();

// Setpoint commands. Return false (no-op) unless the master is active. The
// legacy arctic::setCoolingSetpoint / setHotWaterSetpoint route here when
// active so the existing UI/REST callers work unchanged.
bool set_cooling_setpoint(int celsius);
bool set_hot_water_setpoint(int celsius);

// Write a one-byte value to a register covered by a known Macon wire window.
bool write_register(uint16_t address, uint8_t value);

}  // namespace macon_master
