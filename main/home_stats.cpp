#include "home_stats.h"

#include <string.h>

static constexpr uint32_t GAP_SECONDS = 3 * HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC;

static bool is_running(const history_telemetry_sample_t& s) {
    return (s.flags & HISTORY_TELEMETRY_COMPRESSOR_VALID) &&
           (s.flags & HISTORY_TELEMETRY_COMPRESSOR_RUNNING);
}

static void push_run(home_run_t* out, size_t cap, size_t* count,
                     const home_run_t& run) {
    if (cap == 0) return;
    if (*count == cap) {
        memmove(out, out + 1, (cap - 1) * sizeof(*out));
        (*count)--;
    }
    out[(*count)++] = run;
}

size_t home_stats_runs(const history_telemetry_sample_t* samples, size_t n,
                       uint32_t now, home_run_t* out, size_t cap,
                       home_run_summary_t* summary) {
    home_run_summary_t sum = {};
    size_t count = 0;
    home_run_t open = {};
    bool is_open = false;
    const history_telemetry_sample_t* prev = nullptr;

    for (size_t i = 0; i < n; i++) {
        const history_telemetry_sample_t& s = samples[i];
        const bool gap = prev && s.timestamp - prev->timestamp > GAP_SECONDS;
        const bool running = is_running(s);

        if (s.flags & HISTORY_TELEMETRY_SETPOINT_VALID) {
            sum.setpoint_valid = true;
            sum.setpoint_deci_c = s.setpoint_deci_c;
        }

        if (is_open && (gap || !running)) {
            open.end = gap ? prev->timestamp + HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC
                           : s.timestamp;
            push_run(out, cap, &count, open);
            sum.last = open;
            is_open = false;
        }
        if (running && !is_open) {
            open = {};
            open.start = s.timestamp;
            open.mode = s.mode;
            open.seen_start = prev && !gap && !is_running(*prev);
            if (open.seen_start && open.start + 3600 >= now) sum.starts_last_hour++;
            is_open = true;
        } else if (running && open.mode == HISTORY_TELEMETRY_MODE_UNKNOWN) {
            open.mode = s.mode;
        }
        prev = &s;
    }

    if (is_open) {
        if (now >= prev->timestamp && now - prev->timestamp <= GAP_SECONDS) {
            open.end = 0;
            sum.current = open;
        } else {
            open.end = prev->timestamp + HISTORY_TELEMETRY_SAMPLE_INTERVAL_SEC;
            sum.last = open;
        }
        push_run(out, cap, &count, open);
    }

    if (summary) *summary = sum;
    return count;
}

void home_stats_tank_series(const history_telemetry_sample_t* samples, size_t n,
                            uint32_t start, uint32_t end, int16_t* deci_c,
                            bool* valid, size_t buckets) {
    if (buckets == 0) return;
    if (buckets > HOME_STATS_MAX_BUCKETS) buckets = HOME_STATS_MAX_BUCKETS;
    for (size_t b = 0; b < buckets; b++) {
        deci_c[b] = 0;
        valid[b] = false;
    }
    if (end <= start) return;

    int32_t sums[HOME_STATS_MAX_BUCKETS];
    uint16_t counts[HOME_STATS_MAX_BUCKETS];
    memset(sums, 0, sizeof(sums));
    memset(counts, 0, sizeof(counts));

    const uint64_t span = end - start;
    for (size_t i = 0; i < n; i++) {
        const history_telemetry_sample_t& s = samples[i];
        if (!(s.flags & HISTORY_TELEMETRY_TANK_VALID)) continue;
        if (s.timestamp < start || s.timestamp >= end) continue;
        const size_t b = (size_t)((uint64_t)(s.timestamp - start) * buckets / span);
        sums[b] += s.tank_deci_c;
        counts[b]++;
    }
    for (size_t b = 0; b < buckets; b++) {
        if (counts[b] == 0) continue;
        deci_c[b] = (int16_t)(sums[b] / counts[b]);
        valid[b] = true;
    }
}
