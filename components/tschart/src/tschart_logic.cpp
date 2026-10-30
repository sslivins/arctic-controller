#include "tschart_logic.h"

namespace tschart {

static int64_t floor_to(int64_t v, int64_t step) {
    const int64_t q = v / step;
    return (v % step != 0 && v < 0 ? q - 1 : q) * step;
}

static int64_t ceil_to(int64_t v, int64_t step) {
    const int64_t q = v / step;
    return (v % step != 0 && v > 0 ? q + 1 : q) * step;
}

static int32_t clamp32(int64_t v) {
    if (v > INT32_MAX) return INT32_MAX;
    if (v < INT32_MIN + 1) return INT32_MIN + 1;
    return (int32_t)v;
}

Axis nice_axis(int32_t lo, int32_t hi, int32_t max_steps, int32_t min_step) {
    if (hi < lo) {
        const int32_t t = lo;
        lo = hi;
        hi = t;
    }
    if (max_steps < 1) max_steps = 1;
    if (min_step < 1) min_step = 1;
    static constexpr int64_t MULT[] = {1, 2, 5};
    int64_t decade = min_step;
    Axis axis = {lo, hi, min_step};
    for (int guard = 0; guard < 30; guard++) {
        for (int64_t m : MULT) {
            const int64_t step = decade * m;
            int64_t a = floor_to(lo, step);
            int64_t b = ceil_to(hi, step);
            if (a == b) b += step;
            axis = {clamp32(a), clamp32(b), clamp32(step)};
            if ((b - a) / step <= max_steps) return axis;
        }
        decade *= 10;
        if (decade > INT32_MAX / 5) break;
    }
    return axis;
}

Axis auto_range(const RangeCfg& cfg, int32_t data_lo, int32_t data_hi,
                int32_t max_steps) {
    const bool have_data = data_lo <= data_hi;
    if (!cfg.auto_range) {
        int64_t lo = cfg.has_min ? cfg.min : (have_data ? data_lo : 0);
        int64_t hi = cfg.has_max ? cfg.max : (have_data ? data_hi : lo + 1);
        if (hi <= lo) hi = lo + 1;
        Axis step = nice_axis((int32_t)lo, (int32_t)hi, max_steps, cfg.min_step);
        // Fixed bounds stay exactly as configured; only the step is niced.
        return {cfg.has_min ? cfg.min : step.lo, cfg.has_max ? cfg.max : step.hi,
                step.step};
    }

    int64_t lo, hi;
    if (have_data) {
        lo = data_lo;
        hi = data_hi;
    } else {
        lo = 0;
        hi = cfg.min_span > 0 ? cfg.min_span : (cfg.min_step > 0 ? cfg.min_step : 1);
    }
    if (cfg.include_zero) {
        if (lo > 0) lo = 0;
        if (hi < 0) hi = 0;
    }
    auto padding = [&](int64_t span) {
        const int64_t p = span * cfg.pad_permille / 1000;
        return p < cfg.min_pad ? (int64_t)cfg.min_pad : p;
    };
    if (cfg.include_zero && (lo == 0 || hi == 0)) {
        // On a zero baseline: headroom on the far side only, then the
        // minimum span (so an idle trace gets the minimum, not min + pad).
        const int64_t pad = padding(hi - lo);
        if (lo == 0) {
            hi += pad;
            if (hi < cfg.min_span) hi = cfg.min_span;
        } else {
            lo -= pad;
            if (lo > -cfg.min_span) lo = -cfg.min_span;
        }
    } else {
        if (hi - lo < cfg.min_span) {
            const int64_t c = (lo + hi) / 2;
            lo = c - cfg.min_span / 2;
            hi = lo + cfg.min_span;
        }
        const int64_t pad = padding(hi - lo);
        lo -= pad;
        hi += pad;
    }
    if (cfg.has_min && lo > cfg.min) lo = cfg.min;
    if (cfg.has_max && hi < cfg.max) hi = cfg.max;
    return nice_axis(clamp32(lo), clamp32(hi), max_steps, cfg.min_step);
}

int32_t map_x(uint32_t t, uint32_t start, uint32_t end, int32_t x1, int32_t x2) {
    if (end <= start || t <= start) return x1;
    if (t >= end) return x2;
    return x1 + (int32_t)(((uint64_t)(t - start) * (uint32_t)(x2 - x1)) / (end - start));
}

int32_t map_y(int32_t v, int32_t lo, int32_t hi, int32_t y1, int32_t y2) {
    if (hi <= lo) return y2;
    return y2 - (int32_t)(((int64_t)v - lo) * (y2 - y1) / ((int64_t)hi - lo));
}

uint32_t time_at(int32_t x, uint32_t start, uint32_t end, int32_t x1, int32_t x2) {
    if (x <= x1 || x2 <= x1 || end <= start) return start;
    if (x >= x2) return end;
    return start + (uint32_t)((uint64_t)(x - x1) * (end - start) / (uint32_t)(x2 - x1));
}

void interpolate_plateaus(const uint32_t* t, const int32_t* v, size_t n,
                          uint32_t gap_seconds, int32_t* out) {
    for (size_t k = 0; k < n; k++) out[k] = NONE;
    size_t a = 0;
    while (a < n) {
        if (v[a] == NONE) { a++; continue; }
        size_t b = a;
        while (b + 1 < n && v[b + 1] != NONE && t[b + 1] - t[b] <= gap_seconds) b++;
        // Anchors: segment start, each plateau midpoint, segment end. Times
        // are doubled so plateau midpoints stay integral.
        int64_t t0 = 2 * (int64_t)t[a];
        int32_t v0 = v[a];
        size_t k = a;
        auto emit_to = [&](int64_t t1, int32_t v1) {
            while (k <= b && 2 * (int64_t)t[k] <= t1) {
                const int64_t tk = 2 * (int64_t)t[k];
                out[k] = (t1 > t0)
                    ? v0 + (int32_t)(((int64_t)(v1 - v0) * (tk - t0)) / (t1 - t0))
                    : v1;
                k++;
            }
            t0 = t1;
            v0 = v1;
        };
        for (size_t i = a; i <= b;) {
            size_t j = i;
            while (j + 1 <= b && v[j + 1] == v[i]) j++;
            emit_to((int64_t)t[i] + t[j], v[i]);
            i = j + 1;
        }
        emit_to(2 * (int64_t)t[b], v[b]);
        a = b + 1;
    }
}

size_t decimate(const int32_t* xs, const uint32_t* t, const int32_t* v,
                size_t n, uint32_t gap_seconds, Point* out) {
    size_t w = 0;
    bool in_seg = false;
    size_t i = 0;
    while (i < n) {
        if (v[i] == NONE) { in_seg = false; i++; continue; }
        if (in_seg && t[i] - t[i - 1] > gap_seconds) in_seg = false;
        const bool brk = !in_seg;
        // One pixel column of this segment: [i, j).
        size_t j = i + 1;
        size_t lo = i, hi = i;
        while (j < n && xs[j] == xs[i] && v[j] != NONE &&
               t[j] - t[j - 1] <= gap_seconds) {
            if (v[j] < v[lo]) lo = j;
            if (v[j] > v[hi]) hi = j;
            j++;
        }
        const size_t last = j - 1;
        size_t pick[4] = {i, lo < hi ? lo : hi, lo < hi ? hi : lo, last};
        size_t prev = (size_t)-1;
        bool first = true;
        for (size_t p : pick) {
            if (p == prev || (prev != (size_t)-1 && p < prev)) continue;
            out[w++] = {xs[p], v[p], brk && first};
            first = false;
            prev = p;
        }
        in_seg = true;
        i = j;
    }
    return w;
}

void split_panes(int32_t y1, int32_t y2, int32_t gap, const uint16_t* weights,
                 const uint16_t* min_heights, const bool* visible, size_t n,
                 PaneSlot* out) {
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        out[i] = {y1, y1 - 1};
        if (visible[i]) k++;
    }
    if (k == 0) return;
    const int32_t avail = (y2 - y1 + 1) - gap * (int32_t)(k - 1);
    if (avail <= 0) return;

