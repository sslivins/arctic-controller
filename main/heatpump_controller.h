/*
 * Arctic Heat Pump State and Control
 * Protocol-neutral interface for monitoring and controlling the heat pump
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "heatpump_types.h"

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
    HeatPumpOperation operation = HeatPumpOperation::UNKNOWN;
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
    
    // System readings (decoded by the macon library from the real Tuya window)
    uint16_t compressor_freq = 0;    // Hz
    uint16_t fan_speed = 0;          // reg2003 A10 DC motor raw level (0..~72)
    uint16_t ac_voltage = 0;         // V
    uint16_t ac_current = 0;         // A
    uint16_t dc_voltage = 0;         // V (volts; unit conversion owned by macon lib)
    uint16_t primary_eev_opening = 0;   // steps
    uint32_t realtime_power_w = 0;   // W (real-time power; conversion owned by macon lib)

    // Estimated performance (owned by the macon library; flow is an outside
    // estimate — see estimate_performance). thermal_w is signed: + heating, - cooling.
    int32_t  thermal_w = 0;          // W water-side heat (estimated)
    uint16_t cop_x100 = 0;           // COP x100 (e.g. 392 = 3.92); 0 when !cop_valid
    bool     cop_valid = false;      // true when the estimate is meaningful
    
    // Component run-state, derived by the macon library from the native
    // MaconState (icon bits / compressor frequency / reversing-valve mode).
    // These replace the fictional Arctic status1/status2 bitfields, which the
    // Macon mainboard never actually used.
    bool compressor_running = false;
    bool pump_running = false;
    bool fan_running = false;
    bool defrosting = false;
    bool backup_heater = false;           // no confirmed Macon register -> always false
    bool reversing_valve_cooling = false; // reversing valve energized (cooling)

    // Raw Macon fault-register bytes (reg 2007, 2125, 2126, 2127, 2128) exactly
    // as the mainboard reports them. Decoded natively via arctic-macon's
    // macon_decode_faults(); NOT the old fictional error1/error2 masks.
    uint8_t fault_run = 0;   // reg2007
    uint8_t fault_ee = 0;    // reg2125
    uint8_t fault_comp = 0;  // reg2126
    uint8_t fault_elec = 0;  // reg2127
    uint8_t fault_ref = 0;   // reg2128
    bool    any_fault = false; // macon_has_fault() over the five bytes (ex-RUN)

    // Convenience status getters (now plain accessors over the derived fields).
    bool isCompressorRunning() const { return compressor_running; }
    bool isWaterPumpRunning() const { return pump_running; }
    bool isFanRunning() const { return fan_running; }
    bool isDefrosting() const { return defrosting; }
    bool isBackupHeaterOn() const { return backup_heater; }
    bool hasAnyError() const { return any_fault; }

    // DC bus voltage in volts (already converted by the macon library).
    float getDcVoltageV() const { return static_cast<float>(dc_voltage); }

    // Fan UI level (0=off..3=high) bucketed from the raw reg2003 level.
    // TODO(fan-rework): replace with bars = round(fan_speed/fan_speed_max*N)
    // once the library exposes fan_speed + fan_speed_max.
    int getFanSpeedLevel() const {
        if (!fan_running || fan_speed == 0) return 0;
        if (fan_speed >= 60) return 3;
        if (fan_speed >= 30) return 2;
        return 1;
    }
};

enum class TelemetryOperation : uint8_t {
    UNKNOWN = 0,
    HEATING = 1,
    COOLING = 2,
    HOT_WATER = 3,
};

struct TelemetrySnapshot {
    bool connected = false;
    bool inlet_valid = false;
    bool outlet_valid = false;
    bool setpoint_valid = false;
    bool compressor_valid = false;
    bool compressor_running = false;
    int16_t inlet_c = 0;
    int16_t outlet_c = 0;
    int16_t active_setpoint_c = 0;
    TelemetryOperation operation = TelemetryOperation::UNKNOWN;
};

// ============================================================================
// API Functions
// ============================================================================

// Initialize demo mode - populates state with simulated values
void initDemoState();

// Check if running in demo mode
bool isDemoMode();

// Start the demo-state synchronization task.
void startDemoSync();

// Get current state (thread-safe copy)
HeatPumpState getState();

/**
 * Return an atomic, freshness-checked snapshot suitable for persistent
 * telemetry. Invalid or stale fields remain explicitly marked invalid.
 */
TelemetrySnapshot getTelemetrySnapshot();

// Check if connected (always true in demo mode)
bool isConnected();

// ============================================================================
// Control Functions (write to heat pump)
// In demo mode, these update the synthetic state. Live writes require the
// active Macon master; passive-listen mode rejects writes.
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

// ============================================================================
// Demo State Injection (only works in demo mode)
// ============================================================================

// Set a field in the heat pump state by name. Returns true if the field was found.
// Allows testing read-only values (temps, readings, errors, status) via REST API.
bool setDemoField(const char* field, int32_t value);

// Inject (or clear) a fault by its canonical Macon code (e.g. "P02", "E19").
// Delegates the code -> (register, bit) mapping to the arctic-macon library so
// no bit positions are hardcoded here. Returns the number of register-bit sites
// written (a code such as E28/E05 maps to two sites), or 0 if the code is
// unknown. Demo mode only; re-decodes the register cache before returning.
int injectDemoFault(const char* code, bool active);

// Clear every active fault in the demo register cache (all five fault
// registers), preserving the run/state indicator. Demo mode only.
void clearDemoFaults();

// ============================================================================
// External Feed (passive Tuya listen mode)
// ============================================================================
// Populates HeatPumpState from register windows decoded off the RS485 bus by
// the passive Tuya listener. The Tab5
// never transmits in this mode. Registers are 1 byte each on the Tuya wire.

// Initialize external-feed mode (creates the state mutex, clears state).
void initExternalFeed();

// Returns true if running in external-feed (passive listen) mode.
bool isExternalFeed();

// Feed a decoded register window (reg_base..reg_base+count-1, one byte per
// register) into the state. Re-syncs HeatPumpState and marks it connected.
// Safe to call from the listener task.
void feedRegisterWindow(uint16_t reg_base, const uint8_t* regs, size_t count);

// Debug/calibration: copy the raw fed register cache into `out` (up to
// `max_count` entries). Returns the number of registers copied. `base_out`
// receives the register number of index 0 (DEMO_REG_BASE).
uint16_t getRawRegisters(uint16_t* out, uint16_t max_count, uint16_t* base_out);

// Diagnostic: a distinct (field_a, field_b) response window observed on the
// Tuya bus, with the FULL payload (including any window prefix bytes that
// feedRegisterWindow() strips). Used to hunt for register blocks the codec
// doesn't yet map (e.g. compressor frequency).
struct ObservedWindow {
    uint16_t field_a;       // wire addr field
    uint16_t field_b;       // wire count field (payload byte length)
    uint32_t hits;          // times this window has been seen
    uint32_t last_ms;       // timestamp of most recent sighting
    uint8_t  known;         // 1 if the codec maps it to a register base
    uint8_t  payload_len;   // bytes captured (<= sizeof(payload))
    uint8_t  payload[64];   // most-recent full payload (incl. prefix)
};

// Record a response window (known or unknown) into the diagnostic catalog.
// Called by the listener for every parsed response frame.
void recordObservedWindow(uint16_t field_a, uint16_t field_b, uint8_t known,
                          const uint8_t* payload, size_t len);

// Copy the observed-window catalog into `out` (up to `max_count`). Returns the
// number of distinct windows recorded.
uint16_t getObservedWindows(ObservedWindow* out, uint16_t max_count);

}  // namespace arctic
