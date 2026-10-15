#include "chart_axis.h"

static int32_t floor_to(int32_t v, int32_t step) {
    const int32_t q = v / step;
    return (v % step != 0 && v < 0 ? q - 1 : q) * step;
}

static int32_t ceil_to(int32_t v, int32_t step) {
    const int32_t q = v / step;
    return (v % step != 0 && v > 0 ? q + 1 : q) * step;
}

ChartAxis chart_axis_nice(int32_t lo, int32_t hi, int32_t max_steps) {
    static constexpr int32_t STEPS[] = {10, 20, 50, 100, 200, 500, 1000};
    if (hi < lo) {
        const int32_t t = lo;
        lo = hi;
        hi = t;
    }
    if (max_steps < 1) max_steps = 1;
    ChartAxis axis = {lo, hi, STEPS[0]};
    for (int32_t step : STEPS) {
        axis = {floor_to(lo, step), ceil_to(hi, step), step};
        if (axis.hi == axis.lo) axis.hi += step;
        if ((axis.hi - axis.lo) / step <= max_steps) break;
    }
    return axis;
}
