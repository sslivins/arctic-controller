/*
 * Arctic Heat Pump Controller
 * Daily energy ledger: electrical and estimated thermal energy per local day.
 *
 * The controller feeds add() about once a second with what the unit is doing.
 * Each interval is booked to the state seen at its START (the previous call),
 * so a mode change is only counted from the reading that reported it. An
 * interval longer than kMaxStepSeconds (bus outage, reboot, clock jump) is a
 * gap and isn't booked at all; nor is time while the unit is disconnected.
 * Both show up as missing coverage rather than as standby.
 *
 * Thermal energy is only booked over intervals where the performance estimate
 * was valid, together with the electrical energy of those same intervals, so
 * thermal / covered electrical is an honest estimated COP even on days with
 * holes in the estimate.
 *
 * Pure logic (no IDF, no clock): energy_store.cpp persists it.
 */
#pragma once

#include <stdint.h>
#include <string.h>

namespace energy {

enum Bucket : uint8_t {
    BUCKET_HEATING = 0,
    BUCKET_COOLING = 1,
    BUCKET_HOT_WATER = 2,
    // Compressor off: controller, fan run-on, circulation and freeze protection.
    BUCKET_STANDBY = 3,
};
constexpr int kBuckets = 4;
constexpr int kModes = 3;  // heating, cooling, hot water

enum Source : uint8_t {
    SOURCE_UNIT = 0,      // the unit's own inlet/outlet sensors
    SOURCE_EXTERNAL = 1,  // external (BACnet / Modbus TCP) sensors
    SOURCE_FALLBACK = 2,  // external configured but failing; unit sensors used
    SOURCE_SETTLING = 3,  // external, settling after a start/defrost/mode change
};
constexpr int kSources = 4;

// What the unit was doing at one reading.
struct Input {
    bool connected = false;      // fresh data from the unit
    bool power_valid = false;
    uint32_t power_w = 0;
    bool running = false;        // compressor
    bool defrost = false;
    Bucket bucket = BUCKET_STANDBY;
    bool thermal_valid = false;  // performance estimate valid
    int32_t thermal_w = 0;       // signed: + heating, - cooling
    Source source = SOURCE_UNIT;
    // Water-side heat during defrost from the unit's own sensors (usually
    // negative: defrost pulls heat back out of the loop).
    bool defrost_thermal_valid = false;
    int32_t defrost_thermal_w = 0;
    bool outdoor_valid = false;
    int16_t outdoor_c = 0;
    uint16_t flow_lpm_x10 = 0;
    uint8_t glycol_pct = 0;
};

// The local calendar day a reading falls in.
struct Day {
    uint32_t date = 0;        // YYYYMMDD
    int64_t start_s = 0;      // epoch of local midnight
    int16_t utc_offset_min = 0;
};

enum : uint8_t {
    DAY_MIXED_SETTINGS = 1 << 0,  // flow / glycol changed during the day
    DAY_SETTINGS_SET = 1 << 1,
    DAY_OUTDOOR_SET = 1 << 2,
};

// One finished (or in-progress) day as stored and reported. Fixed layout:
// it's persisted in NVS month blobs, so only ever append fields.
struct DayRecord {
    uint32_t date;                  // YYYYMMDD; 0 = empty slot
    int16_t utc_offset_min;
    uint8_t flags;
    uint8_t glycol_pct;
    uint16_t flow_lpm_x10;
    uint16_t starts;
    uint32_t elec_wh[kBuckets];
    uint32_t elec_unattributed_wh;  // carried over from firmware before the ledger
    uint32_t thermal_wh[kModes];    // estimated heat delivered/removed (magnitude)
    uint32_t covered_elec_wh[kModes];
    int32_t defrost_thermal_wh;
    uint16_t defrosts;
    uint16_t reserved;
    uint32_t defrost_s;
    uint32_t power_s;               // seconds with a valid power reading
    uint32_t thermal_s;             // seconds with a valid thermal estimate
    uint32_t source_s[kSources];    // compressor-running seconds by temperature source
    uint32_t run_s[kModes];
    int16_t outdoor_min_deci;
    int16_t outdoor_max_deci;
    int16_t outdoor_avg_deci;
    int16_t reserved2;
};
static_assert(sizeof(DayRecord) == 112, "DayRecord is persisted; keep its layout");

inline uint32_t total_elec_wh(const DayRecord& r) {
    uint32_t t = r.elec_unattributed_wh;
    for (int b = 0; b < kBuckets; b++) t += r.elec_wh[b];
    return t;
}

// Running totals for the open day, at watt-second resolution. Persisted as the
// "today" checkpoint, so only ever append fields.
struct DayAccum {
    uint32_t date;
    int16_t utc_offset_min;
    uint8_t flags;
    uint8_t glycol_pct;
    uint16_t flow_lpm_x10;
    uint16_t starts;
    uint16_t defrosts;
    uint16_t reserved;
    uint64_t elec_ws[kBuckets];
    uint64_t elec_unattributed_ws;
    uint64_t thermal_ws[kModes];
    uint64_t covered_elec_ws[kModes];
    int64_t defrost_thermal_ws;
    uint32_t defrost_s;
    uint32_t power_s;
    uint32_t thermal_s;
    uint32_t source_s[kSources];
    uint32_t run_s[kModes];
    int16_t outdoor_min_deci;
    int16_t outdoor_max_deci;
    int32_t reserved3;
    int64_t outdoor_sum_deci_s;
    uint32_t outdoor_s;
    uint32_t reserved2;
};
// No padding: the checkpoint is CRC'd and compared byte for byte.
static_assert(sizeof(DayAccum) == 176, "DayAccum is persisted; keep its layout");

inline uint32_t ws_to_wh(uint64_t ws) { return (uint32_t)((ws + 1800) / 3600); }
inline int32_t ws_to_wh_signed(int64_t ws) {
    return (int32_t)(ws >= 0 ? (ws + 1800) / 3600 : -((-ws + 1800) / 3600));
}

inline DayRecord to_record(const DayAccum& a) {
    DayRecord r;
    memset(&r, 0, sizeof(r));
    r.date = a.date;
    r.utc_offset_min = a.utc_offset_min;
    r.flags = a.flags;
    r.glycol_pct = a.glycol_pct;
    r.flow_lpm_x10 = a.flow_lpm_x10;
    r.starts = a.starts;
    for (int b = 0; b < kBuckets; b++) r.elec_wh[b] = ws_to_wh(a.elec_ws[b]);
    r.elec_unattributed_wh = ws_to_wh(a.elec_unattributed_ws);
    for (int m = 0; m < kModes; m++) {
        r.thermal_wh[m] = ws_to_wh(a.thermal_ws[m]);
        r.covered_elec_wh[m] = ws_to_wh(a.covered_elec_ws[m]);
        r.run_s[m] = a.run_s[m];
    }
    r.defrost_thermal_wh = ws_to_wh_signed(a.defrost_thermal_ws);
    r.defrosts = a.defrosts;
    r.defrost_s = a.defrost_s;
    r.power_s = a.power_s;
    r.thermal_s = a.thermal_s;
    for (int s = 0; s < kSources; s++) r.source_s[s] = a.source_s[s];
    if (a.outdoor_s > 0) {
        r.outdoor_min_deci = a.outdoor_min_deci;
        r.outdoor_max_deci = a.outdoor_max_deci;
        const int64_t avg = a.outdoor_sum_deci_s / (int64_t)a.outdoor_s;
        r.outdoor_avg_deci = (int16_t)avg;
    }
    return r;
}

class EnergyLedger {
public:
    static constexpr int64_t kMaxStepSeconds = 120;
    static constexpr int64_t kSaveIntervalSeconds = 600;

