/*
 * Arctic Heat Pump Controller
 * Shared Demo State
 * 
 * Provides shared in-memory state for demo mode, used by both UI and API.
 */
#pragma once

#include <stdint.h>
#include "modbus/arctic_registers.h"

// Demo state for power, mode, and setpoints
// Shared between heatpump_screen.cpp and api_server.cpp

// Get/set demo power state
bool heatpump_demo_get_power();
void heatpump_demo_set_power(bool on);

// Get/set demo mode
arctic::WorkingMode heatpump_demo_get_mode();
void heatpump_demo_set_mode(arctic::WorkingMode mode);

// Get/set demo setpoints (in Celsius)
int16_t heatpump_demo_get_cooling_setpoint();
int16_t heatpump_demo_get_heating_setpoint();
int16_t heatpump_demo_get_hotwater_setpoint();
void heatpump_demo_set_cooling_setpoint(int16_t temp);
void heatpump_demo_set_heating_setpoint(int16_t temp);
void heatpump_demo_set_hotwater_setpoint(int16_t temp);
