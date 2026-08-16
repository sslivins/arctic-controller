/*
 * Arctic Heat Pump Controller
 * Error Management Module - Implementation
 *
 * Thin presentation adapter over the arctic-macon native fault decode. The
 * library (macon_faults.h) owns the canonical (reg, bit) -> code/label/severity
 * table; this file adds controller-only concerns: remedy text, first-seen
 * tracking, and the history ring buffer.
 */
#include "heatpump_errors.h"
#include "heatpump_controller.h"
#include "macon_faults.h"
#include "macon_registers.h"  // arctic::REG_FAULT_* address constants
#include <cJSON.h>
#include <esp_log.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char* TAG = "hp_errors";

namespace arctic {

// ============================================================================
// Severity mapping (library FaultSeverity -> controller ErrorSeverity)
// ============================================================================
static ErrorSeverity toErrorSeverity(FaultSeverity s) {
    switch (s) {
        case FaultSeverity::INFO:     return ErrorSeverity::INFO;
        case FaultSeverity::WARNING:  return ErrorSeverity::WARNING;
        case FaultSeverity::CRITICAL: return ErrorSeverity::CRITICAL;
        case FaultSeverity::FAULT:
        default:                      return ErrorSeverity::ERROR;
    }
}

// ============================================================================
// Controller-owned remedy text, keyed by fault code
// ============================================================================
// The arctic-macon library owns the code/label/severity/(reg,bit) identity but
// deliberately does NOT carry human remedy steps. Those installation-facing
// instructions live here, keyed by the LCD code. Codes absent from this table
// fall back to kDefaultResolution.
static const char* kDefaultResolution = "Contact the dealer.";

struct CodeResolution { const char* code; const char* resolution; };
static const CodeResolution kResolutions[] = {
    { "E19", "Check the inlet water temperature sensor at the heat exchanger for a short or open circuit and correct or replace." },
    { "E18", "Check the outlet water temperature sensor at the heat exchanger for a short or open circuit and correct or replace." },
    { "E13", "Check the coil temperature sensor for a short or open circuit and correct or replace." },
    { "E05", "Check the heat pump coil temperature sensor and wires for a short or open circuit and correct or replace sensors." },
    { "E01", "Check if the compressor discharge temperature sensor for short or open circuit and correct or replace." },
    { "E09", "Check if the compressor suction temperature sensor for short or open circuit and correct or replace." },
    { "E22", "Check if the ambient temperature sensor for the heat pump or its wiring has a short or open circuit and correct or replace." },
    { "E21", "Check the wired controller's cable and its connections." },
    { "r06", "This applies to 3-phase units where the phasing of the wires is incorrect and needs to be corrected." },
    { "P11", "1) Check water system is operating normal, look for reduction in normal water flow. 2) Check whether there was a refrigerant leak and repair. 3) Verify unit is in normal operation with proper exhaust temperature and system pressure." },
    { "P02", "1) Check whether the water temperature is too high or blocked. 2) Check whether the fan blades are blocked or if evaporator fins are blocked. 3) Check whether snow or ice has built up inside the unit. 4) Check that the water tank temperature setting is not too high." },
    { "P06", "1) Check whether the unit is leaking refrigerant. 2) Repair and vacuum system, then refill with exact amount of refrigerant as per nameplate." },
    { "P27", "Check that the fan is in good condition and that the evaporator fins are not in need of cleaning." },
    { "P01", "Flow is too low or wiring is open circuit. Check the water system, water pump, and operation of water flow switch and correct problem." },
    { "P15", "1) Check if water system is operating abnormally, such as water flow is too low. 2) Verify unit is in normal operation with proper exhaust temperature and system pressure." },
    { "P16", "1) Check water system is normal and water flow is adequate. 2) Verify unit is in normal operation with proper exhaust temperature and system pressure." },
    { "PC",  "Ambient temperature is out of the allowed operating range. Wait for conditions to improve." },
};

static const char* resolutionForCode(const char* code) {
    if (code) {
        for (const auto& r : kResolutions) {
            if (strcmp(r.code, code) == 0) return r.resolution;
        }
    }
    return kDefaultResolution;
}

// ============================================================================
// Fault register indexing helpers
// ============================================================================
// Map a Macon fault register address to a 0..4 index (order matches
// MACON_FAULT_REGS: 2007, 2125, 2126, 2127, 2128).
static int regIndex(uint16_t reg) {
    switch (reg) {
        case REG_FAULT_RUNSTATE:    return 0;
        case REG_FAULT_SENSOR_EE:   return 1;
        case REG_FAULT_SENSOR_COMP: return 2;
        case REG_FAULT_ELEC:        return 3;
        case REG_FAULT:             return 4;
        default:                    return -1;
    }
}

static uint8_t regByte(const HeatPumpState& s, uint16_t reg) {
    switch (reg) {
        case REG_FAULT_RUNSTATE:    return s.fault_run;
        case REG_FAULT_SENSOR_EE:   return s.fault_ee;
        case REG_FAULT_SENSOR_COMP: return s.fault_comp;
        case REG_FAULT_ELEC:        return s.fault_elec;
        case REG_FAULT:             return s.fault_ref;
        default:                    return 0;
    }
}

// ============================================================================
// Error History
// ============================================================================
static ErrorHistoryEntry s_history[ERROR_HISTORY_SIZE];
static int s_history_head = 0;  // Next write position
static int s_history_count = 0;

// Previous raw fault-register bytes and per-(reg,bit) first-seen tracking.
// Indexed by position in the library's MACON_FAULT_BITS table.
static uint8_t s_prev_regs[5] = {0};
static time_t  s_first_seen[64] = {0};  // >= MACON_FAULT_BITS_COUNT

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

// Find the MACON_FAULT_BITS index for a (reg, bit), or -1.
static int faultTableIndex(uint16_t reg, uint8_t bit) {
    for (size_t i = 0; i < MACON_FAULT_BITS_COUNT; ++i) {
        if (MACON_FAULT_BITS[i].reg == reg && MACON_FAULT_BITS[i].bit == bit) {
            return (int)i;
        }
    }
    return -1;
}

// ============================================================================
// Public Functions
// ============================================================================

int getActiveErrors(ActiveError* errors, int max_errors) {
    HeatPumpState state = getState();
    MaconFault faults[32];
    size_t n = macon_decode_faults(state.fault_run, state.fault_ee,
                                   state.fault_comp, state.fault_elec,
                                   state.fault_ref, faults, 32);
    time_t now = time(nullptr);
    int count = 0;
    for (size_t i = 0; i < n && count < max_errors; ++i) {
        ActiveError& e = errors[count++];
        e.code = faults[i].code;
        e.name = faults[i].label;
        e.description = faults[i].label;
        e.resolution = resolutionForCode(faults[i].code);
        e.severity = toErrorSeverity(faults[i].severity);
        e.reg = faults[i].reg;
        e.bit = faults[i].bit;
        int ti = faultTableIndex(faults[i].reg, faults[i].bit);
        e.first_seen = (ti >= 0 && s_first_seen[ti] > 0) ? s_first_seen[ti] : now;
        e.last_seen = now;
        e.active = true;
    }
    return count;
}

int getActiveErrorCount() {
    HeatPumpState state = getState();
    MaconFault faults[32];
    return (int)macon_decode_faults(state.fault_run, state.fault_ee,
                                    state.fault_comp, state.fault_elec,
                                    state.fault_ref, faults, 32);
}

ErrorSeverity getHighestSeverity() {
    HeatPumpState state = getState();
    MaconFault faults[32];
    size_t n = macon_decode_faults(state.fault_run, state.fault_ee,
                                   state.fault_comp, state.fault_elec,
                                   state.fault_ref, faults, 32);
    ErrorSeverity highest = ErrorSeverity::INFO;
    for (size_t i = 0; i < n; ++i) {
        ErrorSeverity sv = toErrorSeverity(faults[i].severity);
        if (sv > highest) highest = sv;
    }
    return highest;
}

bool describeFaultCode(const char* code, const char** name_out,
                       const char** description_out,
                       const char** resolution_out,
                       ErrorSeverity* severity_out) {
    const MaconFaultBit* sites[8];
    size_t n = macon_fault_bits_for_code(code, sites, 8);
    if (n == 0) return false;
    const MaconFaultBit* fb = sites[0];
    if (name_out)        *name_out = fb->label;
    if (description_out)  *description_out = fb->label;
    if (resolution_out)   *resolution_out = resolutionForCode(code);
    if (severity_out)     *severity_out = toErrorSeverity(fb->severity);
    return true;
}

void updateErrorHistory(uint8_t fault_run, uint8_t fault_ee, uint8_t fault_comp,
                        uint8_t fault_elec, uint8_t fault_ref) {
    time_t now = time(nullptr);
    const uint8_t cur[5] = { fault_run, fault_ee, fault_comp, fault_elec, fault_ref };

    for (size_t i = 0; i < MACON_FAULT_BITS_COUNT; ++i) {
        const MaconFaultBit& fb = MACON_FAULT_BITS[i];
        if (fb.severity == FaultSeverity::INFO) continue;  // skip RUN indicator
        int ri = regIndex(fb.reg);
        if (ri < 0) continue;
        bool now_set = (cur[ri] >> fb.bit) & 0x1;
        bool was_set = (s_prev_regs[ri] >> fb.bit) & 0x1;
        if (now_set && !was_set) {
            s_first_seen[i] = now;
            ESP_LOGW(TAG, "Error SET: %s - %s", fb.code, fb.label);
            addHistoryEntry(fb.code, false, 0);
        } else if (!now_set && was_set) {
            time_t occurred = s_first_seen[i];
            time_t duration = (occurred > 0 && now > occurred) ? (now - occurred) : 0;
            ESP_LOGI(TAG, "Error CLEARED: %s - %s (duration: %d sec)",
                     fb.code, fb.label, (int)duration);
            addHistoryEntry(fb.code, true, occurred);
            s_first_seen[i] = 0;
        }
    }

    for (int r = 0; r < 5; ++r) s_prev_regs[r] = cur[r];
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

    // E26 - Low ambient / comm, cleared 15 minutes ago (was active for 45 min)
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
        snprintf(buf, sizeof(buf), "%ds", (int)duration);
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

bool isErrorActive(uint16_t reg, uint8_t bit) {
    HeatPumpState state = getState();
    if (regIndex(reg) < 0) return false;
    return (regByte(state, reg) >> bit) & 0x1;
}

bool hasActiveFaultCode(uint8_t fault_run, uint8_t fault_ee, uint8_t fault_comp,
                        uint8_t fault_elec, uint8_t fault_ref, const char* code) {
    if (!code) return false;
    MaconFault faults[32];
    size_t n = macon_decode_faults(fault_run, fault_ee, fault_comp,
                                   fault_elec, fault_ref, faults, 32);
    for (size_t i = 0; i < n; ++i) {
        if (faults[i].code && strcmp(faults[i].code, code) == 0) return true;
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
