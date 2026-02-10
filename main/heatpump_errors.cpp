/*
 * Arctic Heat Pump Controller
 * Error Management Module - Implementation
 */
#include "heatpump_errors.h"
#include "modbus/arctic_heatpump.h"
#include "modbus/arctic_registers.h"
#include <cJSON.h>
#include <esp_log.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char* TAG = "hp_errors";

namespace arctic {

// ============================================================================
// Error Definitions - Based on Arctic Heat Pump documentation
// ============================================================================

// Error register 1 (2137) definitions - mapped to Arctic display codes
static const ErrorDef s_error1_defs[] = {
    // Bit 0 - Indoor EE (no Arctic code documented)
    { error1::INDOOR_EE,         "E27", "INDOOR_EE",        "Indoor unit EEPROM error",
      "Contact the dealer.",
      ErrorSeverity::ERROR },
    
    // Bit 1 - Outdoor EE = E28
    { error1::OUTDOOR_EE,        "E28", "OUTDOOR_EE",       "Outdoor unit EEPROM fault",
      "Contact the dealer.",
      ErrorSeverity::ERROR },
    
    // Bit 2 - Inlet water temp sensor = E19
    { error1::INLET_TEMP_SENS,   "E19", "INLET_SENS",       "Inlet water temperature sensor fault",
      "Check the inlet water temperature sensor at the heat exchanger for a short or open circuit and correct or replace.",
      ErrorSeverity::ERROR },
    
    // Bit 3 - Outlet water temp sensor = E18
    { error1::OUTLET_TEMP_SENS,  "E18", "OUTLET_SENS",      "Outlet water temperature sensor fault",
      "Check the outlet water temperature sensor at the heat exchanger for a short or open circuit and correct or replace.",
      ErrorSeverity::ERROR },
    
    // Bit 4 - Cooling coil antifreeze = P30/E13
    { error1::INDOOR_COIL_SENS,  "E13", "COIL_SENS",        "Cooling coil temperature sensor fault",
      "Check the coil temperature sensor for a short or open circuit and correct or replace.",
      ErrorSeverity::ERROR },
    
    // Bit 5 - External coil temp sensor = E05
    { error1::OUTDOOR_COIL_SENS, "E05", "COIL_SENS",        "Heat pump coil temperature sensor fault",
      "Check the heat pump coil temperature sensor and wires for a short or open circuit and correct or replace sensors.",
      ErrorSeverity::ERROR },
    
    // Bit 6 - Discharge temp sensor = E01
    { error1::DISCHARGE_SENS,    "E01", "DISCHARGE_SENS",   "Compressor discharge temperature sensor fault",
      "Check if the compressor discharge temperature sensor for short or open circuit and correct or replace.",
      ErrorSeverity::ERROR },
    
    // Bit 7 - Suction temp sensor = E09
    { error1::SUCTION_SENS,      "E09", "SUCTION_SENS",     "Compressor suction temperature sensor fault",
      "Check if the compressor suction temperature sensor for short or open circuit and correct or replace.",
      ErrorSeverity::ERROR },
    
    // Bit 8 - Ambient temp sensor = E22
    { error1::OUTDOOR_TEMP_SENS, "E22", "AMBIENT_SENS",     "Outdoor ambient temperature sensor fault",
      "Check if the ambient temperature sensor for the heat pump or its wiring has a short or open circuit and correct or replace.",
      ErrorSeverity::ERROR },
    
    // Bit 9 - Drive/main board comm (no Arctic code)
    { error1::INDOOR_OUTDOOR_COMM, "E10", "COMM_IO",        "Communication error between drive board and main board",
      "Contact the dealer.",
      ErrorSeverity::CRITICAL },
    
    // Bit 10 - Wired controller comm = E21
    { error1::WIRED_CTRL_COMM,   "E21", "COMM_CTRL",        "Wired controller communication fault",
      "Check the wired controller's cable and its connections.",
      ErrorSeverity::WARNING },
    
    // Bit 11 - Compressor start = r02
    { error1::COMP_START,        "r02", "COMP_START",       "Compressor start fault",
      "Contact the dealer.",
      ErrorSeverity::CRITICAL },
    
    // Bit 12 - Indoor/outdoor comm (no Arctic code)
    { error1::COMP_DRIVE,        "E12", "COMM_UNIT",        "Communication error between indoor and outdoor unit",
      "Contact the dealer.",
      ErrorSeverity::CRITICAL },
    
    // Bit 13 - IPM error = r01
    { error1::IPM_ERROR,         "r01", "IPM_FAULT",        "IPM module fault",
      "Contact the dealer.",
      ErrorSeverity::CRITICAL },
    
    // Bit 14 - High outlet water temp = PA
    { error1::COMP_TOP_PROT,     "PA",  "TANK_TEMP",        "Tank temperature protection activated",
      "Contact the dealer.",
      ErrorSeverity::CRITICAL },
    
    // Bit 15 - AC voltage = r10
    { error1::AC_VOLTAGE_PROT,   "r10", "AC_VOLTAGE",       "AC voltage too high or too low protection",
      "Contact the dealer.",
      ErrorSeverity::ERROR },
};
static const int s_error1_count = sizeof(s_error1_defs) / sizeof(s_error1_defs[0]);

// Error register 2 (2138) definitions - mapped to Arctic display codes
static const ErrorDef s_error2_defs[] = {
    // Bit 0 - AC current = P19
    { error2::AC_CURRENT_PROT,   "P19", "AC_CURRENT",       "AC current protection",
      "Contact the dealer.",
      ErrorSeverity::ERROR },
    
    // Bit 1 - Compressor current = r06
    { error2::COMP_CURRENT_PROT, "r06", "COMP_PHASE",       "Compressor phase current protection",
      "This applies to 3-phase units where the phasing of the wires is incorrect and needs to be corrected.",
      ErrorSeverity::CRITICAL },
    
    // Bit 2 - DC fan motor = FA
    { error2::FAN_MOTOR,         "FA",  "FAN_MOTOR",        "DC fan motor protection",
      "Contact the dealer.",
      ErrorSeverity::ERROR },
    
    // Bit 3 - Bus voltage = r11
    { error2::BUS_VOLTAGE_PROT,  "r11", "BUS_VOLTAGE",      "DC bus voltage protection",
      "Contact the dealer.",
      ErrorSeverity::CRITICAL },
    
    // Bit 4 - IPM high temp = r05
    { error2::IPM_HIGH_TEMP,     "r05", "IPM_OVERHEAT",     "IPM module temperature too high protection",
      "Contact the dealer.",
      ErrorSeverity::CRITICAL },
    
    // Bit 5 - High discharge temp = P11
    { error2::HIGH_DISCHARGE_TEMP, "P11", "HIGH_DISCHARGE", "Compressor discharge temperature too high protection",
      "1) Check water system is operating normal, look for reduction in normal water flow. 2) Check whether there was a refrigerant leak and repair. 3) Verify unit is in normal operation with proper exhaust temperature and system pressure.",
      ErrorSeverity::CRITICAL },
    
    // Bit 6 - High pressure switch = P02
    { error2::HIGH_PRESSURE,     "P02", "HIGH_PRESSURE",    "High pressure protection activated",
      "1) Check whether the water temperature is too high or blocked. 2) Check whether the fan blades are blocked or if evaporator fins are blocked. 3) Check whether snow or ice has built up inside the unit. 4) Check that the water tank temperature setting is not too high.",
      ErrorSeverity::CRITICAL },
    
    // Bit 7 - Low pressure switch = P06
    { error2::LOW_PRESSURE,      "P06", "LOW_PRESSURE",     "Low pressure protection activated",
      "1) Check whether the unit is leaking refrigerant. 2) Repair and vacuum system, then refill with exact amount of refrigerant as per nameplate.",
      ErrorSeverity::ERROR },
    
    // Bit 8 - Water flow switch = P01
    { error2::WATER_FLOW,        "P01", "WATER_FLOW",       "Water flow switch protection",
      "Flow is too low or wiring is open circuit. Check the water system, water pump, and operation of water flow switch and correct problem.",
      ErrorSeverity::ERROR },
    
    // Bit 9 - Cooling coil overheat = P27
    { error2::COOLING_HIGH_COIL, "P27", "COIL_OVERHEAT",    "Cooling coil temperature overheating protection",
      "Check that the fan is in good condition and that the evaporator fins are not in need of cleaning.",
      ErrorSeverity::WARNING },
    
    // Bit 10 - Low ambient temp (no Arctic code)
    { error2::LOW_AMBIENT_TEMP,  "E26", "LOW_AMBIENT",      "Low ambient temperature protection",
      "Ambient temperature is too low for operation. Wait for conditions to improve.",
      ErrorSeverity::WARNING },
    
    // Bit 11 - Primary low pressure = EC
    { error2::EEV_LOW_PRESS,     "EC",  "EEV_LOW_PRESS",    "EEV circuit low pressure protection",
      "1) Check whether the unit is leaking refrigerant. 2) After leak repair and vacuum, refill with correct refrigerant amount per nameplate.",
      ErrorSeverity::ERROR },
    
    // Bit 12 - Secondary low pressure = ED
    { error2::EVI_LOW_PRESS,     "ED",  "LOW_PRESS_SENS",   "Low pressure protection (pressure sensor)",
      "Check if the ambient temperature sensor is short circuit or disconnected.",
      ErrorSeverity::ERROR },
    
    // Bit 13 - Temp difference = P15
    { error2::WATER_TEMP_DIFF,   "P15", "TEMP_DIFF",        "Inlet/outlet temperature difference too large",
      "1) Check if water system is operating abnormally, such as water flow is too low. 2) Verify unit is in normal operation with proper exhaust temperature and system pressure.",
      ErrorSeverity::WARNING },
    
    // Bit 14 - Low outlet temp = P16
    { error2::LOW_OUTLET_TEMP,   "P16", "LOW_OUTLET",       "Outlet water temperature too low protection",
      "1) Check water system is normal and water flow is adequate. 2) Verify unit is in normal operation with proper exhaust temperature and system pressure.",
      ErrorSeverity::WARNING },
    
    // Bit 15 - Compressor differential = r20/FF
    { error2::COMP_PRESS_DIFF,   "r20", "COMP_PROTECT",     "Compressor protection",
      "Contact the dealer.",
      ErrorSeverity::ERROR },
};
static const int s_error2_count = sizeof(s_error2_defs) / sizeof(s_error2_defs[0]);

// ============================================================================
// Error History
// ============================================================================

static ErrorHistoryEntry s_history[ERROR_HISTORY_SIZE];
static int s_history_head = 0;  // Next write position
static int s_history_count = 0;
static uint16_t s_last_error1 = 0;
static uint16_t s_last_error2 = 0;

// Track when each error first appeared (for duration calculation)
// Index matches the error definition arrays
static time_t s_error1_first_seen[16] = {0};
static time_t s_error2_first_seen[16] = {0};

// Add entry to history ring buffer
static void addHistoryEntry(const char* code, bool is_clearing, time_t occurred_time) {
    ErrorHistoryEntry& entry = s_history[s_history_head];
    strncpy(entry.code, code, sizeof(entry.code) - 1);
    entry.code[sizeof(entry.code) - 1] = '\0';
    
    time_t now = time(nullptr);
    
    if (is_clearing) {
        entry.occurred = occurred_time;  // When it originally occurred
        entry.cleared = now;
        entry.is_active = false;
    } else {
        entry.occurred = now;
        entry.cleared = 0;
        entry.is_active = true;
    }
    
    s_history_head = (s_history_head + 1) % ERROR_HISTORY_SIZE;
    if (s_history_count < ERROR_HISTORY_SIZE) {
        s_history_count++;
    }
}

// ============================================================================
// Public Functions
// ============================================================================

const ErrorDef* getError1Definitions(int* count) {
    if (count) *count = s_error1_count;
    return s_error1_defs;
}

const ErrorDef* getError2Definitions(int* count) {
    if (count) *count = s_error2_count;
    return s_error2_defs;
}

int getActiveErrors(ActiveError* errors, int max_errors) {
    HeatPumpState state = getState();
    int count = 0;
    time_t now = time(nullptr);
    
    // Check error register 1
    for (int i = 0; i < s_error1_count && count < max_errors; i++) {
        if (state.error1 & s_error1_defs[i].mask) {
            ActiveError& err = errors[count++];
            err.code = s_error1_defs[i].code;
            err.name = s_error1_defs[i].name;
            err.description = s_error1_defs[i].description;
            err.resolution = s_error1_defs[i].resolution;
            err.severity = s_error1_defs[i].severity;
            err.register_num = 1;
            err.mask = s_error1_defs[i].mask;
            // Use tracked first_seen time if available
            err.first_seen = s_error1_first_seen[i] > 0 ? s_error1_first_seen[i] : now;
            err.last_seen = now;
            err.active = true;
        }
    }
    
    // Check error register 2
    for (int i = 0; i < s_error2_count && count < max_errors; i++) {
        if (state.error2 & s_error2_defs[i].mask) {
            ActiveError& err = errors[count++];
            err.code = s_error2_defs[i].code;
            err.name = s_error2_defs[i].name;
            err.description = s_error2_defs[i].description;
            err.resolution = s_error2_defs[i].resolution;
            err.severity = s_error2_defs[i].severity;
            err.register_num = 2;
            err.mask = s_error2_defs[i].mask;
            // Use tracked first_seen time if available
            err.first_seen = s_error2_first_seen[i] > 0 ? s_error2_first_seen[i] : now;
            err.last_seen = now;
            err.active = true;
        }
    }
    
    return count;
}

int getActiveErrorCount() {
    HeatPumpState state = getState();
    int count = 0;
    
    for (int i = 0; i < s_error1_count; i++) {
        if (state.error1 & s_error1_defs[i].mask) count++;
    }
    for (int i = 0; i < s_error2_count; i++) {
        if (state.error2 & s_error2_defs[i].mask) count++;
    }
    
    return count;
}

ErrorSeverity getHighestSeverity() {
    HeatPumpState state = getState();
    ErrorSeverity highest = ErrorSeverity::INFO;
    
    for (int i = 0; i < s_error1_count; i++) {
        if ((state.error1 & s_error1_defs[i].mask) && 
            s_error1_defs[i].severity > highest) {
            highest = s_error1_defs[i].severity;
        }
    }
    for (int i = 0; i < s_error2_count; i++) {
        if ((state.error2 & s_error2_defs[i].mask) && 
            s_error2_defs[i].severity > highest) {
            highest = s_error2_defs[i].severity;
        }
    }
    
    return highest;
}

void updateErrorHistory(uint16_t error1, uint16_t error2) {
    time_t now = time(nullptr);
    
    // Check for newly set errors
    uint16_t new_error1 = error1 & ~s_last_error1;
    uint16_t new_error2 = error2 & ~s_last_error2;
    
    // Check for newly cleared errors
    uint16_t cleared_error1 = s_last_error1 & ~error1;
    uint16_t cleared_error2 = s_last_error2 & ~error2;
    
    // Log new errors and track first_seen time
    for (int i = 0; i < s_error1_count; i++) {
        if (new_error1 & s_error1_defs[i].mask) {
            s_error1_first_seen[i] = now;  // Track when error started
            ESP_LOGW(TAG, "Error SET: %s - %s", s_error1_defs[i].code, s_error1_defs[i].description);
            addHistoryEntry(s_error1_defs[i].code, false, 0);
        }
    }
    for (int i = 0; i < s_error2_count; i++) {
        if (new_error2 & s_error2_defs[i].mask) {
            s_error2_first_seen[i] = now;  // Track when error started
            ESP_LOGW(TAG, "Error SET: %s - %s", s_error2_defs[i].code, s_error2_defs[i].description);
            addHistoryEntry(s_error2_defs[i].code, false, 0);
        }
    }
    
    // Log cleared errors with duration
    for (int i = 0; i < s_error1_count; i++) {
        if (cleared_error1 & s_error1_defs[i].mask) {
            time_t occurred = s_error1_first_seen[i];
            time_t duration = (occurred > 0 && now > occurred) ? (now - occurred) : 0;
            ESP_LOGI(TAG, "Error CLEARED: %s - %s (duration: %lld sec)", 
                     s_error1_defs[i].code, s_error1_defs[i].description, (long long)duration);
            addHistoryEntry(s_error1_defs[i].code, true, occurred);
            s_error1_first_seen[i] = 0;  // Clear tracking
        }
    }
    for (int i = 0; i < s_error2_count; i++) {
        if (cleared_error2 & s_error2_defs[i].mask) {
            time_t occurred = s_error2_first_seen[i];
            time_t duration = (occurred > 0 && now > occurred) ? (now - occurred) : 0;
            ESP_LOGI(TAG, "Error CLEARED: %s - %s (duration: %lld sec)", 
                     s_error2_defs[i].code, s_error2_defs[i].description, (long long)duration);
            addHistoryEntry(s_error2_defs[i].code, true, occurred);
            s_error2_first_seen[i] = 0;  // Clear tracking
        }
    }
    
    s_last_error1 = error1;
    s_last_error2 = error2;
}

int getErrorHistory(ErrorHistoryEntry* history, int max_entries) {
    int count = 0;
    int idx = (s_history_head - 1 + ERROR_HISTORY_SIZE) % ERROR_HISTORY_SIZE;
    
    for (int i = 0; i < s_history_count && count < max_entries; i++) {
        history[count++] = s_history[idx];
        idx = (idx - 1 + ERROR_HISTORY_SIZE) % ERROR_HISTORY_SIZE;
    }
    
    return count;
}

void clearErrorHistory() {
    s_history_head = 0;
    s_history_count = 0;
    ESP_LOGI(TAG, "Error history cleared");
}

void populateDemoErrorHistory() {
    // Only populate once after boot
    static bool s_demo_seeded = false;
    if (s_demo_seeded) return;
    s_demo_seeded = true;
    
    time_t now = time(nullptr);
    
    // E26 - Low ambient, cleared 15 minutes ago (was active for 45 min)
    ErrorHistoryEntry& e1 = s_history[s_history_head];
    strncpy(e1.code, "E26", sizeof(e1.code) - 1);
    e1.code[sizeof(e1.code) - 1] = '\0';
    e1.occurred = now - 3600;   // Started 1h ago
    e1.cleared = now - 900;     // Cleared 15m ago
    e1.is_active = false;
    s_history_head = (s_history_head + 1) % ERROR_HISTORY_SIZE;
    s_history_count++;
    
    // E19 - Inlet sensor, cleared 18 minutes ago (was active for 12 min)
    ErrorHistoryEntry& e2 = s_history[s_history_head];
    strncpy(e2.code, "E19", sizeof(e2.code) - 1);
    e2.code[sizeof(e2.code) - 1] = '\0';
    e2.occurred = now - 1800;   // Started 30m ago
    e2.cleared = now - 1080;    // Cleared 18m ago
    e2.is_active = false;
    s_history_head = (s_history_head + 1) % ERROR_HISTORY_SIZE;
    s_history_count++;
    
    ESP_LOGI(TAG, "Demo error history populated");
}

const char* severityToString(ErrorSeverity severity) {
    switch (severity) {
        case ErrorSeverity::INFO:     return "info";
        case ErrorSeverity::WARNING:  return "warning";
        case ErrorSeverity::ERROR:    return "error";
        case ErrorSeverity::CRITICAL: return "critical";
        default:                      return "unknown";
    }
}

const char* formatDuration(time_t start_time, time_t end_time) {
    static char buf[32];
    
    // If no valid start time, return unknown
    if (start_time <= 0) {
        return "Unknown";
    }
    
    // If end_time is 0, error is still active - use current time
    time_t now = (end_time > 0) ? end_time : time(nullptr);
    
    // Handle edge case where now < start (clock sync issues)
    if (now < start_time) {
        return "Active";
    }
    
    time_t duration = now - start_time;
    
    if (duration < 60) {
        snprintf(buf, sizeof(buf), "%llds", (long long)duration);
    } else if (duration < 3600) {
        int mins = duration / 60;
        int secs = duration % 60;
        if (secs > 0) {
            snprintf(buf, sizeof(buf), "%dm %ds", mins, secs);
        } else {
            snprintf(buf, sizeof(buf), "%dm", mins);
        }
    } else if (duration < 86400) {
        int hours = duration / 3600;
        int mins = (duration % 3600) / 60;
        if (mins > 0) {
            snprintf(buf, sizeof(buf), "%dh %dm", hours, mins);
        } else {
            snprintf(buf, sizeof(buf), "%dh", hours);
        }
    } else {
        int days = duration / 86400;
        int hours = (duration % 86400) / 3600;
        if (hours > 0) {
            snprintf(buf, sizeof(buf), "%dd %dh", days, hours);
        } else {
            snprintf(buf, sizeof(buf), "%dd", days);
        }
    }
    
    return buf;
}

bool isErrorActive(uint8_t register_num, uint16_t mask) {
    HeatPumpState state = getState();
    if (register_num == 1) {
        return (state.error1 & mask) != 0;
    } else if (register_num == 2) {
        return (state.error2 & mask) != 0;
    }
    return false;
}

char* getErrorsAsJson() {
    cJSON* root = cJSON_CreateArray();
    
    ActiveError errors[32];
    int count = getActiveErrors(errors, 32);
    
    for (int i = 0; i < count; i++) {
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "code", errors[i].code);
        cJSON_AddStringToObject(err, "name", errors[i].name);
        cJSON_AddStringToObject(err, "description", errors[i].description);
        cJSON_AddStringToObject(err, "resolution", errors[i].resolution);
        cJSON_AddStringToObject(err, "severity", severityToString(errors[i].severity));
        if (errors[i].first_seen > 0) {
            cJSON_AddNumberToObject(err, "occurred", (double)errors[i].first_seen);
        } else {
            cJSON_AddNullToObject(err, "occurred");
        }
        cJSON_AddBoolToObject(err, "active", errors[i].active);
        cJSON_AddItemToArray(root, err);
    }
    
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

char* getErrorHistoryAsJson() {
    cJSON* root = cJSON_CreateArray();
    
    ErrorHistoryEntry history[ERROR_HISTORY_SIZE];
    int count = getErrorHistory(history, ERROR_HISTORY_SIZE);
    
    for (int i = 0; i < count; i++) {
        cJSON* entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "code", history[i].code);
        
        if (history[i].occurred > 0) {
            cJSON_AddNumberToObject(entry, "occurred", (double)history[i].occurred);
        } else {
            cJSON_AddNullToObject(entry, "occurred");
        }
        
        if (history[i].cleared > 0) {
            cJSON_AddNumberToObject(entry, "cleared", (double)history[i].cleared);
        } else {
            cJSON_AddNullToObject(entry, "cleared");
        }
        
        cJSON_AddBoolToObject(entry, "active", history[i].is_active);
        cJSON_AddItemToArray(root, entry);
    }
    
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

}  // namespace arctic
