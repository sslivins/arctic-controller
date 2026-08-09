/*
 * Arctic Heat Pump Domain Types and Demo Register Layout
 */
#pragma once

#include <stdint.h>

namespace arctic {

// ============================================================================
// Working Mode Enum
// ============================================================================

enum class WorkingMode : uint16_t {
    COOLING = 0,
    FLOOR_HEATING = 1,
    FAN_COIL_HEATING = 2,
    HEATING = 3,        // Generic heating (direction only); display fallback when
                        // the specific heating application isn't known
    // Mode 4 is unused
    HOT_WATER = 5,
    AUTO = 6
};

// ============================================================================
// Status Register 2135 Bit Definitions
// ============================================================================

namespace status1 {
    constexpr uint16_t UNIT_ON         = 0x0001;  // Bit 0: Unit ON/OFF
    constexpr uint16_t COMPRESSOR      = 0x0002;  // Bit 1: Compressor running
    constexpr uint16_t FAN_HIGH        = 0x0004;  // Bit 2: Fan high speed
    constexpr uint16_t FAN_MED         = 0x0008;  // Bit 3: Fan medium speed
    constexpr uint16_t FAN_LOW         = 0x0010;  // Bit 4: Fan low speed
    constexpr uint16_t WATER_PUMP      = 0x0020;  // Bit 5: Water pump running
    constexpr uint16_t FOUR_WAY_VALVE  = 0x0040;  // Bit 6: Four-way valve
    constexpr uint16_t BACKUP_HEATER   = 0x0080;  // Bit 7: Backup heater
    constexpr uint16_t WATER_FLOW_SW   = 0x0100;  // Bit 8: Water flow switch connected
    constexpr uint16_t HIGH_PRESS_SW   = 0x0200;  // Bit 9: High pressure switch
    constexpr uint16_t LOW_PRESS_SW    = 0x0400;  // Bit 10: Low pressure switch
    constexpr uint16_t EMERGENCY_SW    = 0x0800;  // Bit 11: Emergency switch
    constexpr uint16_t AC_ONLINE       = 0x1000;  // Bit 12: AC online switch
    constexpr uint16_t MODE_SWITCH     = 0x2000;  // Bit 13: Mode switch
    constexpr uint16_t THREE_WAY_V1    = 0x4000;  // Bit 14: Three-way valve 1
    constexpr uint16_t THREE_WAY_V2    = 0x8000;  // Bit 15: Three-way valve 2
    
