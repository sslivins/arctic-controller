/*
 * tschart -- pure (LVGL-free) chart logic, unit-tested natively.
 *
 * Everything here works on plain arrays and integers: axis nicing,
 * auto-range, pixel mapping, plateau interpolation, per-column decimation,
 * pane layout, end-tag placement, pan/snap math and cursor lookup.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace tschart {

static constexpr int32_t NONE = INT32_MIN;

// ---- Axis -----------------------------------------------------------------

struct Axis {
    int32_t lo;
    int32_t hi;
    int32_t step;
};

// Smallest step of the form min_step * {1, 2, 5} * 10^k that covers [lo, hi]
// in at most max_steps intervals, with lo/hi widened outward to multiples of
// it. min_step <= 0 is treated as 1.
Axis nice_axis(int32_t lo, int32_t hi, int32_t max_steps, int32_t min_step);

struct RangeCfg {
    bool auto_range;
    bool has_min, has_max;   // fixed bounds; in auto mode, hard limits
    int32_t min, max;
    int32_t min_span;        // auto: never tighter than this
    bool include_zero;
    int32_t pad_permille;    // auto: padding each side, of the span
    int32_t min_pad;         // auto: at least this much padding
    int32_t min_step;        // smallest tick step (raw units)
};

// Niced axis for a pane from the data extent (data_lo > data_hi = no data),
// with at most max_steps intervals.
Axis auto_range(const RangeCfg& cfg, int32_t data_lo, int32_t data_hi,
                int32_t max_steps);

// ---- Mapping --------------------------------------------------------------

int32_t map_x(uint32_t t, uint32_t start, uint32_t end, int32_t x1, int32_t x2);
int32_t map_y(int32_t v, int32_t lo, int32_t hi, int32_t y1, int32_t y2);
// Inverse of map_x: time under pixel x (clamped to the window).
uint32_t time_at(int32_t x, uint32_t start, uint32_t end, int32_t x1, int32_t x2);

// ---- Series ---------------------------------------------------------------

// Whole-unit readings staircase. Each run of identical values is a plateau
// the true value crossed somewhere inside, so the line is drawn through each
// plateau's midpoint. Plateau values are kept exactly; NONE or a time gap
// larger than gap_seconds breaks the line.
void interpolate_plateaus(const uint32_t* t, const int32_t* v, size_t n,
                          uint32_t gap_seconds, int32_t* out);

// Decimated points for drawing. A point with brk set starts a new segment.
struct Point {
    int32_t x;
    int32_t v;
    bool brk;
};

// Reduces a series to at most 4 points per pixel column (first, min, max,
// last in time order) without bridging gaps. xs are pixel columns (ascending),
// v uses NONE for missing values, a time gap larger than gap_seconds breaks
// the line. Returns the number of points written (at most n).
size_t decimate(const int32_t* xs, const uint32_t* t, const int32_t* v,
                size_t n, uint32_t gap_seconds, Point* out);

// ---- Layout ---------------------------------------------------------------

struct PaneSlot {
    int32_t y1, y2;   // inclusive; y2 < y1 when hidden
};

// Splits [y1, y2] between the visible panes by weight with gap px between
// them. min_height is honoured when it fits.
void split_panes(int32_t y1, int32_t y2, int32_t gap, const uint16_t* weights,
                 const uint16_t* min_heights, const bool* visible, size_t n,
                 PaneSlot* out);

// Tick budget for a pane of the given height: max_ticks, but no more than
// one interval per min_px (at least 1).
int32_t ticks_for_height(int32_t height, int32_t max_ticks, int32_t min_px);

// ---- End tags -------------------------------------------------------------

// Places tags (desired centre y, height h) so they don't overlap: sorted by
// y, pushed down, shifted up from the bottom, clamped to the top. y is
// updated in place. order receives the indices sorted by desired y.
void place_tags(int32_t* y, size_t n, int32_t h, int32_t gap, int32_t top,
                int32_t bottom, size_t* order);

// ---- Pan ------------------------------------------------------------------

struct PanLimits {
    int32_t min_dx;   // most negative (towards latest)
    int32_t max_dx;   // most positive (towards earliest)
};

// Drag offsets in px that keep the window inside [earliest, latest].
PanLimits pan_limits(uint32_t start, uint32_t end, uint32_t earliest,
                     uint32_t latest, int32_t width);

// Past either end the chart follows the finger at a third of the speed.
int32_t rubber_band(int32_t dx, const PanLimits& lim);

// Window end after a release with offset dx (px). Moves in whole snaps,
// clamped to latest and to earliest + window. False when the drag rounds to
// no move.
bool pan_target(int32_t dx, uint32_t start, uint32_t end, uint32_t earliest,
                uint32_t latest, int32_t width, uint32_t snap, uint32_t* new_end);

// Spinner position (permille across the plot) for loading `to` after
// showing `from`: the middle of the strip the move uncovers, or 500.
int32_t uncovered_permille(uint32_t from_start, uint32_t from_end,
                           uint32_t to_start, uint32_t to_end);

// ---- Time axis ------------------------------------------------------------

// Smallest tick step from 15 min .. 1 day giving at most max_intervals.
uint32_t time_tick_step(uint32_t window, uint32_t max_intervals);
// First tick at or after start, aligned to local midnight given start's
// seconds since local midnight.
uint32_t first_tick(uint32_t start, uint32_t step, uint32_t since_midnight);

// ---- Cursor ---------------------------------------------------------------

// Index of the entry with ok[i] whose xs[i] is nearest px, within max_dx;
// -1 if none.
int32_t nearest_x(const int32_t* xs, const bool* ok, size_t n, int32_t px,
                  int32_t max_dx);

// Index of the time nearest `at` within max_dt (t ascending); -1 if none.
int32_t nearest_time(const uint32_t* t, size_t n, uint32_t at, uint32_t max_dt);

// ---- Notes ----------------------------------------------------------------

// Groups notes (xs ascending) whose x falls within label_w of the group's
// first note. Writes the group's first index for every note; returns the
// number of groups.
size_t group_notes(const int32_t* xs, size_t n, int32_t label_w, size_t* group);

}  // namespace tschart