    // Panes whose weighted share is under their minimum get the minimum, the
    // rest share what's left by weight; repeat until stable.
    static constexpr size_t MAX = 16;
    bool fixed[MAX] = {};
    int32_t h[MAX] = {};
    for (int pass = 0; pass < (int)MAX; pass++) {
        int32_t left = avail;
        uint32_t wsum = 0;
        for (size_t i = 0; i < n && i < MAX; i++) {
            if (!visible[i]) continue;
            if (fixed[i]) left -= min_heights[i];
            else wsum += weights[i] ? weights[i] : 1;
        }
        bool changed = false;
        for (size_t i = 0; i < n && i < MAX; i++) {
            if (!visible[i] || fixed[i]) continue;
            const uint32_t wt = weights[i] ? weights[i] : 1;
            h[i] = wsum ? (int32_t)((int64_t)(left > 0 ? left : 0) * wt / wsum) : 0;
            if (h[i] < min_heights[i] && left > 0) {
                int32_t fixed_sum = 0;
                for (size_t j = 0; j < n && j < MAX; j++) {
                    if (visible[j] && (fixed[j] || j == i)) fixed_sum += min_heights[j];
                }
                if (fixed_sum <= avail) {
                    fixed[i] = true;
                    changed = true;
                }
            }
        }
        if (!changed) break;
    }
    int32_t y = y1;
    size_t seen = 0;
    for (size_t i = 0; i < n && i < MAX; i++) {
        if (!visible[i]) continue;
        seen++;
        int32_t hi = fixed[i] ? min_heights[i] : h[i];
        // The last visible pane absorbs rounding.
        if (seen == k) hi = y2 - y + 1;
        out[i] = {y, y + hi - 1};
        y += hi + gap;
    }
}

