#include "perf_source.h"

#include <algorithm>

namespace perf {

void MedianWindow::add(uint32_t now_ms, float value) {
    prune(now_ms);
    if (count_ == kWindowCapacity) {
        std::copy(samples_ + 1, samples_ + count_, samples_);
        --count_;
    }
    samples_[count_++] = {now_ms, value};
}

void MedianWindow::prune(uint32_t now_ms) {
    size_t keep_from = 0;
    while (keep_from < count_ && now_ms - samples_[keep_from].t_ms > kWindowMs) ++keep_from;
    if (keep_from == 0) return;
    std::copy(samples_ + keep_from, samples_ + count_, samples_);
    count_ -= keep_from;
}

float MedianWindow::median() const {
    float values[kWindowCapacity];
    for (size_t i = 0; i < count_; ++i) values[i] = samples_[i].value;
    std::sort(values, values + count_);
    if (count_ % 2 == 1) return values[count_ / 2];
    return (values[count_ / 2 - 1] + values[count_ / 2]) / 2.0f;
}

const char* perf_source_name(PerfSource s) {
    return s == PerfSource::External ? "external" : "heat_pump";
}

void SourceSelector::reset() { *this = SourceSelector(); }

void SourceSelector::on_reading(Slot slot, uint32_t now_ms, float celsius) {
    int i = static_cast<int>(slot);
    windows_[i].add(now_ms, celsius);
    // A silent gap breaks the run of good readings just like an error does.
    if (!good_run_[i] || (have_ok_[i] && now_ms - last_ok_ms_[i] > kStaleMs)) {
        good_run_[i] = true;
        good_since_ms_[i] = now_ms;
    }
    have_ok_[i] = true;
    errored_[i] = false;
    last_ok_ms_[i] = now_ms;
}

void SourceSelector::on_error(Slot slot, uint32_t /*now_ms*/) {
    good_run_[static_cast<int>(slot)] = false;
    errored_[static_cast<int>(slot)] = true;
}

bool SourceSelector::failing(int slot, uint32_t now_ms) const {
    return errored_[slot] || (have_ok_[slot] && now_ms - last_ok_ms_[slot] > kStaleMs);
}

bool SourceSelector::healthy(int slot, uint32_t now_ms) const {
    return have_ok_[slot] && good_run_[slot] && now_ms - last_ok_ms_[slot] <= kStaleMs &&
           windows_[slot].size() >= kMinSamples;
}

void SourceSelector::track_transitions(uint32_t now_ms, const HeatPumpContext& hp) {
    if (have_hp_) {
        bool started = hp.compressor_running && !last_hp_.compressor_running;
        bool defrost_ended = !hp.defrost && last_hp_.defrost;
        bool mode_changed = hp.mode != last_hp_.mode;
        if (started || defrost_ended || mode_changed) {
            settle_active_ = true;
            settle_until_ms_ = now_ms + kSettleMs;
        }
    } else if (hp.compressor_running) {
        // Running when first seen: we don't know how long, so let it settle.
        settle_active_ = true;
        settle_until_ms_ = now_ms + kSettleMs;
    }
    have_hp_ = true;
    last_hp_ = hp;
    if (settle_active_ && static_cast<int32_t>(now_ms - settle_until_ms_) >= 0) {
        settle_active_ = false;
    }
}

Selection SourceSelector::evaluate(uint32_t now_ms, const bool configured[kSlotCount],
                                   const HeatPumpContext& hp) {
    track_transitions(now_ms, hp);
    for (auto& w : windows_) w.prune(now_ms);

    Selection sel{};
    sel.source = PerfSource::HeatPump;
    bool any = false;
    bool any_failing = false;
    bool all_healthy = true;
    bool all_recovered = true;
    for (int i = 0; i < kSlotCount; ++i) {
        if (!configured[i]) continue;
        any = true;
        if (failing(i, now_ms)) any_failing = true;
        if (!healthy(i, now_ms)) {
            all_healthy = false;
        } else if (now_ms - good_since_ms_[i] < kRecoverMs) {
            all_recovered = false;
        }
    }
    if (!any) {
        current_ = PerfSource::HeatPump;
        return sel;
    }
    if (!all_healthy) {
        current_ = PerfSource::HeatPump;
    } else if (current_ == PerfSource::HeatPump && all_recovered) {
        current_ = PerfSource::External;
    }
    sel.source = current_;
    sel.fallback = current_ == PerfSource::HeatPump && any_failing;
    sel.pending = current_ == PerfSource::HeatPump && !any_failing;
    if (current_ == PerfSource::External) {
        sel.settling = settle_active_;
        sel.supply_c = configured[0] ? windows_[0].median() : 0.0f;
        sel.return_c = configured[1] ? windows_[1].median() : 0.0f;
    }
    return sel;
}

namespace thermux {

bool candidate(const SensorConfig& s) {
    return s.source == SensorSource::ModbusTcp && s.reg_type == RegisterType::Input &&
           s.value_type == ValueType::Int16 && s.scale_exp == -2 && s.address >= kTempStart &&
           s.address < kTempStart + kChannelCount;
}

bool info_matches(const uint16_t info[kInfoCount]) {
    constexpr uint16_t kMapVersion = 1;
    uint16_t interval_s = info[15];
    return info[0] == kMapVersion && info[4] == kChannelCount && interval_s >= 1 && interval_s <= 3600;
}

int channel_of(const SensorConfig& s) { return static_cast<int>(s.address) - kTempStart; }

void rom_from_regs(const uint16_t regs[kRegsPerRom], uint8_t rom[kRomLen]) {
    for (int i = 0; i < kRegsPerRom; ++i) {
        rom[2 * i] = static_cast<uint8_t>(regs[i] >> 8);
        rom[2 * i + 1] = static_cast<uint8_t>(regs[i]);
    }
}

}  // namespace thermux

}  // namespace perf
