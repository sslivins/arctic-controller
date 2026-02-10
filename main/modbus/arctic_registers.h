/*
 * Arctic Heat Pump Modbus Register Definitions
 * Based on EVI DC Inverter Heat Pump Communication Protocol V1.3
 */
#pragma once

#include <stdint.h>

namespace arctic {

// Communication settings
constexpr uint8_t SLAVE_ADDRESS = 1;
constexpr int BAUD_RATE = 2400;
// Parity: Even, 8 data bits, 1 stop bit (8E1)

// GPIO pins for RS-485 on M5Stack Tab5
constexpr int RS485_TX_PIN = 20;
constexpr int RS485_RX_PIN = 21;
constexpr int RS485_DIR_PIN = 34;  // Direction control (RTS)

// Timing constants (per protocol spec)
constexpr int RESPONSE_TIMEOUT_MS = 200;
constexpr int POLL_INTERVAL_NORMAL_MS = 500;
constexpr int POLL_INTERVAL_DISCONNECTED_MS = 5000;
constexpr int MIN_FRAME_GAP_MS = 4;  // 3.5 char times at 2400 baud ≈ 14.5ms, but 4ms is safe

// ============================================================================
// Holding Registers (Read/Write) - Addresses 2000-2099
// Function Codes: 0x03 (read), 0x06 (write single), 0x10 (write multiple)
// ============================================================================

namespace reg {

// Basic control registers (2000-2007)
constexpr uint16_t UNIT_ON_OFF         = 2000;  // 0=OFF, 1=ON
constexpr uint16_t WORKING_MODE        = 2001;  // See WorkingMode enum
constexpr uint16_t COOLING_SETPOINT    = 2002;  // Cooling temperature setting
constexpr uint16_t HEATING_SETPOINT    = 2003;  // Heating temperature setting
constexpr uint16_t HOT_WATER_SETPOINT  = 2004;  // Hot water temperature setting
constexpr uint16_t COOLING_DELTA_T     = 2005;  // Fan coil cooling ΔT
constexpr uint16_t HEATING_DELTA_T     = 2006;  // Underfloor heating ΔT
constexpr uint16_t HOT_WATER_DELTA_T   = 2007;  // Hot water tank ΔT
constexpr uint16_t FAN_COIL_HEATING_DT = 2008;  // Fan coil heating ΔT

// Technician parameters (P1-P47) - Addresses 2009-2057
constexpr uint16_t P1_EEV_INITIAL_OPENING   = 2009;  // Main EEV initial opening (0-500 steps)
constexpr uint16_t P5_STERILIZING_TIME      = 2013;  // Sterilizing time setting
constexpr uint16_t P13_MAX_TEMP_SETTING     = 2021;  // Maximum setting temperature
constexpr uint16_t P23_COOLING_AUTO_TEMP    = 2031;  // Cooling ambient temp for auto mode
constexpr uint16_t P24_HEATING_AUTO_TEMP    = 2032;  // Heating ambient temp for auto mode
constexpr uint16_t P28_MODE_SWITCH_DELAY    = 2036;  // Mode switch delay under auto mode
constexpr uint16_t P29_DEFROST_CYCLE        = 2037;  // Defrost cycle
constexpr uint16_t P30_DEFROST_ENTER_TEMP   = 2038;  // Coil temp to enter defrost
constexpr uint16_t P31_DEFROST_EXTEND_TEMP  = 2039;  // Ambient temp to extend defrost time
constexpr uint16_t P32_DEFROST_TEMP_DIFF    = 2040;  // Ambient-coil temp diff to enter defrost
constexpr uint16_t P33_DEFROST_EXTEND_TIME  = 2041;  // Extend defrost cycle time
constexpr uint16_t P34_MAX_DEFROST_TIME     = 2042;  // Maximum defrost time
constexpr uint16_t P35_DEFROST_EXIT_TEMP    = 2043;  // Coil temp to exit defrost
constexpr uint16_t P36_WATER_RETURN_TEMP    = 2044;  // Water return cycle temp
constexpr uint16_t P37_WATER_RETURN_TIME    = 2045;  // Water return cycle time
constexpr uint16_t P38_LOW_AMBIENT_PROTECT  = 2046;  // Low ambient temp protection setting
constexpr uint16_t P39_FREQ_REDUCTION       = 2047;  // Freq reduction near target temp
constexpr uint16_t P40_COOLING_LOW_AMBIENT  = 2048;  // Cooling low ambient temp protection
constexpr uint16_t P41_EEV_SUPERHEAT_MODE   = 2049;  // 0=Superheat adj, 1=Fixed-point adj
constexpr uint16_t P42_EEV_TARGET_SUPERHEAT = 2050;  // Main EEV target superheat degree
constexpr uint16_t P43_3WAY_VALVE_TIME      = 2051;  // 3-way valve 2 switching time
constexpr uint16_t P44_PUMP_TARGET_MODE     = 2052;  // 0=per P45, 1=OFF, 2=ON
constexpr uint16_t P45_PUMP_INTERVAL        = 2053;  // Water pump running interval
constexpr uint16_t P46_PUMP_LOW_AMBIENT     = 2054;  // Low ambient temp to turn on pump
constexpr uint16_t P47_WATERWAY_CLEANING    = 2055;  // 0=OFF, 1=Pump, 2=Pump+3WV1, 3=Pump+3WV1+3WV2
constexpr uint16_t FREQ_CONTROL_ENABLE      = 2056;  // Accept frequency control (0=NO, 1=YES)
constexpr uint16_t FREQ_CONTROL_SETTING     = 2057;  // Host compressor frequency (0-120)

// ============================================================================
// Input Registers (Read-Only) - Addresses 2100-2138
// Function Code: 0x03 (read)
// ============================================================================

// Temperature registers (values in tenths of degrees Celsius)
constexpr uint16_t WATER_TANK_TEMP     = 2100;
constexpr uint16_t OUTLET_WATER_TEMP   = 2102;
constexpr uint16_t INLET_WATER_TEMP    = 2103;
constexpr uint16_t DISCHARGE_TEMP      = 2104;
constexpr uint16_t SUCTION_TEMP        = 2105;
constexpr uint16_t EVI_SUCTION_TEMP    = 2106;
constexpr uint16_t OUTDOOR_COIL_TEMP   = 2107;
constexpr uint16_t INDOOR_COIL_TEMP    = 2108;
constexpr uint16_t INDOOR_AMBIENT_TEMP = 2109;
constexpr uint16_t OUTDOOR_AMBIENT_TEMP = 2110;

// Saturation temperatures
constexpr uint16_t HIGH_PRESSURE_SAT_TEMP = 2111;
constexpr uint16_t LOW_PRESSURE_SAT_TEMP  = 2112;
constexpr uint16_t EVI_LOW_PRESSURE_SAT_TEMP = 2113;

// System readings
constexpr uint16_t IPM_TEMP            = 2114;
constexpr uint16_t COMPRESSOR_FREQ     = 2118;  // Hz
constexpr uint16_t FAN_SPEED           = 2119;  // RPM
constexpr uint16_t AC_VOLTAGE          = 2120;  // V
constexpr uint16_t AC_CURRENT          = 2121;  // A (tenths)
constexpr uint16_t DC_VOLTAGE          = 2122;  // ÷10 for actual V
constexpr uint16_t DC_CURRENT          = 2123;  // A (tenths)
constexpr uint16_t PRIMARY_EEV_OPENING = 2124;  // steps
constexpr uint16_t SECONDARY_EEV_OPENING = 2125;  // steps
constexpr uint16_t HIGH_PRESSURE       = 2126;  // ÷100 for MPa
constexpr uint16_t LOW_PRESSURE        = 2127;  // ÷100 for MPa

// Status/Error bitmap registers
constexpr uint16_t STATUS_1            = 2135;  // Component status bitmap
constexpr uint16_t STATUS_2            = 2136;  // Operation status bitmap
constexpr uint16_t ERROR_1             = 2137;  // Error bitmap 1
constexpr uint16_t ERROR_2             = 2138;  // Error bitmap 2

}  // namespace reg

// ============================================================================
// Working Mode Enum
// ============================================================================

enum class WorkingMode : uint16_t {
    COOLING = 0,
    FLOOR_HEATING = 1,
    FAN_COIL_HEATING = 2,
    // Modes 3, 4 are unused
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