int32_t ticks_for_height(int32_t height, int32_t max_ticks, int32_t min_px) {
    int32_t t = max_ticks > 0 ? max_ticks : 1;
    if (min_px > 0 && height / min_px < t) t = height / min_px;
    return t < 1 ? 1 : t;
}

void place_tags(int32_t* y, size_t n, int32_t h, int32_t gap, int32_t top,
                int32_t bottom, size_t* order) {
    if (n == 0) return;
    for (size_t i = 0; i < n; i++) order[i] = i;
    for (size_t i = 1; i < n; i++) {
        const size_t o = order[i];
        size_t j = i;
        while (j > 0 && y[order[j - 1]] > y[o]) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = o;
    }
    const int32_t pitch = h + gap;
    const int32_t ymin = top + h / 2;
    const int32_t ymax = bottom - (h - h / 2);
    for (size_t i = 0; i < n; i++) {
        int32_t& v = y[order[i]];
        if (v < ymin) v = ymin;
        if (i > 0 && v < y[order[i - 1]] + pitch) v = y[order[i - 1]] + pitch;
    }
    for (size_t i = n; i-- > 0;) {
        int32_t& v = y[order[i]];
        if (v > ymax) v = ymax;
        if (i + 1 < n && v > y[order[i + 1]] - pitch) v = y[order[i + 1]] - pitch;
    }
    for (size_t i = 0; i < n; i++) {
        int32_t& v = y[order[i]];
        if (v < ymin) v = ymin;
        if (i > 0 && v < y[order[i - 1]] + pitch) v = y[order[i - 1]] + pitch;
    }
}

PanLimits pan_limits(uint32_t start, uint32_t end, uint32_t earliest,
                     uint32_t latest, int32_t width) {
    const uint32_t window = end - start;
    if (window == 0 || width <= 0) return {0, 0};
    const uint32_t fwd = latest > end ? latest - end : 0;
    const uint32_t back = start > earliest ? start - earliest : 0;
    return {-(int32_t)((int64_t)fwd * width / window),
            (int32_t)((int64_t)back * width / window)};
}

