/*
 * Arctic Heat Pump Controller
 * Error Management Module
 *
 * Presentation adapter over the arctic-macon library's native fault decode
 * (macon_decode_faults / MACON_FAULT_BITS). The library owns the canonical
 * (reg, bit) -> code / label / severity table; this module adds the controller
 * concerns the library does not carry: human remedy ("resolution") text,
 * per-fault first-seen tracking, and a cleared/active history ring buffer.
 *
 * There is no fictional Arctic error1/error2 bitfield here any more — a fault's
 * stable identity is the (reg, bit) pair straight from the Macon mainboard.
 */
#pragma once

#include <stdint.h>
#include <time.h>

namespace arctic {

// Maximum number of error history entries (ring buffer — oldest entries are overwritten)
constexpr int ERROR_HISTORY_SIZE = 50;

// Error severity levels
enum class ErrorSeverity {
    INFO,       // Informational (e.g., run indicator)
    WARNING,    // Warning (e.g., low ambient temp)
    ERROR,      // Error requiring attention
    CRITICAL    // Critical error (compressor protection, high pressure)
};

// Active fault with timestamp. Identity is (reg, bit) — the raw Macon fault
// register and the bit within it — NOT a code string (codes are not unique;
// e.g. E28 and E05 each appear at two distinct bits).
struct ActiveError {
    const char* code;       // Code as shown on the OEM LCD (e.g. "P02", "E19")
    const char* name;       // Short label (from the macon library)
    const char* description;// Human-readable description (from the macon library)
    const char* resolution; // Suggested resolution steps (controller-owned)
    ErrorSeverity severity;
    uint16_t reg;           // Macon fault register (2007/2125/2126/2127/2128)
    uint8_t  bit;           // Bit within that register (0..7)
    time_t first_seen;      // When error first appeared
    time_t last_seen;       // Last time error was active
    bool active;            // Currently active
};

// Error history entry
struct ErrorHistoryEntry {
    char code[8];           // Error code
    time_t occurred;        // When it occurred
    time_t cleared;         // When it cleared (0 if still active)
    bool is_active;         // Still active
};

// ============================================================================
// Error Information Functions
// ============================================================================

// Get array of currently active errors
// Returns number of active errors, fills array up to max_errors
int getActiveErrors(ActiveError* errors, int max_errors);

// Get total count of active errors
int getActiveErrorCount();

// Get highest severity of active errors
ErrorSeverity getHighestSeverity();

// Look up display metadata for a fault code (first matching site in the macon
// library table). Returns true if the code is known; any out-pointer may be
// null. Used to enrich history entries, which only store the code string.
bool describeFaultCode(const char* code, const char** name_out,
                       const char** description_out,
                       const char** resolution_out,
                       ErrorSeverity* severity_out);

// ============================================================================
// Error History Functions
// ============================================================================

// Update error history from the five raw Macon fault-register bytes
// (reg 2007, 2125, 2126, 2127, 2128). Should be called each poll/feed cycle.
void updateErrorHistory(uint8_t fault_run, uint8_t fault_ee, uint8_t fault_comp,
                        uint8_t fault_elec, uint8_t fault_ref);

// Get error history (most recent first)
// Returns number of entries, fills array up to max_entries
int getErrorHistory(ErrorHistoryEntry* history, int max_entries);

// Clear error history
void clearErrorHistory();

// Populate error history with demo data (for demo mode)
// Only populates if history is currently empty
void populateDemoErrorHistory();

// ============================================================================
// JSON Helpers
// ============================================================================

// Get errors as JSON array string (caller must free)
char* getErrorsAsJson();

// Get error history as JSON array string (caller must free)
char* getErrorHistoryAsJson();

// ============================================================================
// Utility Functions
// ============================================================================

// Convert severity to string
const char* severityToString(ErrorSeverity severity);

// Format duration as human readable string (e.g., "2h 15m", "45s", "Active")
// Returns static buffer, not thread-safe
const char* formatDuration(time_t start_time, time_t end_time = 0);

// Check if a specific (reg, bit) fault is currently active.
bool isErrorActive(uint16_t reg, uint8_t bit);

}  // namespace arctic
