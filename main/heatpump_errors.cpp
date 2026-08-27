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
#include <cJSON.h>
#include <esp_log.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char* TAG = "hp_errors";

namespace arctic {

// ============================================================================
// Concurrency guard
// ============================================================================
// The error-history state below (s_history / s_first_seen / s_prev_regs) is
// process-global and is written by the poll/feed path and the /api/test/*
// handlers while it is read concurrently by the UI task and the web/API task.
// Without synchronization an interleaved reader/writer can observe a torn
// ring-buffer index or first-seen table and dereference a bad pointer — the
// store-access-fault panic (MCAUSE 0x7) seen in device-tests. A single mutex
// serializes every public read and write entry point.
//
// The mutex is created on first use. C++11 guarantees thread-safe
// initialization of the function-local static, so no explicit init call is
// required. If creation ever fails (heap exhaustion at first touch), the guard
// degrades to a no-op rather than crashing.
static SemaphoreHandle_t errorStateMutex() {
    static SemaphoreHandle_t s_mutex = xSemaphoreCreateMutex();
    return s_mutex;
}

namespace {
struct ErrorStateLock {
    SemaphoreHandle_t m;
    ErrorStateLock() : m(errorStateMutex()) {
        if (m) xSemaphoreTake(m, portMAX_DELAY);
    }
    ~ErrorStateLock() {
        if (m) xSemaphoreGive(m);
    }
    ErrorStateLock(const ErrorStateLock&) = delete;
    ErrorStateLock& operator=(const ErrorStateLock&) = delete;
};
}  // namespace

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
// Controller-owned remedy text
// ============================================================================
// Remedy ("resolution") text lives in the arctic-macon library, keyed by the
// semantic MaconFaultId so multi-site codes cannot drift; the controller only
// looks it up via macon_fault_resolution(id) and carries no code strings of
// its own.

// ============================================================================
// Error History
// ============================================================================
static ErrorHistoryEntry s_history[ERROR_HISTORY_SIZE];
static int s_history_head = 0;  // Next write position
static int s_history_count = 0;

// Previous raw fault-register bytes (for edge detection) and first-seen
// timestamps keyed by OPAQUE fault site id. The site store is a small linear
// table (there are ~31 distinct sites); the controller never derives a register
// or bit from a site — it only compares and stores the token.
static uint8_t s_prev_regs[5] = {0};

struct FirstSeen { MaconFaultSiteId site; time_t when; };
static FirstSeen s_first_seen[64];
static size_t    s_first_seen_count = 0;

static time_t getFirstSeen(MaconFaultSiteId site) {
    for (size_t i = 0; i < s_first_seen_count; ++i) {
        if (s_first_seen[i].site == site) return s_first_seen[i].when;
    }
    return 0;
}

static void setFirstSeen(MaconFaultSiteId site, time_t when) {
    for (size_t i = 0; i < s_first_seen_count; ++i) {
        if (s_first_seen[i].site == site) { s_first_seen[i].when = when; return; }
    }
    if (s_first_seen_count < (sizeof(s_first_seen) / sizeof(s_first_seen[0]))) {
        s_first_seen[s_first_seen_count++] = { site, when };
    }
}

static void clearFirstSeen(MaconFaultSiteId site) {
    for (size_t i = 0; i < s_first_seen_count; ++i) {
        if (s_first_seen[i].site == site) { s_first_seen[i].when = 0; return; }
    }
}

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

int getActiveErrors(ActiveError* errors, int max_errors) {
    HeatPumpState state = getState();
    MaconFault faults[MAX_ACTIVE_FAULTS];
    size_t n = macon_decode_faults(state.fault_run, state.fault_ee,
                                   state.fault_comp, state.fault_elec,
                                   state.fault_ref, faults, MAX_ACTIVE_FAULTS);
    time_t now = time(nullptr);
    int count = 0;
    ErrorStateLock lock;
    for (size_t i = 0; i < n && count < max_errors; ++i) {
        ActiveError& e = errors[count++];
        e.code = faults[i].code;
        e.name = faults[i].label;
        e.description = faults[i].label;
        e.resolution = macon_fault_resolution(faults[i].id);
        e.severity = toErrorSeverity(faults[i].severity);
        e.site = faults[i].site;
        time_t fs = getFirstSeen(faults[i].site);
        e.first_seen = (fs > 0) ? fs : now;
        e.last_seen = now;
        e.active = true;
    }
    return count;
}

int getActiveErrorCount() {
    HeatPumpState state = getState();
    MaconFault faults[MAX_ACTIVE_FAULTS];
    return (int)macon_decode_faults(state.fault_run, state.fault_ee,
                                    state.fault_comp, state.fault_elec,
                                    state.fault_ref, faults, MAX_ACTIVE_FAULTS);
}

ErrorSeverity getHighestSeverity() {
    HeatPumpState state = getState();
    MaconFault faults[MAX_ACTIVE_FAULTS];
    size_t n = macon_decode_faults(state.fault_run, state.fault_ee,
                                   state.fault_comp, state.fault_elec,
                                   state.fault_ref, faults, MAX_ACTIVE_FAULTS);
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
    if (resolution_out)   *resolution_out = macon_fault_resolution(fb->id);
    if (severity_out)     *severity_out = toErrorSeverity(fb->severity);
    return true;
}

void updateErrorHistory(uint8_t fault_run, uint8_t fault_ee, uint8_t fault_comp,
                        uint8_t fault_elec, uint8_t fault_ref) {
    time_t now = time(nullptr);
    ErrorStateLock lock;

    // Decode previous and current fault bytes into opaque fault sites, then diff
    // by site id. The controller never inspects register/bit here — the library
    // owns the bit layout; we only compare and store site tokens.
    // Keep these large (MAX_ACTIVE_FAULTS-entry, ~1.5 KB each) decode buffers
    // OFF the stack. updateErrorHistory runs on the demo-sync task's modest
    // 4 KB stack, and a full fault set plus the per-fault ESP_LOG/addHistoryEntry
    // calls below would otherwise overflow it (observed: "stack overflow in task
    // arctic_demo_sync"). Safe as static because ErrorStateLock serializes every
    // caller for the whole function body, so only one execution is ever here.
    static MaconFault prev_f[MAX_ACTIVE_FAULTS];
    static MaconFault cur_f[MAX_ACTIVE_FAULTS];
    size_t np = macon_decode_faults(s_prev_regs[0], s_prev_regs[1], s_prev_regs[2],
                                    s_prev_regs[3], s_prev_regs[4], prev_f, MAX_ACTIVE_FAULTS);
    size_t nc = macon_decode_faults(fault_run, fault_ee, fault_comp,
                                    fault_elec, fault_ref, cur_f, MAX_ACTIVE_FAULTS);

    // Newly appeared: present now, absent before.
    for (size_t i = 0; i < nc; ++i) {
        bool was = false;
        for (size_t j = 0; j < np; ++j) {
            if (prev_f[j].site == cur_f[i].site) { was = true; break; }
        }
        if (!was) {
            setFirstSeen(cur_f[i].site, now);
            ESP_LOGW(TAG, "Error SET: %s - %s", cur_f[i].code, cur_f[i].label);
            addHistoryEntry(cur_f[i].code, false, 0);
        }
    }

    // Cleared: present before, absent now.
    for (size_t j = 0; j < np; ++j) {
        bool still = false;
        for (size_t i = 0; i < nc; ++i) {
            if (cur_f[i].site == prev_f[j].site) { still = true; break; }
        }
        if (!still) {
            time_t occurred = getFirstSeen(prev_f[j].site);
            time_t duration = (occurred > 0 && now > occurred) ? (now - occurred) : 0;
            ESP_LOGI(TAG, "Error CLEARED: %s - %s (duration: %d sec)",
                     prev_f[j].code, prev_f[j].label, (int)duration);
            addHistoryEntry(prev_f[j].code, true, occurred);
            clearFirstSeen(prev_f[j].site);
        }
    }

    s_prev_regs[0] = fault_run;
    s_prev_regs[1] = fault_ee;
    s_prev_regs[2] = fault_comp;
    s_prev_regs[3] = fault_elec;
    s_prev_regs[4] = fault_ref;
}

int getErrorHistory(ErrorHistoryEntry* history, int max_entries) {
    int count = 0;
    ErrorStateLock lock;
    int idx = (s_history_head - 1 + ERROR_HISTORY_SIZE) % ERROR_HISTORY_SIZE;

    for (int i = 0; i < s_history_count && count < max_entries; i++) {
        history[count++] = s_history[idx];
        idx = (idx - 1 + ERROR_HISTORY_SIZE) % ERROR_HISTORY_SIZE;
    }

    return count;
}

void clearErrorHistory() {
    ErrorStateLock lock;
    s_history_head = 0;
    s_history_count = 0;
    ESP_LOGI(TAG, "Error history cleared");
}

void populateDemoErrorHistory() {
    // Only populate once after boot
    static bool s_demo_seeded = false;
    ErrorStateLock lock;
    if (s_demo_seeded) return;
    s_demo_seeded = true;

    time_t now = time(nullptr);

    // Codes come from the library by semantic fault id — the controller bakes in
    // no OEM code strings of its own.
    const char* comm_code = macon_code_for_fault_id(MaconFaultId::IndoorOutdoorCommunication);
    const char* inlet_code = macon_code_for_fault_id(MaconFaultId::InletWaterSensor);

    // Comm fault, cleared 15 minutes ago (was active for 45 min)
    ErrorHistoryEntry& e1 = s_history[s_history_head];
    strncpy(e1.code, comm_code ? comm_code : "", sizeof(e1.code) - 1);
    e1.code[sizeof(e1.code) - 1] = '\0';
    e1.occurred = now - 3600;   // Started 1h ago
    e1.cleared = now - 900;     // Cleared 15m ago
    e1.is_active = false;
    s_history_head = (s_history_head + 1) % ERROR_HISTORY_SIZE;
    s_history_count++;

    // Inlet sensor, cleared 18 minutes ago (was active for 12 min)
    ErrorHistoryEntry& e2 = s_history[s_history_head];
    strncpy(e2.code, inlet_code ? inlet_code : "", sizeof(e2.code) - 1);
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

char* getErrorsAsJson() {
    cJSON* root = cJSON_CreateArray();

    ActiveError errors[MAX_ACTIVE_FAULTS];
    int count = getActiveErrors(errors, MAX_ACTIVE_FAULTS);

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
