/*
 * Chooses where the supply/return temperatures for the heat output and COP
 * estimate come from, and smooths the external readings.
 *
 * External (Modbus TCP) readings are median-filtered over a short window.
 * If any configured external sensor stops giving good readings, both
 * temperatures fall back to the heat pump's own sensors (never a mix of one
 * external and one internal reading), and switch back only after every
 * external sensor has read cleanly for a while. After the compressor starts,
 * a defrost ends or the mode changes, the external estimate is held off
 * while the loop settles.
 *
 * Pure logic, host-testable. Times are monotonic milliseconds.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "macon_state.h"
#include "perf_settings.h"

namespace perf {

constexpr uint32_t kWindowMs = 120000;
constexpr size_t kWindowCapacity = 32;
constexpr size_t kMinSamples = 3;
constexpr uint32_t kStaleMs = 60000;
constexpr uint32_t kRecoverMs = 60000;
constexpr uint32_t kSettleMs = 180000;

class MedianWindow {
public:
    void add(uint32_t now_ms, float value);
    void prune(uint32_t now_ms);
    size_t size() const { return count_; }
    // Median of the samples; call only when size() > 0.
    float median() const;
    void clear() { count_ = 0; }

private:
    struct Sample {
        uint32_t t_ms;
        float value;
    };
    Sample samples_[kWindowCapacity];
    size_t count_ = 0;
};

enum class PerfSource : uint8_t { HeatPump = 0, External = 1 };
const char* perf_source_name(PerfSource s);  // "heat_pump" / "external"

struct HeatPumpContext {
    bool compressor_running;
    bool defrost;
    arctic::MaconMode mode;
};

struct Selection {
    PerfSource source;
    // True when an external sensor is failing (errors, or silent for too
    // long), so the estimate fell back to the heat pump's own sensors.
    bool fallback;
    // External sensors are reading fine but haven't yet been clean for long
    // enough to be trusted (after start-up, a settings change or a failure);
    // the heat pump's sensors are used meanwhile.
    bool pending;
    // External sensors in use but the loop hasn't settled yet; no estimate.
    bool settling;
    // Filtered external temperatures; valid when source == External and that
    // slot is configured (the caller uses the heat pump's reading otherwise).
    float supply_c;
    float return_c;
};

class SourceSelector {
public:
    // Forget everything (e.g. after the sensor settings change).
    void reset();
    void on_reading(Slot slot, uint32_t now_ms, float celsius);
    void on_error(Slot slot, uint32_t now_ms);
    Selection evaluate(uint32_t now_ms, const bool configured[kSlotCount], const HeatPumpContext& hp);

private:
    bool healthy(int slot, uint32_t now_ms) const;
    bool failing(int slot, uint32_t now_ms) const;
    void track_transitions(uint32_t now_ms, const HeatPumpContext& hp);

    MedianWindow windows_[kSlotCount];
    bool have_ok_[kSlotCount] = {};
    bool errored_[kSlotCount] = {};
    uint32_t last_ok_ms_[kSlotCount] = {};
    bool good_run_[kSlotCount] = {};
    uint32_t good_since_ms_[kSlotCount] = {};
    PerfSource current_ = PerfSource::HeatPump;
    bool have_hp_ = false;
    HeatPumpContext last_hp_{};
    bool settle_active_ = false;
    uint32_t settle_until_ms_ = 0;
};

// Thermux (github.com/sslivins/thermux) Modbus map v1: channel temperatures
// at input registers 100..199 in 0.01 °C, 0x8000 when invalid.
namespace thermux {
constexpr uint16_t kInfoCount = 16;
constexpr uint16_t kTempStart = 100;
constexpr uint16_t kStatusStart = 200;
constexpr uint16_t kRomStart = 1000;
constexpr uint16_t kRegsPerRom = 4;
constexpr uint16_t kChannelCount = 100;

enum class ChannelStatus : uint8_t { Ok = 0, Unassigned = 1, Missing = 2, ReadError = 3, Stale = 4 };

// Whether a sensor config looks like a Thermux temperature channel.
bool candidate(const SensorConfig& s);
// Whether the 16 info registers identify a Thermux with map v1.
bool info_matches(const uint16_t info[kInfoCount]);
int channel_of(const SensorConfig& s);
void rom_from_regs(const uint16_t regs[kRegsPerRom], uint8_t rom[kRomLen]);
}  // namespace thermux

}  // namespace perf