    // Convenience masks
    constexpr uint16_t FAN_ANY         = FAN_HIGH | FAN_MED | FAN_LOW;
}

// ============================================================================
// Status Register 2136 Bit Definitions
// ============================================================================

namespace status2 {
    constexpr uint16_t SOLENOID_VALVE  = 0x0001;  // Bit 0
    constexpr uint16_t UNLOADING_VALVE = 0x0002;  // Bit 1
    constexpr uint16_t OIL_RETURN_VALVE = 0x0004; // Bit 2
    constexpr uint16_t DEFROSTING      = 0x0020;  // Bit 5: Defrosting in progress
    constexpr uint16_t REFRIG_RECOVERY = 0x0040;  // Bit 6: Refrigerant recovery
    constexpr uint16_t OIL_RETURN      = 0x0080;  // Bit 7: Oil return operation
    constexpr uint16_t WIRED_CTRL_CONN = 0x0100;  // Bit 8: Wired controller connected
    constexpr uint16_t ENERGY_SAVING   = 0x0200;  // Bit 9: Energy-saving mode
    constexpr uint16_t ANTIFREEZE_1    = 0x0400;  // Bit 10: 1st class antifreeze
    constexpr uint16_t ANTIFREEZE_2    = 0x0800;  // Bit 11: 2nd class antifreeze
    constexpr uint16_t STERILIZATION   = 0x1000;  // Bit 12: High temp sterilization
}

// ============================================================================
// Error Register 2137 Bit Definitions
// ============================================================================

namespace error1 {
    constexpr uint16_t INDOOR_EE       = 0x0001;  // Indoor EEPROM error
    constexpr uint16_t OUTDOOR_EE      = 0x0002;  // Outdoor EEPROM error
    constexpr uint16_t INLET_TEMP_SENS = 0x0004;  // Inlet water temp sensor
    constexpr uint16_t OUTLET_TEMP_SENS = 0x0008; // Outlet water temp sensor
    constexpr uint16_t INDOOR_COIL_SENS = 0x0010; // Indoor coil temp sensor
    constexpr uint16_t OUTDOOR_COIL_SENS = 0x0020;// Outdoor coil temp sensor
    constexpr uint16_t DISCHARGE_SENS  = 0x0040;  // Discharge temp sensor
    constexpr uint16_t SUCTION_SENS    = 0x0080;  // Suction temp sensor
    constexpr uint16_t OUTDOOR_TEMP_SENS = 0x0100;// Outdoor ambient temp sensor
    constexpr uint16_t INDOOR_OUTDOOR_COMM = 0x0200; // Indoor/outdoor unit comm
    constexpr uint16_t WIRED_CTRL_COMM = 0x0400;  // Wired controller comm
    constexpr uint16_t COMP_START      = 0x0800;  // Compressor start error
    constexpr uint16_t COMP_DRIVE      = 0x1000;  // Compressor drive error
    constexpr uint16_t IPM_ERROR       = 0x2000;  // IPM error
    constexpr uint16_t COMP_TOP_PROT   = 0x4000;  // Compressor top protection
    constexpr uint16_t AC_VOLTAGE_PROT = 0x8000;  // AC voltage protection
}

// ============================================================================
// Error Register 2138 Bit Definitions
// ============================================================================

namespace error2 {
    constexpr uint16_t AC_CURRENT_PROT = 0x0001;  // AC current protection
    constexpr uint16_t COMP_CURRENT_PROT = 0x0002;// Compressor current protection
    constexpr uint16_t FAN_MOTOR       = 0x0004;  // Fan motor error
    constexpr uint16_t BUS_VOLTAGE_PROT = 0x0008; // Bus voltage protection
    constexpr uint16_t IPM_HIGH_TEMP   = 0x0010;  // IPM high temp protection
    constexpr uint16_t HIGH_DISCHARGE_TEMP = 0x0020; // High discharge temp
    constexpr uint16_t HIGH_PRESSURE   = 0x0040;  // High pressure protection
    constexpr uint16_t LOW_PRESSURE    = 0x0080;  // Low pressure protection
    constexpr uint16_t WATER_FLOW      = 0x0100;  // Water flow protection
    constexpr uint16_t COOLING_HIGH_COIL = 0x0200;// Cooling high outdoor coil temp
    constexpr uint16_t LOW_AMBIENT_TEMP = 0x0400; // Low ambient temp protection
    constexpr uint16_t EEV_LOW_PRESS   = 0x0800;  // EEV low pressure protection
    constexpr uint16_t EVI_LOW_PRESS   = 0x1000;  // EVI low pressure protection
    constexpr uint16_t WATER_TEMP_DIFF = 0x2000;  // Large inlet/outlet temp diff
    constexpr uint16_t LOW_OUTLET_TEMP = 0x4000;  // Low outlet water temp
    constexpr uint16_t COMP_PRESS_DIFF = 0x8000;  // Compressor pressure diff
}

// ============================================================================
// Helper functions
// ============================================================================

// Convert working mode enum to string
inline const char* workingModeToString(WorkingMode mode) {
    switch (mode) {
        case WorkingMode::COOLING:         return "cooling";
        case WorkingMode::FLOOR_HEATING:   return "floor_heating";
        case WorkingMode::FAN_COIL_HEATING: return "fan_coil_heating";
        case WorkingMode::HEATING:         return "heating";
        case WorkingMode::HOT_WATER:       return "hot_water";
        case WorkingMode::AUTO:            return "auto";
        default:                           return "unknown";
    }
}

// Check if any error bits are set
inline bool hasErrors(uint16_t error1_val, uint16_t error2_val) {
    return (error1_val != 0) || (error2_val != 0);
}

}  // namespace arctic
