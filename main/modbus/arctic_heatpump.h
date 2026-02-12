/*
 * Arctic Heat Pump State and Control
 * High-level interface for monitoring and controlling the heat pump
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "arctic_registers.h"

namespace arctic {

// ============================================================================
// Heat Pump State Structure
// ============================================================================

struct HeatPumpState {
    // Connection status
    bool connected = false;
    uint32_t last_successful_read_ms = 0;
    uint32_t last_attempt_ms = 0;
    uint8_t consecutive_failures = 0;
    
    // Settings (from holding registers 2000-2007)
    bool unit_on = false;
    WorkingMode working_mode = WorkingMode::COOLING;
    int16_t cooling_setpoint = 0;    // °C (raw value, may need /10)
    int16_t heating_setpoint = 0;    // °C
    int16_t hot_water_setpoint = 0;  // °C
    
    // Temperatures (from input registers 2100-2117)
    int16_t water_tank_temp = 0;
    int16_t outlet_water_temp = 0;
    int16_t inlet_water_temp = 0;
    int16_t discharge_temp = 0;
    int16_t suction_temp = 0;
    int16_t outdoor_coil_temp = 0;
    int16_t indoor_coil_temp = 0;
    int16_t outdoor_ambient_temp = 0;
    int16_t ipm_temp = 0;
    
    // System readings (from input registers 2118-2127)
    uint16_t compressor_freq = 0;    // Hz
    uint16_t fan_speed = 0;          // RPM
    uint16_t ac_voltage = 0;         // V
    uint16_t ac_current = 0;         // A (raw, may need conversion)
    uint16_t dc_voltage = 0;         // V (raw ÷ 10 for actual)
    uint16_t dc_current = 0;         // A (raw)
    uint16_t primary_eev_opening = 0;   // steps
    uint16_t secondary_eev_opening = 0; // steps
    uint16_t high_pressure = 0;      // MPa (raw ÷ 100)
    uint16_t low_pressure = 0;       // MPa (raw ÷ 100)
    
    // Status bitmaps
    uint16_t status1 = 0;  // Register 2135
    uint16_t status2 = 0;  // Register 2136
    uint16_t error1 = 0;   // Register 2137
    uint16_t error2 = 0;   // Register 2138
    
    // Convenience status getters
    bool isCompressorRunning() const { return (status1 & status1::COMPRESSOR) != 0; }
    bool isWaterPumpRunning() const { return (status1 & status1::WATER_PUMP) != 0; }
    bool isFanRunning() const { return (status1 & status1::FAN_ANY) != 0; }
    bool isDefrosting() const { return (status2 & status2::DEFROSTING) != 0; }
    bool isBackupHeaterOn() const { return (status1 & status1::BACKUP_HEATER) != 0; }
    bool hasAnyError() const { return (error1 != 0) || (error2 != 0); }
    
    // Get actual DC voltage (register value ÷ 10)
    float getDcVoltageV() const { return dc_voltage / 10.0f; }
    
    // Get actual pressures (register value ÷ 100)
    float getHighPressureMPa() const { return high_pressure / 100.0f; }
    float getLowPressureMPa() const { return low_pressure / 100.0f; }
    
    // Get fan speed level (0=off, 1=low, 2=med, 3=high)
    int getFanSpeedLevel() const {
        if (status1 & status1::FAN_HIGH) return 3;
        if (status1 & status1::FAN_MED) return 2;
        if (status1 & status1::FAN_LOW) return 1;
        return 0;
    }
};

// ============================================================================
// API Functions
// ============================================================================

// Initialize the heat pump communication
// Must call modbus::init() first (unless demo mode)
void init();

// Initialize demo mode - populates state with simulated values
// No Modbus connection needed. Call instead of init() + startPolling().
void initDemoState();

// Check if running in demo mode
bool isDemoMode();

// Start/stop the polling task
void startPolling();
void stopPolling();

// Get current state (thread-safe copy)
HeatPumpState getState();

// Check if connected (always true in demo mode)
bool isConnected();

// ============================================================================
// Control Functions (write to heat pump)
// In demo mode, these update s_state directly instead of Modbus writes.
// ============================================================================

// Turn unit on/off
bool setUnitPower(bool on);

// Set working mode
bool setWorkingMode(WorkingMode mode);

// Set temperature setpoints
bool setCoolingSetpoint(int16_t temp);
bool setHeatingSetpoint(int16_t temp);
bool setHotWaterSetpoint(int16_t temp);

// Generic register write (for technician parameters)
// Returns true on success
bool writeRegister(uint16_t address, uint16_t value);

// Read a single register (for technician parameters)
// Returns true on success, value is stored in *value_out
bool readRegister(uint16_t address, uint16_t* value_out);

// ============================================================================
// Debug/Diagnostic Functions
// ============================================================================

// Get error description strings for current errors
// Returns number of errors found, fills buffer with descriptions
int getErrorDescriptions(char* buffer, size_t buffer_size);

// Get status description string
void getStatusDescription(char* buffer, size_t buffer_size);

// Force an immediate poll (for testing)
void forcePoll();

// ============================================================================
// Demo State Injection (only works in demo mode)
// ============================================================================

// Set a field in the heat pump state by name. Returns true if the field was found.
// Allows testing read-only values (temps, readings, errors, status) via REST API.
bool setDemoField(const char* field, int32_t value);

}  // namespace arctic
