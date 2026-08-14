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

enum class HeatPumpOperation : uint8_t {
    UNKNOWN = 0,
    OFF,
    IDLE,
    HEATING,
    COOLING,
    DEFROST,
    FAULT
};

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

inline const char* heatPumpOperationToString(HeatPumpOperation operation) {
    switch (operation) {
        case HeatPumpOperation::OFF:      return "off";
        case HeatPumpOperation::IDLE:     return "idle";
        case HeatPumpOperation::HEATING:  return "heating";
        case HeatPumpOperation::COOLING:  return "cooling";
        case HeatPumpOperation::DEFROST:  return "defrost";
        case HeatPumpOperation::FAULT:    return "fault";
        default:                          return "unknown";
    }
}

}  // namespace arctic