int32_t rubber_band(int32_t dx, const PanLimits& lim) {
    if (dx > lim.max_dx) return lim.max_dx + (dx - lim.max_dx) / 3;
    if (dx < lim.min_dx) return lim.min_dx + (dx - lim.min_dx) / 3;
    return dx;
}

bool pan_target(int32_t dx, uint32_t start, uint32_t end, uint32_t earliest,
                uint32_t latest, int32_t width, uint32_t snap, uint32_t* new_end) {
    const uint32_t window = end - start;
    if (window == 0 || width <= 0) return false;
    const PanLimits lim = pan_limits(start, end, earliest, latest, width);
    if (dx < lim.min_dx) dx = lim.min_dx;
    if (dx > lim.max_dx) dx = lim.max_dx;
    if (dx == 0) return false;
    const int64_t secs = -(int64_t)dx * window / width;
    const int64_t s = snap > 0 ? snap : 1;
    const int64_t steps = (secs >= 0 ? secs + s / 2 : secs - s / 2) / s;
    if (steps == 0) return false;
    int64_t e = (int64_t)end + steps * s;
    if (e > (int64_t)latest) e = latest;
    const int64_t floor_end = (int64_t)earliest + window;
    if (e < floor_end) e = floor_end;
    if (e < (int64_t)window) e = window;
    if (e == (int64_t)end) return false;
    *new_end = (uint32_t)e;
    return true;
}

int32_t uncovered_permille(uint32_t from_start, uint32_t from_end,
                           uint32_t to_start, uint32_t to_end) {
    const int64_t span = (int64_t)to_end - to_start;
    if (span <= 0 || (int64_t)from_end - from_start != span) return 500;
    const int64_t shift = (int64_t)to_end - from_end;
    if (shift == 0 || shift >= span || -shift >= span) return 500;
    if (shift < 0) return (int32_t)(-shift * 500 / span);
    return (int32_t)(1000 - shift * 500 / span);
}

uint32_t time_tick_step(uint32_t window, uint32_t max_intervals) {
    static constexpr uint32_t STEPS[] = {900, 1800, 3600, 7200, 10800,
                                         21600, 43200, 86400};
    if (max_intervals == 0) max_intervals = 1;
    for (uint32_t s : STEPS) {
        if (window / s <= max_intervals) return s;
    }
    return STEPS[sizeof(STEPS) / sizeof(STEPS[0]) - 1];
}

uint32_t first_tick(uint32_t start, uint32_t step, uint32_t since_midnight) {
    if (step == 0) return start;
    return start + (step - since_midnight % step) % step;
}

int32_t nearest_x(const int32_t* xs, const bool* ok, size_t n, int32_t px,
                  int32_t max_dx) {
    int32_t best = -1;
    int32_t best_dx = INT32_MAX;
    for (size_t i = 0; i < n; i++) {
        if (ok && !ok[i]) continue;
        const int32_t d = xs[i] > px ? xs[i] - px : px - xs[i];
        if (d < best_dx) {
            best_dx = d;
            best = (int32_t)i;
        }
    }
    return best_dx <= max_dx ? best : -1;
}

int32_t nearest_time(const uint32_t* t, size_t n, uint32_t at, uint32_t max_dt) {
    if (n == 0) return -1;
    size_t lo = 0, hi = n;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (t[mid] < at) lo = mid + 1;
        else hi = mid;
    }
    int32_t best = -1;
    uint32_t best_dt = UINT32_MAX;
    if (lo < n) {
        best = (int32_t)lo;
        best_dt = t[lo] - at;
    }
    if (lo > 0 && at - t[lo - 1] <= best_dt) {
        best = (int32_t)(lo - 1);
        best_dt = at - t[lo - 1];
    }
    return best_dt <= max_dt ? best : -1;
}

size_t group_notes(const int32_t* xs, size_t n, int32_t label_w, size_t* group) {
    size_t count = 0;
    size_t i = 0;
    while (i < n) {
        size_t j = i + 1;
        while (j < n && xs[j] < xs[i] + label_w) j++;
        for (size_t k = i; k < j; k++) group[k] = i;
        count++;
        i = j;
    }
    return count;
}

}  // namespace tschart
