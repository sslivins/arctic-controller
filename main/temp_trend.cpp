#include "temp_trend.h"

const char* temp_trend_to_string(TempTrend trend) {
    switch (trend) {
        case TempTrend::RISING:  return "rising";
        case TempTrend::FALLING: return "falling";
        case TempTrend::STEADY:  return "steady";
        default:                 return "unknown";
    }
}

TempTrendTracker::TempTrendTracker(uint8_t window, uint16_t enter_mc_per_min,
                                   uint16_t exit_mc_per_min)
    : window_(window < 2 ? 2 : (window > MAX_WINDOW ? MAX_WINDOW : window)),
      enter_(enter_mc_per_min / 1000.0f),
      exit_(exit_mc_per_min / 1000.0f) {}

void TempTrendTracker::reset() {
    count_ = 0;
    head_ = 0;
    have_reading_ = false;
    bucket_sum_ = 0.0f;
    bucket_n_ = 0;
    since_change_ = 0;
    slope_ = 0.0f;
    trend_ = TempTrend::UNKNOWN;
}

float TempTrendTracker::fit() const {
    // Oldest sample first; x is the sample index.
    const uint8_t n = window_;
    const float mx = (n - 1) / 2.0f;
    float my = 0.0f;
    for (uint8_t i = 0; i < n; i++) my += samples_[(head_ + i) % n];
    my /= n;
    float sxy = 0.0f, sxx = 0.0f;
    for (uint8_t i = 0; i < n; i++) {
        const float dx = i - mx;
        sxy += dx * (samples_[(head_ + i) % n] - my);
        sxx += dx * dx;
    }
    return sxy / sxx * (60000.0f / SAMPLE_INTERVAL_MS);
}

void TempTrendTracker::update(uint32_t now_ms, int16_t value_c, bool valid) {
    if (!valid) {
        reset();
        return;
    }
    if (have_reading_ && now_ms - last_reading_ms_ > MAX_GAP_MS) reset();
    if (!have_reading_) {
        have_reading_ = true;
        bucket_start_ms_ = now_ms;
    }
    last_reading_ms_ = now_ms;
    bucket_sum_ += value_c;
    bucket_n_++;
    if (now_ms - bucket_start_ms_ < SAMPLE_INTERVAL_MS) return;

    const float mean = bucket_sum_ / bucket_n_;
    bucket_sum_ = 0.0f;
    bucket_n_ = 0;
    bucket_start_ms_ = now_ms;
    push(mean);
}

void TempTrendTracker::push(float sample) {
    // Ring buffer: head_ is the oldest sample once full.
    if (count_ < window_) {
        samples_[count_++] = sample;
        if (count_ < window_) return;
        head_ = 0;
        trend_ = TempTrend::STEADY;
        since_change_ = MIN_HOLD_SAMPLES;
    } else {
        samples_[head_] = sample;
        head_ = (head_ + 1) % window_;
    }

    slope_ = fit();
    if (since_change_ < MIN_HOLD_SAMPLES) since_change_++;

    TempTrend next = trend_;
    if (trend_ == TempTrend::RISING) {
        if (slope_ < exit_) next = slope_ <= -enter_ ? TempTrend::FALLING : TempTrend::STEADY;
    } else if (trend_ == TempTrend::FALLING) {
        if (slope_ > -exit_) next = slope_ >= enter_ ? TempTrend::RISING : TempTrend::STEADY;
    } else {
        if (slope_ >= enter_) next = TempTrend::RISING;
        else if (slope_ <= -enter_) next = TempTrend::FALLING;
    }
    if (next != trend_ && since_change_ >= MIN_HOLD_SAMPLES) {
        trend_ = next;
        since_change_ = 0;
    }
}
