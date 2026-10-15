#pragma once

#include <cstdint>

// A temperature axis whose gridlines fall on whole, round degrees.
//
// All values are tenths of the display unit (°C or °F), so the steps are
// 1, 2, 5, 10, 20, 50 or 100 degrees. Splitting an arbitrary range into a
// fixed number of parts instead gives labels like 4.6°, 14.4°, 24.1°.
struct ChartAxis {
    int32_t lo;
    int32_t hi;
    int32_t step;
};

// Smallest round step that covers [lo, hi] in at most max_steps gridline
// intervals, with lo and hi widened outward to multiples of it.
ChartAxis chart_axis_nice(int32_t lo, int32_t hi, int32_t max_steps = 6);
