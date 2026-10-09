#pragma once

#include "lvgl.h"

/**
 * Mark a fault period on a chart plot: a solid full-height line where it
 * starts (visible however short the fault), plus diagonal hatching once the
 * period is wide enough to show it. Draw it over the run bands and under the
 * data lines.
 */
static inline void chart_draw_fault_zone(lv_layer_t* layer, int32_t x1,
                                         int32_t x2, int32_t y1, int32_t y2,
                                         lv_color_t color) {
    static constexpr int32_t MIN_HATCH_W = 8;
    static constexpr int32_t HATCH_STEP = 12;

    if (x2 - x1 >= MIN_HATCH_W) {
        lv_draw_rect_dsc_t fill;
        lv_draw_rect_dsc_init(&fill);
        fill.bg_color = color;
        fill.bg_opa = LV_OPA_10;
        const lv_area_t area = {x1, y1, x2, y2};
        lv_draw_rect(layer, &fill, &area);

        // Lines x + y = c, clipped to the zone.
        lv_draw_line_dsc_t hatch;
        lv_draw_line_dsc_init(&hatch);
        hatch.color = color;
        hatch.width = 2;
        hatch.opa = LV_OPA_40;
        for (int32_t c = x1 + y1 + HATCH_STEP / 2; c < x2 + y2; c += HATCH_STEP) {
            const int32_t xa = LV_MAX(x1, c - y2);
            const int32_t xb = LV_MIN(x2, c - y1);
            if (xa >= xb) continue;
            hatch.p1.x = xa;
            hatch.p1.y = c - xa;
            hatch.p2.x = xb;
            hatch.p2.y = c - xb;
            lv_draw_line(layer, &hatch);
        }
    }

    lv_draw_line_dsc_t edge;
    lv_draw_line_dsc_init(&edge);
    edge.color = color;
    edge.width = 2;
    edge.opa = LV_OPA_70;
    edge.p1.x = x1;
    edge.p1.y = y1;
    edge.p2.x = x1;
    edge.p2.y = y2;
    lv_draw_line(layer, &edge);
}
