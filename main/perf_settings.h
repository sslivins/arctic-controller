/*
 * Heat output & COP settings: assumed flow, loop fluid, and where the supply
 * and return temperatures come from (the heat pump's own sensors, or any
 * Modbus TCP device such as a Thermux).
 *
 * Pure logic plus NVS persistence; host-testable.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "macon_fluid.h"

namespace perf {

enum class SensorSource : uint8_t { HeatPump = 0, ModbusTcp = 1 };
enum class RegisterType : uint8_t { Input = 0, Holding = 1 };
enum class ValueType : uint8_t {
    Int16 = 0,
    Uint16 = 1,
    Float32 = 2,         // high word first (ABCD)
    Float32Swapped = 3,  // low word first (CDAB)
};
// Raw register value that means "no reading" (16-bit value types only).
enum class NoReading : uint8_t { None = 0, X8000 = 1, X7FFF = 2, XFFFF = 3 };

enum class Slot : uint8_t { Supply = 0, Return = 1 };
constexpr int kSlotCount = 2;

constexpr size_t kHostMax = 64;  // including the terminator
constexpr size_t kRomLen = 8;

struct SensorConfig {
    SensorSource source;
    char host[kHostMax];
    uint16_t port;
    uint8_t unit_id;
    uint16_t address;
    RegisterType reg_type;
    ValueType value_type;
    int8_t scale_exp;  // value = raw * 10^scale_exp; 0..-3
    NoReading no_reading;
    // ROM ID of the Thermux sensor first seen on this channel, so a different
    // sensor moved onto it can be noticed. Cleared whenever the sensor is saved.
    bool rom_known;
    uint8_t rom[kRomLen];
};

struct Settings {
    uint16_t flow_lpm_x10;
    arctic::LoopFluid fluid;
    uint8_t glycol_pct;  // by volume; kept when switching to Water
    SensorConfig sensors[kSlotCount];
};

constexpr uint16_t kFlowMinX10 = 10;      // 1.0 L/min
constexpr uint16_t kFlowMaxX10 = 3000;    // 300.0 L/min
constexpr uint16_t kFlowDefaultX10 = 400; // 40 L/min
constexpr uint8_t kGlycolStep = 5;
constexpr uint8_t kGlycolDefault = 30;
constexpr uint16_t kDefaultPort = 502;
constexpr uint8_t kDefaultUnitId = 1;

// Readings outside this range are treated as no reading.
constexpr float kMinPlausibleC = -40.0f;
constexpr float kMaxPlausibleC = 120.0f;

Settings defaults();
SensorConfig default_sensor();

enum class Invalid : uint8_t {
    None = 0,
    Flow,
    Fluid,
    Glycol,
    Source,
    Host,
    Port,
    Register,
    RegisterType,
    ValueType,
    Scale,
    NoReading,
};
Invalid validate_sensor(const SensorConfig& s);
Invalid validate(const Settings& s);
const char* invalid_name(Invalid v);  // the API field name, e.g. "host"

// True for a hostname or IPv4 address made of letters, digits, '.', '-', '_'.
bool host_valid(const char* host);

// Same Modbus source (host, port, unit, register, register type, value type).
bool same_source(const SensorConfig& a, const SensorConfig& b);

// Registers the value spans (1 or 2).
uint16_t register_count(ValueType t);

enum class DecodeResult : uint8_t { Ok, NoReading, OutOfRange };
// Decode `regs` (register_count() of them) into degrees C.
DecodeResult decode_value(const SensorConfig& s, const uint16_t* regs, float* out_c);

// Versioned blob with CRC. deserialize() returns false (and leaves `out`
// untouched) on a wrong size, version, CRC, or an invalid result.
constexpr size_t kBlobSize = 1 + 2 + 1 + 1 + kSlotCount * (1 + kHostMax + 2 + 1 + 2 + 1 + 1 + 1 + 1 + 1 + kRomLen) + 4;
void serialize(const Settings& s, uint8_t* buf);
bool deserialize(const uint8_t* buf, size_t len, Settings* out);

// NVS persistence (namespace "perf"). load() returns defaults when nothing is
// stored or the stored blob is unreadable.
Settings load();
bool save(const Settings& s);

// Whether any sensor reads over the network.
bool uses_network(const Settings& s);

const char* source_name(SensorSource s);   // "heat_pump" / "modbus_tcp"
const char* fluid_name(arctic::LoopFluid f);  // "water" / "propylene_glycol" / "ethylene_glycol"
const char* register_type_name(RegisterType t);  // "input" / "holding"
const char* value_type_name(ValueType t);  // "int16" / "uint16" / "float32" / "float32_swapped"
const char* no_reading_name(NoReading n);  // "none" / "0x8000" / "0x7fff" / "0xffff"
bool parse_source(const char* s, SensorSource* out);
bool parse_fluid(const char* s, arctic::LoopFluid* out);
bool parse_register_type(const char* s, RegisterType* out);
bool parse_value_type(const char* s, ValueType* out);
bool parse_no_reading(const char* s, NoReading* out);

}  // namespace perf
