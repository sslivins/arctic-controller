// Direction of travel for a whole-degree temperature reading.
//
// The unit reports water temperatures rounded to 1 °C and a reading near a
// rounding boundary flicks between two values every few seconds, so comparing
// consecutive readings is useless.  Instead every reading is averaged into a
// 15 s bucket (the dither averages out and recovers sub-degree resolution), a
// least-squares line is fitted through the last `window` bucket means, and the
// slope's sign is reported with hysteresis and a minimum hold so the arrow
// can't flicker.  Thresholds were chosen from ~2.5 months of Sun Peaks logs.
#pragma once

#include <stdint.h>

enum class TempTrend : int8_t {
    UNKNOWN = -2,  // not enough history yet (boot, reconnect, sensor gap)
    FALLING = -1,
    STEADY = 0,
    RISING = 1,
};

const char* temp_trend_to_string(TempTrend trend);

class TempTrendTracker {
public:
    static constexpr uint32_t SAMPLE_INTERVAL_MS = 15000;
    static constexpr uint8_t MAX_WINDOW = 60;
    // Samples the arrow must hold before it may change again (1 min).
    static constexpr uint8_t MIN_HOLD_SAMPLES = 4;
    // No reading for this long restarts the window.
    static constexpr uint32_t MAX_GAP_MS = 4 * SAMPLE_INTERVAL_MS;

    // window: samples in the fit (window * 15 s of history).
    // enter/exit: slope thresholds in m°C per minute; an arrow appears at
    // |slope| >= enter and clears once |slope| < exit.
    TempTrendTracker(uint8_t window, uint16_t enter_mc_per_min, uint16_t exit_mc_per_min);

    // Feed every fresh reading (any rate); the arrow updates at most once per
    // SAMPLE_INTERVAL_MS.  An invalid reading restarts the window.
    void update(uint32_t now_ms, int16_t value_c, bool valid);
    void reset();

    uint8_t window() const { return window_; }
    TempTrend trend() const { return trend_; }
    // Last fitted slope (°C/min); 0 until the window is full.
    float slope_c_per_min() const { return slope_; }

private:
    void push(float sample);
    float fit() const;

    uint8_t window_;
    float enter_;
    float exit_;
    float samples_[MAX_WINDOW] = {};
    uint8_t count_ = 0;
    uint8_t head_ = 0;
    bool have_reading_ = false;
    uint32_t last_reading_ms_ = 0;
    uint32_t bucket_start_ms_ = 0;
    float bucket_sum_ = 0.0f;
    uint16_t bucket_n_ = 0;
    uint32_t since_change_ = 0;
    float slope_ = 0.0f;
    TempTrend trend_ = TempTrend::UNKNOWN;
};

// Supply and return move 0.15-0.35 °C/min during a run: 7 min window, arrow
// at 0.1 °C/min.  The tank moves 0.05-0.15 °C/min: 15 min window, arrow at
// 0.05 °C/min.
inline TempTrendTracker make_water_trend_tracker() { return TempTrendTracker(28, 100, 50); }
inline TempTrendTracker make_tank_trend_tracker() { return TempTrendTracker(60, 50, 25); }