    // Feed one reading. Returns true when it closed the previous day; that
    // day's record is then available from take_finished() until the next add().
    bool add(int64_t now_s, const Day& day, const Input& in) {
        has_finished_ = false;
        if (!open_) {
            open_day(day);
            set_reference(now_s, in);
            return false;
        }
        if (day.date < cur_.date) {
            // The clock went back past midnight: never reopen a closed day.
            // Drop the reference so nothing is booked until it's caught up.
            has_prev_ = false;
            return false;
        }
        if (!has_prev_ || now_s < prev_s_) {
            set_reference(now_s, in);
            if (day.date != cur_.date) close_and_open(day);
            return has_finished_;
        }
        const int64_t dt = now_s - prev_s_;
        const bool in_step = dt <= kMaxStepSeconds;
        if (day.date != cur_.date) {
            if (in_step && prev_s_ < day.start_s) book(prev_s_, day.start_s, prev_);
            close_and_open(day);
            if (in_step) book(prev_s_ > day.start_s ? prev_s_ : day.start_s, now_s, prev_);
        } else if (in_step) {
            book(prev_s_, now_s, prev_);
        }
        if (in_step && in.connected && prev_.connected) {
            if (in.running && !prev_.running && cur_.starts < UINT16_MAX) cur_.starts++;
            if (in.defrost && !prev_.defrost && cur_.defrosts < UINT16_MAX) cur_.defrosts++;
        }
        set_reference(now_s, in);
        return has_finished_;
    }

