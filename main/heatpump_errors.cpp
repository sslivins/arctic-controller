/*
 * Arctic Heat Pump Controller
 * Error Management Module - Implementation
 */
#include "heatpump_errors.h"
#include "modbus/arctic_heatpump.h"
#include "modbus/arctic_registers.h"
#include <cJSON.h>
#include <esp_log.h>
#include <string.h>
#include <time.h>

static const char* TAG = "hp_errors";

namespace arctic {

// ============================================================================
// Error Definitions
// ============================================================================

// Error register 1 (2137) definitions
static const ErrorDef s_error1_defs[] = {
    { error1::INDOOR_EE,         "E01", "INDOOR_EE",        "Indoor unit EEPROM error",           ErrorSeverity::ERROR },
    { error1::OUTDOOR_EE,        "E02", "OUTDOOR_EE",       "Outdoor unit EEPROM error",          ErrorSeverity::ERROR },
    { error1::INLET_TEMP_SENS,   "E03", "INLET_SENS",       "Inlet water temperature sensor fault", ErrorSeverity::ERROR },
    { error1::OUTLET_TEMP_SENS,  "E04", "OUTLET_SENS",      "Outlet water temperature sensor fault", ErrorSeverity::ERROR },
    { error1::INDOOR_COIL_SENS,  "E05", "INDOOR_COIL_SENS", "Indoor coil temperature sensor fault", ErrorSeverity::ERROR },
    { error1::OUTDOOR_COIL_SENS, "E06", "OUTDOOR_COIL_SENS","Outdoor coil temperature sensor fault", ErrorSeverity::ERROR },
    { error1::DISCHARGE_SENS,    "E07", "DISCHARGE_SENS",   "Discharge temperature sensor fault", ErrorSeverity::ERROR },
    { error1::SUCTION_SENS,      "E08", "SUCTION_SENS",     "Suction temperature sensor fault",   ErrorSeverity::ERROR },
    { error1::OUTDOOR_TEMP_SENS, "E09", "OUTDOOR_SENS",     "Outdoor ambient temperature sensor fault", ErrorSeverity::ERROR },
    { error1::INDOOR_OUTDOOR_COMM, "E10", "COMM_IO",        "Indoor/outdoor unit communication failure", ErrorSeverity::CRITICAL },
    { error1::WIRED_CTRL_COMM,   "E11", "COMM_CTRL",        "Wired controller communication failure", ErrorSeverity::WARNING },
    { error1::COMP_START,        "E12", "COMP_START",       "Compressor failed to start",         ErrorSeverity::CRITICAL },
    { error1::COMP_DRIVE,        "E13", "COMP_DRIVE",       "Compressor drive fault",             ErrorSeverity::CRITICAL },
    { error1::IPM_ERROR,         "E14", "IPM_ERROR",        "Inverter power module (IPM) fault",  ErrorSeverity::CRITICAL },
    { error1::COMP_TOP_PROT,     "E15", "COMP_OVERHEAT",    "Compressor overheat protection",     ErrorSeverity::CRITICAL },
    { error1::AC_VOLTAGE_PROT,   "E16", "AC_VOLTAGE",       "AC supply voltage out of range",     ErrorSeverity::ERROR },
};
static const int s_error1_count = sizeof(s_error1_defs) / sizeof(s_error1_defs[0]);

// Error register 2 (2138) definitions
static const ErrorDef s_error2_defs[] = {
    { error2::AC_CURRENT_PROT,   "E17", "AC_CURRENT",       "AC supply current protection",       ErrorSeverity::ERROR },
    { error2::COMP_CURRENT_PROT, "E18", "COMP_CURRENT",     "Compressor overcurrent protection",  ErrorSeverity::CRITICAL },
    { error2::FAN_MOTOR,         "E19", "FAN_MOTOR",        "Fan motor fault",                    ErrorSeverity::ERROR },
    { error2::BUS_VOLTAGE_PROT,  "E20", "BUS_VOLTAGE",      "DC bus voltage protection",          ErrorSeverity::CRITICAL },
    { error2::IPM_HIGH_TEMP,     "E21", "IPM_OVERHEAT",     "IPM high temperature protection",    ErrorSeverity::CRITICAL },
    { error2::HIGH_DISCHARGE_TEMP, "E22", "HIGH_DISCHARGE", "Compressor discharge temperature too high", ErrorSeverity::CRITICAL },
    { error2::HIGH_PRESSURE,     "E23", "HIGH_PRESSURE",    "High pressure protection triggered", ErrorSeverity::CRITICAL },
    { error2::LOW_PRESSURE,      "E24", "LOW_PRESSURE",     "Low pressure protection triggered",  ErrorSeverity::ERROR },
    { error2::WATER_FLOW,        "E25", "WATER_FLOW",       "Water flow sensor fault or no flow", ErrorSeverity::ERROR },
    { error2::COOLING_HIGH_COIL, "E26", "COIL_OVERHEAT",    "Outdoor coil temperature too high",  ErrorSeverity::WARNING },
    { error2::LOW_AMBIENT_TEMP,  "E27", "LOW_AMBIENT",      "Ambient temperature too low for operation", ErrorSeverity::WARNING },
    { error2::EEV_LOW_PRESS,     "E28", "EEV_LOW_PRESS",    "EEV low pressure protection",        ErrorSeverity::ERROR },
    { error2::EVI_LOW_PRESS,     "E29", "EVI_LOW_PRESS",    "EVI low pressure protection",        ErrorSeverity::ERROR },
    { error2::WATER_TEMP_DIFF,   "E30", "TEMP_DIFF",        "Inlet/outlet water temperature difference too large", ErrorSeverity::WARNING },
    { error2::LOW_OUTLET_TEMP,   "E31", "LOW_OUTLET",       "Outlet water temperature too low (freeze protection)", ErrorSeverity::WARNING },
    { error2::COMP_PRESS_DIFF,   "E32", "COMP_PRESS",       "Compressor pressure differential fault", ErrorSeverity::ERROR },
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

// Add entry to history ring buffer
static void addHistoryEntry(const char* code, bool is_clearing) {
    ErrorHistoryEntry& entry = s_history[s_history_head];
    strncpy(entry.code, code, sizeof(entry.code) - 1);
    entry.code[sizeof(entry.code) - 1] = '\0';
    
    time_t now = time(nullptr);
    
    if (is_clearing) {
        entry.occurred = 0;
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
            err.severity = s_error1_defs[i].severity;
            err.register_num = 1;
            err.mask = s_error1_defs[i].mask;
            err.first_seen = now;  // Would need tracking for accurate value
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
            err.severity = s_error2_defs[i].severity;
            err.register_num = 2;
            err.mask = s_error2_defs[i].mask;
            err.first_seen = now;
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
    // Check for newly set errors
    uint16_t new_error1 = error1 & ~s_last_error1;
    uint16_t new_error2 = error2 & ~s_last_error2;
    
    // Check for newly cleared errors
    uint16_t cleared_error1 = s_last_error1 & ~error1;
    uint16_t cleared_error2 = s_last_error2 & ~error2;
    
    // Log new errors
    for (int i = 0; i < s_error1_count; i++) {
        if (new_error1 & s_error1_defs[i].mask) {
            ESP_LOGW(TAG, "Error SET: %s - %s", s_error1_defs[i].code, s_error1_defs[i].description);
            addHistoryEntry(s_error1_defs[i].code, false);
        }
    }
    for (int i = 0; i < s_error2_count; i++) {
        if (new_error2 & s_error2_defs[i].mask) {
            ESP_LOGW(TAG, "Error SET: %s - %s", s_error2_defs[i].code, s_error2_defs[i].description);
            addHistoryEntry(s_error2_defs[i].code, false);
        }
    }
    
    // Log cleared errors
    for (int i = 0; i < s_error1_count; i++) {
        if (cleared_error1 & s_error1_defs[i].mask) {
            ESP_LOGI(TAG, "Error CLEARED: %s - %s", s_error1_defs[i].code, s_error1_defs[i].description);
            addHistoryEntry(s_error1_defs[i].code, true);
        }
    }
    for (int i = 0; i < s_error2_count; i++) {
        if (cleared_error2 & s_error2_defs[i].mask) {
            ESP_LOGI(TAG, "Error CLEARED: %s - %s", s_error2_defs[i].code, s_error2_defs[i].description);
            addHistoryEntry(s_error2_defs[i].code, true);
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

const char* severityToString(ErrorSeverity severity) {
    switch (severity) {
        case ErrorSeverity::INFO:     return "info";
        case ErrorSeverity::WARNING:  return "warning";
        case ErrorSeverity::ERROR:    return "error";
        case ErrorSeverity::CRITICAL: return "critical";
        default:                      return "unknown";
    }
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
        cJSON_AddStringToObject(err, "severity", severityToString(errors[i].severity));
        cJSON_AddNumberToObject(err, "register", errors[i].register_num);
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
