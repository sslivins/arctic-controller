/*
 * Arctic Heat Pump Controller
 * Error Management Module
 * 
 * Provides structured error information, history tracking, and API support.
 */
#pragma once

#include <stdint.h>
#include <time.h>

namespace arctic {

// Maximum number of error history entries
constexpr int ERROR_HISTORY_SIZE = 32;

// Error severity levels
enum class ErrorSeverity {
    INFO,       // Informational (e.g., entering defrost)
    WARNING,    // Warning (e.g., low ambient temp)
    ERROR,      // Error requiring attention
    CRITICAL    // Critical error (compressor protection, high pressure)
};

// Single error definition
struct ErrorDef {
    uint16_t mask;          // Bit mask in error register
    const char* code;       // Short code (e.g., "E01", "E17")
    const char* name;       // Short name (e.g., "INDOOR_EE")
    const char* description;// Human-readable description
    ErrorSeverity severity;
};

// Active error with timestamp
struct ActiveError {
    const char* code;       // Error code (e.g., "E01")
    const char* name;       // Error name
    const char* description;// Description
    ErrorSeverity severity;
    uint8_t register_num;   // 1 or 2 (for register 2137 or 2138)
    uint16_t mask;          // Bit mask
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

// Get error definitions for register 1 (2137)
const ErrorDef* getError1Definitions(int* count);

// Get error definitions for register 2 (2138)
const ErrorDef* getError2Definitions(int* count);

// ============================================================================
// Error History Functions
// ============================================================================

// Update error history based on current state
// Should be called periodically (e.g., every poll)
void updateErrorHistory(uint16_t error1, uint16_t error2);

// Get error history (most recent first)
// Returns number of entries, fills array up to max_entries
int getErrorHistory(ErrorHistoryEntry* history, int max_entries);

// Clear error history
void clearErrorHistory();

// ============================================================================
// JSON Helpers
// ============================================================================

// Get errors as JSON array string (caller must free)
// Format: [{"code":"E01","name":"...","description":"...","severity":"error","active":true}]
char* getErrorsAsJson();

// Get error history as JSON array string (caller must free)
char* getErrorHistoryAsJson();

// ============================================================================
// Utility Functions
// ============================================================================

// Convert severity to string
const char* severityToString(ErrorSeverity severity);

// Check if a specific error is active
bool isErrorActive(uint8_t register_num, uint16_t mask);

}  // namespace arctic