    // Forget the reference point (e.g. the clock isn't set), keeping the totals.
    void pause() { has_prev_ = false; }

    // Seed the open day from a checkpoint. Only before tracking starts. A day
    // other than the next reading's is closed by that reading.
    void restore(const DayAccum& a) {
        if (open_) return;
        cur_ = a;
        open_ = true;
        has_prev_ = false;
        saved_ = a;
        has_saved_ = true;
    }

    // Electrical energy the firmware integrated before the ledger existed.
    void add_unattributed(uint64_t ws) { cur_.elec_unattributed_ws += ws; }

    bool take_finished(DayRecord* out) const {
        if (!has_finished_) return false;
        *out = finished_;
        return true;
    }

    bool save_due(int64_t now_s, bool force = false) const {
        if (!open_ || (has_saved_ && memcmp(&saved_, &cur_, sizeof(cur_)) == 0)) return false;
        return force || !has_saved_ || saved_.date != cur_.date ||
               now_s - last_save_s_ >= kSaveIntervalSeconds;
    }
    void mark_saved(int64_t now_s) { mark_saved(now_s, cur_); }
    // `what` is the accum() copy that was written, which may trail cur_.
    void mark_saved(int64_t now_s, const DayAccum& what) {
        saved_ = what;
        has_saved_ = true;
        last_save_s_ = now_s;
    }

    bool valid() const { return open_; }
    const DayAccum& accum() const { return cur_; }
    DayRecord today() const { return to_record(cur_); }
    // total_elec_wh(today()) without building a record on the caller's stack.
    uint32_t today_elec_wh() const {
        uint32_t t = ws_to_wh(cur_.elec_unattributed_ws);
        for (int b = 0; b < kBuckets; b++) t += ws_to_wh(cur_.elec_ws[b]);
        return t;
    }

private:
    void open_day(const Day& day) {
        memset(&cur_, 0, sizeof(cur_));
        cur_.date = day.date;
        cur_.utc_offset_min = day.utc_offset_min;
        open_ = true;
    }

    void close_and_open(const Day& day) {
        finished_ = to_record(cur_);
        has_finished_ = true;
        open_day(day);
    }

    void set_reference(int64_t now_s, const Input& in) {
        prev_ = in;
        prev_s_ = now_s;
        has_prev_ = true;
    }

