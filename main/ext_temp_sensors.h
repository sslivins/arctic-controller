/*
 * External supply/return temperature sensors over Modbus TCP, for the heat
 * output & COP estimate.
 *
 * A background worker polls any sensor set to Modbus TCP every few seconds
 * and feeds perf::SourceSelector. Nothing here holds the UI or RS-485 locks;
 * consumers only take a short internal mutex.
 */
#pragma once

#include <stdint.h>

#include "perf_settings.h"
#include "perf_source.h"

namespace ext_temp {

enum class Error : uint8_t {
    None = 0,
    NotRead,        // no attempt yet
    Resolve,        // hostname didn't resolve
    Connect,        // refused / unreachable / timed out connecting
    Timeout,        // connected but no (complete) reply
    Protocol,       // malformed or mismatched reply
    Exception,      // the device answered with a Modbus exception
    Rejected,       // BACnet Error/Reject/Abort response
    Units,          // BACnet object uses unsupported engineering units
    NoReading,      // the register holds the "no reading" value
    OutOfRange,     // decoded value outside -40..120 °C
    SensorChanged,  // a different Thermux sensor is now on this channel
    WrongDevice,    // BACnet device instance no longer matches the saved binding
    Busy,           // a test is already running
};
const char* error_name(Error e);

struct SlotStatus {
    bool configured;   // set to Modbus TCP
    bool has_reading;  // at least one good reading since the settings changed
    float celsius;     // latest good reading (unfiltered)
    uint32_t age_s;    // since that reading
    Error error;       // result of the latest attempt
    uint8_t exception; // Modbus exception or BACnet error/reject/abort code
    uint8_t error_class;
    char object_name[41];
    char rom_hex[17];
    bool bacnet_device_known;
    uint32_t bacnet_device_instance;
};

struct TestResult {
    Error error;
    uint8_t exception;
    uint8_t error_class;
    float celsius;
    // The register type that answered. When the requested one is rejected as
    // an illegal function/address, the test tries the other one.
    perf::RegisterType reg_type;
    // Filled when the device identifies as a Thermux and the register is one
    // of its temperature channels.
    bool thermux;
    int channel;
    uint8_t thermux_status;  // perf::thermux::ChannelStatus
    uint16_t thermux_age_s;  // 65535 = never read
    bool rom_valid;
    char rom_hex[17];
    char object_name[41];
    uint32_t bacnet_units;
    uint32_t bacnet_reliability;
    bool bacnet_reliability_known;
    bool bacnet_device_known;
    uint32_t bacnet_device_instance;
};

struct BrowseSensor {
    perf::BacnetObjectType object_type;
    uint32_t object_instance;
    char object_name[41];
    float celsius;
    uint32_t units;
    uint32_t reliability;
    bool reliability_known;
    bool available;
    bool rom_valid;
    char rom_hex[17];
};

struct BrowseResult {
    Error error;
    uint8_t exception;
    uint8_t error_class;
    char device_name[41];
    char model_name[41];
    bool device_instance_known;
    uint32_t device_instance;
    BrowseSensor sensors[64];
    size_t count;
    uint32_t total_objects;
    uint32_t scanned;
    bool truncated;
};

// Loads settings and, when `allow_worker` and a sensor needs it, starts the
// worker. Call once at boot, after NVS.
void init(bool allow_worker);

// Persists anything the worker learned (a Thermux sensor's ROM ID). The worker
// runs on a PSRAM stack and so must never write flash itself; call this
// periodically from a task with an internal-RAM stack.
void service();

perf::Settings settings();

// Validates and saves. `sensor_edited[slot]` marks sensors the user edited
// (their learned Thermux ROM ID is dropped, so a replaced sensor is accepted);
// other sensors keep the stored one. Returns Invalid::None on success.
perf::Invalid apply_settings(const perf::Settings& s, const bool sensor_edited[perf::kSlotCount],
                             bool* saved);

void slot_status(SlotStatus out[perf::kSlotCount]);

// Decide where this estimate's temperatures come from. Cheap; called on every
// state decode.
perf::Selection choose_source(const perf::HeatPumpContext& hp);

// Test a sensor configuration (which need not be saved). test_start() queues
// it on the worker and returns a ticket, or 0 when a test is already queued.
uint32_t test_start(const perf::SensorConfig& cfg);
// True once the ticket's result is ready.
bool test_result(uint32_t ticket, TestResult* out);
// Convenience for the HTTP handler: start and wait up to `timeout_ms`.
bool test_blocking(const perf::SensorConfig& cfg, TestResult* out, uint32_t timeout_ms);

bool browse_blocking(const char* host, uint16_t port, BrowseResult* out, uint32_t timeout_ms);

}  // namespace ext_temp
