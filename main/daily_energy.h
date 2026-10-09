/*
 * Arctic Heat Pump Controller
 * Today's electrical energy, integrated from the real-time power reading.
 *
 * Kept in RAM only: a reboot starts the day's total again from zero.
 */
#pragma once

#include <stdint.h>

class DailyEnergy {
public:
    // Readings further apart than this are a gap (bus outage, time jump):
    // the interval isn't integrated rather than guessing the power across it.
    static constexpr int64_t kMaxStepSeconds = 120;

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

    uint32_t wh() const { return (uint32_t)(watt_seconds_ / 3600); }
    bool valid() const { return day_ != INT32_MIN; }

private:
    int32_t day_ = INT32_MIN;
    int64_t last_s_ = 0;
    uint64_t watt_seconds_ = 0;
};