    // Book [a, b) to the open day using the state `p` that held over it.
    void book(int64_t a, int64_t b, const Input& p) {
        if (b <= a || !p.connected) return;
        const uint32_t dt = (uint32_t)(b - a);
        const bool mode = p.bucket < kModes;
        if (p.power_valid) {
            cur_.elec_ws[p.bucket] += (uint64_t)p.power_w * dt;
            cur_.power_s += dt;
        }
        if (p.running && mode) cur_.run_s[p.bucket] += dt;
        if (p.running && !p.defrost) cur_.source_s[p.source] += dt;
        if (p.defrost) {
            cur_.defrost_s += dt;
            if (p.defrost_thermal_valid) {
                cur_.defrost_thermal_ws += (int64_t)p.defrost_thermal_w * dt;
            }
        }
        if (p.thermal_valid && p.power_valid && mode) {
            // Heat in the direction the mode expects; a reading the wrong way
            // round adds nothing but still counts its electricity, so a bad
            // estimate drags the COP down rather than up.
            const int32_t q = p.bucket == BUCKET_COOLING ? -p.thermal_w : p.thermal_w;
            if (q > 0) cur_.thermal_ws[p.bucket] += (uint64_t)q * dt;
            cur_.covered_elec_ws[p.bucket] += (uint64_t)p.power_w * dt;
            cur_.thermal_s += dt;
            note_settings(p);
        }
        if (p.outdoor_valid) {
            const int16_t deci = (int16_t)(p.outdoor_c * 10);
            if (!(cur_.flags & DAY_OUTDOOR_SET)) {
                cur_.outdoor_min_deci = deci;
                cur_.outdoor_max_deci = deci;
                cur_.flags |= DAY_OUTDOOR_SET;
            }
            if (deci < cur_.outdoor_min_deci) cur_.outdoor_min_deci = deci;
            if (deci > cur_.outdoor_max_deci) cur_.outdoor_max_deci = deci;
            cur_.outdoor_sum_deci_s += (int64_t)deci * dt;
            cur_.outdoor_s += dt;
        }
    }

    void note_settings(const Input& p) {
        if (!(cur_.flags & DAY_SETTINGS_SET)) {
            cur_.flow_lpm_x10 = p.flow_lpm_x10;
            cur_.glycol_pct = p.glycol_pct;
            cur_.flags |= DAY_SETTINGS_SET;
        } else if (cur_.flow_lpm_x10 != p.flow_lpm_x10 || cur_.glycol_pct != p.glycol_pct) {
            cur_.flags |= DAY_MIXED_SETTINGS;
        }
    }

    DayAccum cur_{};
    bool open_ = false;
    Input prev_{};
    int64_t prev_s_ = 0;
    bool has_prev_ = false;
    DayRecord finished_{};
    bool has_finished_ = false;
    DayAccum saved_{};
    bool has_saved_ = false;
    int64_t last_save_s_ = 0;
};

// Time-weighted average of the power reading between take() calls, for the
// 30 s telemetry samples (a point reading would alias the unit's ramps).
class PowerAverager {
public:
    void add(uint32_t now_ms, bool valid, uint32_t watts) {
        if (has_last_ && last_valid_) {
            const uint32_t dt = now_ms - last_ms_;
            if (dt > 0 && dt <= kMaxStepMs) {
                sum_ += (uint64_t)last_w_ * dt;
                ms_ += dt;
            }
        }
        has_last_ = true;
        last_ms_ = now_ms;
        last_valid_ = valid;
        last_w_ = watts;
    }

    // Average since the last take(); false when there was no valid reading.
    // A single reading with nothing to weight it by still counts.
    bool take(uint32_t now_ms, uint16_t* watts) {
        add(now_ms, last_valid_, last_w_);
        bool ok = false;
        if (ms_ > 0) {
            const uint64_t avg = sum_ / ms_;
            *watts = (uint16_t)(avg > 0xFFFE ? 0xFFFE : avg);
            ok = true;
        } else if (has_last_ && last_valid_) {
            *watts = (uint16_t)(last_w_ > 0xFFFE ? 0xFFFE : last_w_);
            ok = true;
        }
        sum_ = 0;
        ms_ = 0;
        return ok;
    }

private:
    static constexpr uint32_t kMaxStepMs = 120000;
    bool has_last_ = false;
    bool last_valid_ = false;
    uint32_t last_ms_ = 0;
    uint32_t last_w_ = 0;
    uint64_t sum_ = 0;
    uint64_t ms_ = 0;
};

}  // namespace energy
