/*
 * Arctic Heat Pump Controller
 * Today's electrical energy, integrated from the real-time power reading.
 *
 * The total is saved to NVS (daily_energy_store) every kSaveIntervalSeconds
 * while it changes, and on a planned restart, then restored after boot when
 * it's still the same day. An unplanned reset loses at most one interval.
 */
#pragma once

#include <stdint.h>

class DailyEnergy {
public:
    // Readings further apart than this are a gap (bus outage, time jump):
    // the interval isn't integrated rather than guessing the power across it.
    static constexpr int64_t kMaxStepSeconds = 120;
    static constexpr int64_t kSaveIntervalSeconds = 600;

    // Integrate `watts` since the previous call. `day_key` identifies the
    // local calendar day; a new day starts the total again.
    void add(int64_t now_s, int32_t day_key, uint32_t watts) {
        if (day_key != day_) {
            day_ = day_key;
            watt_seconds_ = 0;
            last_s_ = now_s;
            return;
        }
        const int64_t dt = now_s - last_s_;
        last_s_ = now_s;
        if (dt <= 0 || dt > kMaxStepSeconds) return;
        watt_seconds_ += (uint64_t)watts * (uint64_t)dt;
    }

    // Forget the reference point (e.g. the clock isn't set), keeping the total.
    void pause() { last_s_ = INT64_MIN / 2; }

    // Seed the total saved before a reboot. Only before tracking starts; the
    // next add() keeps it if it's for the same day, and starts over if not.
    void restore(int32_t day_key, uint64_t watt_seconds) {
        if (valid()) return;
        day_ = day_key;
        watt_seconds_ = watt_seconds;
        saved_day_ = day_key;
        saved_ws_ = watt_seconds;
        pause();
    }

    // True when the total has changed since the last save and either the
    // interval has passed or `force` (a planned restart) is set.
    bool save_due(int64_t now_s, bool force = false) const {
        if (!valid() || (day_ == saved_day_ && watt_seconds_ == saved_ws_)) return false;
        return force || now_s - last_save_s_ >= kSaveIntervalSeconds;
    }
    void mark_saved(int64_t now_s) {
        saved_day_ = day_;
        saved_ws_ = watt_seconds_;
        last_save_s_ = now_s;
    }

    uint32_t wh() const { return (uint32_t)(watt_seconds_ / 3600); }
    uint64_t watt_seconds() const { return watt_seconds_; }
    int32_t day() const { return day_; }
    bool valid() const { return day_ != INT32_MIN; }

private:
    int32_t day_ = INT32_MIN;
    int64_t last_s_ = 0;
    uint64_t watt_seconds_ = 0;
    int32_t saved_day_ = INT32_MIN;
    uint64_t saved_ws_ = 0;
    int64_t last_save_s_ = 0;
};
