/*
 * Arctic Heat Pump Domain Types and Demo Register Layout
 */
#pragma once

#include <stdint.h>

namespace arctic {

// ============================================================================
// Working Mode Enum
// ============================================================================

// Mirrors the unit's working-mode values (see arctic-macon MaconWorkingMode for the
// bench mapping). Only COOLING, HEATING, HOT_WATER and HOT_WATER_COOLING can be
// selected; MODE_2..MODE_4 are heating variants that are shown but never
// written, and UNKNOWN covers anything the library can't decode.
enum class WorkingMode : uint16_t {
    COOLING = 0,
    HEATING = 1,
    MODE_2 = 2,
    MODE_3 = 3,
    MODE_4 = 4,
    HOT_WATER = 5,
    HOT_WATER_COOLING = 6,
    UNKNOWN = 0xFF
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
        case WorkingMode::COOLING:           return "cooling";
        case WorkingMode::HEATING:           return "heating";
        case WorkingMode::MODE_2:            return "mode_2";
        case WorkingMode::MODE_3:            return "mode_3";
        case WorkingMode::MODE_4:            return "mode_4";
        case WorkingMode::HOT_WATER:         return "hot_water";
        case WorkingMode::HOT_WATER_COOLING: return "hot_water_cooling";
        default:                             return "unknown";
    }
}

inline bool isSelectableWorkingMode(WorkingMode mode) {
    return mode == WorkingMode::COOLING || mode == WorkingMode::HEATING ||
           mode == WorkingMode::HOT_WATER ||
           mode == WorkingMode::HOT_WATER_COOLING;
}

// Parse a mode key for a write. Accepts the current keys plus the pre-2026-09
// names ("floor_heating", "auto") so existing automations keep working. Only
// selectable modes parse; everything else returns false.
inline bool parseSelectableWorkingMode(const char* key, WorkingMode* out) {
    if (key == nullptr || out == nullptr) return false;
    struct Entry { const char* key; WorkingMode mode; };
    static const Entry kEntries[] = {
        {"cooling",           WorkingMode::COOLING},
        {"heating",           WorkingMode::HEATING},
        {"hot_water",         WorkingMode::HOT_WATER},
        {"hot_water_cooling", WorkingMode::HOT_WATER_COOLING},
        {"floor_heating",     WorkingMode::HEATING},
        {"auto",              WorkingMode::HOT_WATER_COOLING},
    };
    for (const Entry& e : kEntries) {
        const char* a = e.key;
        const char* b = key;
        while (*a != '\0' && *a == *b) { ++a; ++b; }
        if (*a == '\0' && *b == '\0') { *out = e.mode; return true; }
    }
    return false;
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
