/*
 * tschart rendering: the cached static image (spans, grid, series, time
 * axis, notes) and the dynamic layer drawn over it every frame (y-axis
 * gutter, end tags, edge hatching while panning, cursor and card).
 */
#include "tschart_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_timer.h"
static int64_t now_us() { return esp_timer_get_time(); }
#else
static int64_t now_us() { return (int64_t)lv_tick_get() * 1000; }
#endif

namespace tschart {

namespace {

const lv_color_t CARD_BG = lv_color_hex(0x1a1a2e);

struct Geo {
    lv_area_t coords;
    lv_area_t plot;
    PaneSlot slot[TSCHART_MAX_PANES];
};

Geo make_geo(lv_obj_t *c, int32_t ox, int32_t oy) {
    Impl *d = impl(c);
    Geo g;
    lv_obj_get_coords(c, &g.coords);
    g.plot = d->plot;
    lv_area_move(&g.coords, ox, oy);
    lv_area_move(&g.plot, ox, oy);
    for (uint8_t i = 0; i < d->npanes; i++) {
        g.slot[i] = {d->panes[i].slot.y1 + oy, d->panes[i].slot.y2 + oy};
    }
    return g;
}

const lv_font_t *main_font(lv_obj_t *c) {
    return lv_obj_get_style_text_font(c, LV_PART_MAIN);
}

void line(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
          lv_color_t color, int32_t width, lv_opa_t opa = LV_OPA_COVER) {
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width;
    dsc.opa = opa;
    dsc.round_start = width > 2;
    dsc.round_end = width > 2;
    dsc.p1.x = x1;
    dsc.p1.y = y1;
    dsc.p2.x = x2;
    dsc.p2.y = y2;
    lv_draw_line(layer, &dsc);
}

void rect(lv_layer_t *layer, const lv_area_t &a, lv_color_t color, lv_opa_t opa,
          int32_t radius = 0) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = opa;
    dsc.radius = radius;
    lv_draw_rect(layer, &dsc, &a);
}

void text(lv_layer_t *layer, const lv_area_t &a, const char *s, lv_color_t color,
          lv_text_align_t align, const lv_font_t *font) {
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.text = s;
    dsc.text_local = 1;  // drawn after s goes out of scope
    dsc.color = color;
    dsc.align = align;
    dsc.font = font;
    lv_draw_label(layer, &dsc, &a);
}

int32_t text_w(const char *s, const lv_font_t *font) {
    lv_point_t size;
    lv_text_get_size(&size, s, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

void dot(lv_layer_t *layer, int32_t x, int32_t y, lv_color_t color, lv_color_t ring) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.radius = LV_RADIUS_CIRCLE;
    dsc.border_color = ring;
    dsc.border_width = 2;
    const lv_area_t a = {x - 7, y - 7, x + 7, y + 7};
    lv_draw_rect(layer, &dsc, &a);
}

// Diagonal lines x + y = c inside [x1, x2] x [y1, y2], phase anchored at c0.
void hatch(lv_layer_t *layer, int32_t x1, int32_t x2, int32_t y1, int32_t y2,
           int32_t c0, lv_color_t color, lv_opa_t opa) {
    static constexpr int32_t STEP = 12;
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = 2;
    dsc.opa = opa;
    int32_t c = c0 - ((c0 - (x1 + y1)) / STEP + 1) * STEP;
    for (; c < x2 + y2; c += STEP) {
        const int32_t xa = LV_MAX(x1, c - y2);
        const int32_t xb = LV_MIN(x2, c - y1);
        if (xa >= xb) continue;
        dsc.p1.x = xa;
        dsc.p1.y = c - xa;
        dsc.p2.x = xb;
        dsc.p2.y = c - xb;
        lv_draw_line(layer, &dsc);
    }
}

// Fault period: a solid line where it starts (visible however short), plus
// hatching once it's wide enough to show.
void fault_zone(lv_layer_t *layer, int32_t x1, int32_t x2, int32_t y1, int32_t y2,
                lv_color_t color, bool start_line) {
    if (x2 - x1 >= 8) {
        rect(layer, {x1, y1, x2, y2}, color, LV_OPA_10);
        hatch(layer, x1, x2, y1, y2, x1 + y1 + 6, color, LV_OPA_40);
    }
    if (start_line) line(layer, x1, y1, x1, y2, color, 2, LV_OPA_70);
}

size_t fmt_time(Impl *d, uint32_t t, uint32_t step, char *buf, size_t cap) {
    if (d->time_fmt) return d->time_fmt(t, step, buf, cap, d->time_fmt_ud);
    const time_t tt = t;
    struct tm tm = {};
    localtime_r(&tt, &tm);
    return strftime(buf, cap, "%H:%M", &tm);
}

// ---- Static image ------------------------------------------------------------

void draw_spans(lv_obj_t *c, lv_layer_t *layer, const Geo &g) {
    Impl *d = impl(c);
    const lv_area_t &P = g.plot;
    for (const SpanSet &set : d->spans) {
        for (size_t i = 0; i < set.count; i++) {
            const tschart_span_t &sp = set.spans[i];
            const uint32_t end = sp.end ? sp.end : d->end;
            if (sp.start > d->end || end < d->start) continue;
            int32_t x1 = map_x(LV_MAX(sp.start, d->start), d->start, d->end, P.x1, P.x2);
            int32_t x2 = map_x(LV_MIN(end, d->end), d->start, d->end, P.x1, P.x2);
            if (x2 - x1 < sp.min_width) x2 = x1 + sp.min_width;
            x2 = LV_MIN(x2, P.x2);
            if (x2 < x1) continue;
            if (sp.style == TSCHART_SPAN_HATCH) {
                fault_zone(layer, x1, x2, P.y1, P.y2, sp.color, sp.start >= d->start);
            } else {
                rect(layer, {x1, P.y1, x2, P.y2}, sp.color, sp.opa);
            }
        }
    }
}

void draw_time_axis(lv_obj_t *c, lv_layer_t *layer, const Geo &g) {
    Impl *d = impl(c);
    const lv_area_t &P = g.plot;
    const uint32_t window = d->end - d->start;
    const uint32_t step =
        time_tick_step(window, (uint32_t)LV_MAX(1, lv_area_get_width(&P) / 150));
    const time_t st = d->start;
    struct tm tm = {};
    localtime_r(&st, &tm);
    const uint32_t since_midnight =
        (uint32_t)(tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec);
    for (uint32_t t = first_tick(d->start, step, since_midnight);
         t <= d->end && t >= d->start; t += step) {
        const int32_t x = map_x(t, d->start, d->end, P.x1, P.x2);
        line(layer, x, P.y1, x, P.y2, col_grid(c), 1);
        char label[24];
        fmt_time(d, t, step, label, sizeof(label));
        text(layer, {x - 60, P.y2 + 8, x + 60, g.coords.y2}, label, col_dim(),
             LV_TEXT_ALIGN_CENTER, d->small_font);
    }
}

void draw_grid(lv_obj_t *c, lv_layer_t *layer, const Geo &g) {
    Impl *d = impl(c);
    for (uint8_t i = 0; i < d->npanes; i++) {
        const tschart_pane &p = d->panes[i];
        if (!p.visible || p.axis.step <= 0) continue;
        for (int32_t v = p.axis.lo; v <= p.axis.hi; v += p.axis.step) {
            const int32_t y = map_y(v, p.axis.lo, p.axis.hi, g.slot[i].y1, g.slot[i].y2);
            line(layer, g.plot.x1, y, g.plot.x2, y, col_grid(c), 1);
        }
    }
}

void draw_series(lv_obj_t *c, lv_layer_t *layer, const Geo &g) {
    Impl *d = impl(c);
    size_t max_n = 0;
    for (uint8_t i = 0; i < d->nlayers; i++) max_n = LV_MAX(max_n, d->layers[i].count);
    if (max_n == 0) return;
    int32_t *xs = (int32_t *)lv_malloc(max_n * sizeof(int32_t));
    Point *pts = (Point *)lv_malloc(max_n * sizeof(Point));
    if (!xs || !pts) {
        lv_free(xs);
        lv_free(pts);
        return;
    }
    const lv_area_t saved = layer->_clip_area;
    const lv_area_t &P = g.plot;
    for (uint8_t i = 0; i < d->nlayers; i++) {
        const tschart_layer &l = d->layers[i];
        const tschart_pane &p = *l.pane;
        if (!l.visible || !p.visible || !l.vals || l.count == 0) continue;
        const PaneSlot &s = g.slot[p.index];
        const lv_area_t clip = {P.x1, s.y1 - l.cfg.width, P.x2, s.y2 + l.cfg.width};
        if (!lv_area_intersect(&layer->_clip_area, &saved, &clip)) continue;
        for (size_t k = 0; k < l.count; k++) {
            xs[k] = map_x(l.src->times[k], d->start, d->end, P.x1, P.x2);
        }
        const size_t n = decimate(xs, l.src->times, l.vals, l.count,
                                  l.src->data.gap_seconds, pts);
        auto Y = [&](int32_t v) { return map_y(v, p.axis.lo, p.axis.hi, s.y1, s.y2); };
        if (l.cfg.fill) {
            // An area per pixel column (its tallest point) down to the
            // baseline, joined to the next column only when contiguous.
            const int32_t base = Y(LV_CLAMP(p.axis.lo, 0, p.axis.hi));
            const lv_opa_t opa = l.cfg.fill_opa ? l.cfg.fill_opa : (lv_opa_t)LV_OPA_30;
            bool have = false;
            int32_t col_x = 0, col_top = 0;
            for (size_t k = 0; k < n; k++) {
                const int32_t y = Y(pts[k].v);
                if (have && pts[k].x != col_x) {
                    const int32_t right = pts[k].brk ? col_x : LV_MAX(pts[k].x - 1, col_x);
                    rect(layer, {col_x, LV_MIN(col_top, base), right, LV_MAX(col_top, base)},
                         l.cfg.color, opa);
                    have = false;
                }
                if (!have) {
                    have = true;
                    col_x = pts[k].x;
                    col_top = y;
                } else if (y < col_top) {
                    col_top = y;
                }
            }
            if (have) {
                rect(layer, {col_x, LV_MIN(col_top, base), col_x, LV_MAX(col_top, base)},
                     l.cfg.color, opa);
            }
        }
        for (size_t k = 1; k < n; k++) {
            if (pts[k].brk) continue;
            const int32_t x1 = pts[k - 1].x, y1 = Y(pts[k - 1].v);
            const int32_t x2 = pts[k].x, y2 = Y(pts[k].v);
            if (l.cfg.geom == TSCHART_GEOM_STEP) {
                if (x2 != x1) line(layer, x1, y1, x2, y1, l.cfg.color, l.cfg.width);
                if (y2 != y1) line(layer, x2, y1, x2, y2, l.cfg.color, l.cfg.width);
            } else {
                line(layer, x1, y1, x2, y2, l.cfg.color, l.cfg.width);
            }
        }
        layer->_clip_area = saved;
    }
    layer->_clip_area = saved;
    lv_free(xs);
    lv_free(pts);
}

// Notes above the plot; ones whose labels would collide share one: the
// text if they're all the same, otherwise the count ("×3").
void draw_notes(lv_obj_t *c, lv_layer_t *layer, const Geo &g) {
    Impl *d = impl(c);
    if (d->nnotes == 0) return;
    const lv_area_t &P = g.plot;
    const int32_t top = P.y1 - NOTE_STRIP_H;
    int32_t *xs = (int32_t *)lv_malloc(d->nnotes * sizeof(int32_t));
    size_t *group = (size_t *)lv_malloc(d->nnotes * sizeof(size_t));
    size_t *idx = (size_t *)lv_malloc(d->nnotes * sizeof(size_t));
    if (!xs || !group || !idx) {
        lv_free(xs);
        lv_free(group);
        lv_free(idx);
        return;
    }
    size_t n = 0;
    for (size_t i = 0; i < d->nnotes; i++) {
        const uint32_t t = d->notes[i].time;
        if (t < d->start || t > d->end) continue;
        xs[n] = map_x(t, d->start, d->end, P.x1, P.x2);
        idx[n++] = i;
    }
    group_notes(xs, n, NOTE_LABEL_W, group);
    for (size_t i = 0; i < n;) {
        size_t j = i + 1;
        bool same = true;
        while (j < n && group[j] == i) {
            if (strcmp(d->notes[idx[j]].text, d->notes[idx[i]].text) != 0) same = false;
            j++;
        }
        const Note &first = d->notes[idx[i]];
        char label[48];
        if (same) {
            snprintf(label, sizeof(label), "%s%s", d->note_prefix, first.text);
        } else {
            snprintf(label, sizeof(label), "%s\xC3\x97%u", d->note_prefix, (unsigned)(j - i));
        }
        int32_t lx = xs[i] + 6;
        if (lx + NOTE_LABEL_W > P.x2) lx = P.x2 - NOTE_LABEL_W;
        text(layer, {lx, top + 4, lx + NOTE_LABEL_W, P.y1 - 4}, label, first.color,
             LV_TEXT_ALIGN_LEFT, main_font(c));
        // Carry each line up into the strip so the label flags it.
        for (size_t k = i; k < j; k++) {
            line(layer, xs[k], top + 8, xs[k], P.y1, d->notes[idx[k]].color, 2);
        }
        i = j;
    }
    lv_free(xs);
    lv_free(group);
    lv_free(idx);
}

void draw_static(lv_obj_t *c, lv_layer_t *layer, const Geo &g) {
    Impl *d = impl(c);
    if (!d->has_ds || d->end <= d->start) return;
    draw_spans(c, layer, g);
    draw_grid(c, layer, g);
    draw_time_axis(c, layer, g);
    draw_series(c, layer, g);
    draw_notes(c, layer, g);
}

CacheKey current_key(lv_obj_t *c, uint32_t start, uint32_t end) {
    lv_area_t coords;
    lv_obj_get_coords(c, &coords);
    return {impl(c)->static_gen, start, end, lv_area_get_width(&coords),
            lv_area_get_height(&coords)};
}

// ---- Dynamic layer -------------------------------------------------------------

// Hatched strip with nothing to show (before `earliest` or after `latest`),
// its boundary line and a label beside it.
void edge_zone(lv_obj_t *c, lv_layer_t *layer, int32_t x1, int32_t x2, int32_t edge_x,
               const char *label, bool label_right) {
    Impl *d = impl(c);
    const lv_area_t &P = d->plot;
    const lv_area_t saved = layer->_clip_area;
    const lv_area_t zone = {x1, P.y1, x2, P.y2};
    if (!lv_area_intersect(&layer->_clip_area, &saved, &zone)) {
        layer->_clip_area = saved;
        return;
    }
    hatch(layer, x1, x2, P.y1, P.y2, edge_x + P.y1, col_grid(c), LV_OPA_70);
    line(layer, edge_x, P.y1, edge_x, P.y2, col_dim(), 2);
    if (label && label[0]) {
        const lv_font_t *font = d->small_font;
        const int32_t w = text_w(label, font);
        const int32_t h = lv_font_get_line_height(font);
        const int32_t lx = label_right ? edge_x + 12 : edge_x - 12 - w;
        const lv_area_t ta = {lx, P.y1 + 10, lx + w, P.y1 + 10 + h};
        rect(layer, {ta.x1 - 4, ta.y1 - 2, ta.x2 + 4, ta.y2 + 2}, col_panel(c),
             LV_OPA_COVER, 4);
        text(layer, ta, label, col_dim(), LV_TEXT_ALIGN_LEFT, font);
    }
    layer->_clip_area = saved;
}

void edge_zones(lv_obj_t *c, lv_layer_t *layer, int32_t dx) {
    Impl *d = impl(c);
    const lv_area_t &P = d->plot;
    const int64_t w = lv_area_get_width(&P);
    const uint32_t start = d->cache_key.start;
    const int64_t span = (int64_t)d->cache_key.end - start;
    if (span <= 0) return;
    auto x_at = [&](uint32_t t) { return P.x1 + ((int64_t)t - start) * w / span + dx; };
    const int64_t xs = x_at(d->earliest);
    const int64_t xn = x_at(d->latest);
    if (xs > P.x1) {
        edge_zone(c, layer, P.x1, (int32_t)LV_MIN(xs, (int64_t)P.x2), (int32_t)xs,
                  d->edge_before, false);
    }
    if (xn < P.x2) {
        edge_zone(c, layer, (int32_t)LV_MAX(xn, (int64_t)P.x1), P.x2, (int32_t)xn,
                  d->edge_after, true);
    }
}

// Open y axis: a gutter over the plot's left edge with each pane's ticks in
// its colour.
void draw_gutter(lv_obj_t *c, lv_layer_t *layer) {
    Impl *d = impl(c);
    const lv_area_t &P = d->plot;
    const int32_t gx = P.x1 + GUTTER_W;
    const int32_t lh = lv_font_get_line_height(d->small_font);
    for (uint8_t i = 0; i < d->npanes; i++) {
        const tschart_pane &p = d->panes[i];
        if (!p.visible || p.axis.step <= 0 || p.slot.y2 <= p.slot.y1) continue;
        rect(layer, {P.x1, p.slot.y1, gx, p.slot.y2}, col_panel(c), LV_OPA_90);
        line(layer, gx, p.slot.y1, gx, p.slot.y2, p.cfg.color, 2);
        for (int32_t v = p.axis.lo; v <= p.axis.hi; v += p.axis.step) {
            const int32_t y = map_y(v, p.axis.lo, p.axis.hi, p.slot.y1, p.slot.y2);
            line(layer, gx - 5, y, gx, y, p.cfg.color, 2);
            char label[24];
            fmt_value(p, v, TSCHART_FMT_AXIS, label, sizeof(label));
            text(layer, {P.x1, y - lh / 2, gx - 8, y + lh / 2}, label, p.cfg.color,
                 LV_TEXT_ALIGN_RIGHT, d->small_font);
        }
    }
}

int32_t last_value(const Impl *d, const tschart_layer &l) {
    for (size_t k = l.count; k-- > 0;) {
        const uint32_t t = l.src->times[k];
        if (t > d->end) continue;
        if (t < d->start) break;
        if (l.vals[k] != NONE) return l.vals[k];
    }
    return NONE;
}

// Each line's latest value at the right edge, kept apart within its pane.
void draw_end_tags(lv_obj_t *c, lv_layer_t *layer) {
    Impl *d = impl(c);
    const lv_area_t &P = d->plot;
    const lv_font_t *font = d->small_font;
    const int32_t h = lv_font_get_line_height(font) + 6;
    for (uint8_t pi = 0; pi < d->npanes; pi++) {
        const tschart_pane &p = d->panes[pi];
        if (!p.visible || p.slot.y2 <= p.slot.y1) continue;
        int32_t ys[TSCHART_MAX_LAYERS], want[TSCHART_MAX_LAYERS];
        const tschart_layer *ls[TSCHART_MAX_LAYERS];
        char labels[TSCHART_MAX_LAYERS][40];
        size_t order[TSCHART_MAX_LAYERS];
        size_t n = 0;
        for (uint8_t i = 0; i < d->nlayers; i++) {
            const tschart_layer &l = d->layers[i];
            if (l.pane != &p || !l.visible || !l.has_tag || !l.vals) continue;
            const int32_t v = last_value(d, l);
            if (v == NONE) continue;
            char value[24];
            fmt_value(p, v, TSCHART_FMT_TAG, value, sizeof(value));
            snprintf(labels[n], sizeof(labels[n]), "%s%s%s", l.tag_name,
                     l.tag_name[0] ? " " : "", value);
            want[n] = ys[n] = map_y(v, p.axis.lo, p.axis.hi, p.slot.y1, p.slot.y2);
            ls[n++] = &l;
        }
        if (n == 0) continue;
        place_tags(ys, n, h, 2, p.slot.y1, p.slot.y2, order);
        for (size_t k = 0; k < n; k++) {
            const int32_t w = text_w(labels[k], font) + 12;
            const lv_area_t box = {P.x2 - 4 - w, ys[k] - h / 2, P.x2 - 4, ys[k] + h / 2};
            if (abs(ys[k] - want[k]) > 4) {
                line(layer, box.x1 - 6, want[k], box.x1, ys[k], ls[k]->cfg.color, 1);
            }
            lv_draw_rect_dsc_t bg;
            lv_draw_rect_dsc_init(&bg);
            bg.bg_color = col_panel(c);
            bg.bg_opa = LV_OPA_90;
            bg.radius = 4;
            bg.border_color = ls[k]->cfg.color;
            bg.border_width = 1;
            lv_draw_rect(layer, &bg, &box);
            text(layer, {box.x1, box.y1 + 3, box.x2, box.y2}, labels[k], ls[k]->cfg.color,
                 LV_TEXT_ALIGN_CENTER, font);
        }
    }
}

// ---- Cursor ----------------------------------------------------------------------

int32_t row_width(const tschart_card_row &r, const lv_font_t *font) {
    const int32_t lw = text_w(r.label, font);
    const int32_t vw = r.value[0] ? text_w(r.value, font) + 16 : 0;
    switch (r.kind) {
        case TSCHART_ROW_BLOCK:
        case TSCHART_ROW_VALUE:
            return 24 + lw + vw;
        default:
            return lw + vw;
    }
}

int32_t cursor_x(Impl *d) {
    const tschart_source *src = default_source(d);
    return map_x(src->times[d->cursor], d->start, d->end, d->plot.x1, d->plot.x2);
}

lv_area_t card_box(lv_obj_t *c) {
    Impl *d = impl(c);
    const lv_area_t &P = d->plot;
    const lv_font_t *font = main_font(c);
    int32_t w = 0;
    for (uint8_t i = 0; i < d->card.count; i++) {
        w = LV_MAX(w, row_width(d->card.rows[i], font));
    }
    w = LV_CLAMP(200, w + 2 * CARD_PAD, lv_area_get_width(&P) * 6 / 10);
    const int32_t rows = d->card.count + (d->card.overflow ? 1 : 0);
    const int32_t h = 6 + rows * CARD_ROW_H + 8;
    const int32_t x = cursor_x(d);
    int32_t bx = x + 16;
    if (bx + w > P.x2) bx = x - 16 - w;
    if (bx < P.x1) bx = P.x1;
    return {bx, P.y1 + 4, bx + w, P.y1 + 4 + h};
}

void draw_card(lv_obj_t *c, lv_layer_t *layer, const lv_area_t &box) {
    Impl *d = impl(c);
    const lv_font_t *font = main_font(c);
    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = CARD_BG;
    bg.bg_opa = LV_OPA_90;
    bg.radius = 10;
    bg.border_color = col_grid(c);
    bg.border_width = 1;
    lv_draw_rect(layer, &bg, &box);
    lv_area_t row = {box.x1 + CARD_PAD, box.y1 + 6, box.x2 - CARD_PAD,
                     box.y1 + 6 + CARD_ROW_H};
    for (uint8_t i = 0; i < d->card.count; i++, row.y1 += CARD_ROW_H, row.y2 += CARD_ROW_H) {
        const tschart_card_row &r = d->card.rows[i];
        const int32_t cy = (row.y1 + row.y2) / 2;
        const int32_t vw = r.value[0] ? text_w(r.value, font) : 0;
        lv_area_t name = {row.x1, row.y1, row.x2 - (vw ? vw + 8 : 0), row.y2};
        switch (r.kind) {
            case TSCHART_ROW_TITLE:
            case TSCHART_ROW_TEXT:
                text(layer, name, r.label, col_dim(), LV_TEXT_ALIGN_LEFT, font);
                break;
            case TSCHART_ROW_WARNING:
                text(layer, name, r.label, r.color, LV_TEXT_ALIGN_LEFT, font);
                break;
            case TSCHART_ROW_BLOCK:
                rect(layer, {row.x1, cy - 8, row.x1 + 16, cy + 8}, r.color, LV_OPA_COVER, 3);
                name.x1 += 24;
                text(layer, name, r.label, r.color, LV_TEXT_ALIGN_LEFT, font);
                break;
            case TSCHART_ROW_VALUE:
                rect(layer, {row.x1, cy - 2, row.x1 + 16, cy + 2}, r.color, LV_OPA_COVER, 2);
                name.x1 += 24;
                text(layer, name, r.label, col_text(c), LV_TEXT_ALIGN_LEFT, font);
                break;
        }
        if (vw) text(layer, row, r.value, col_text(c), LV_TEXT_ALIGN_RIGHT, font);
    }
    if (d->card.overflow) {
        text(layer, row, "\xE2\x80\xA6", col_dim(), LV_TEXT_ALIGN_LEFT, font);
    }
}

void draw_cursor(lv_obj_t *c, lv_layer_t *layer) {
    Impl *d = impl(c);
    const lv_area_t &P = d->plot;
    const int32_t x = cursor_x(d);
    lv_draw_line_dsc_t dash;
    lv_draw_line_dsc_init(&dash);
    dash.color = col_dim();
    dash.width = 2;
    dash.dash_width = 6;
    dash.dash_gap = 5;
    dash.p1.x = x;
    dash.p1.y = P.y1;
    dash.p2.x = x;
    dash.p2.y = P.y2;
    lv_draw_line(layer, &dash);
    tschart_source *def = default_source(d);
    const uint32_t t = def->times[d->cursor];
    for (uint8_t i = 0; i < d->nlayers; i++) {
        const tschart_layer &l = d->layers[i];
        const tschart_pane &p = *l.pane;
        if (!l.visible || !l.cfg.in_cursor || !p.visible || !l.vals) continue;
        const int32_t v = value_at(l, t, (size_t)d->cursor, l.src == def);
        if (v == NONE || v < p.axis.lo || v > p.axis.hi) continue;
        dot(layer, x, map_y(v, p.axis.lo, p.axis.hi, p.slot.y1, p.slot.y2), l.cfg.color,
            col_panel(c));
    }
    draw_card(c, layer, card_box(c));
}

}  // namespace

// ---- Shared helpers ----------------------------------------------------------------

lv_color_t col_panel(lv_obj_t *c) { return lv_obj_get_style_bg_color(c, LV_PART_MAIN); }
lv_color_t col_grid(lv_obj_t *c) { return lv_obj_get_style_border_color(c, LV_PART_MAIN); }
lv_color_t col_text(lv_obj_t *c) { return lv_obj_get_style_text_color(c, LV_PART_MAIN); }
lv_color_t col_dim() { return lv_color_hex(0x888888); }

size_t fmt_value(const tschart_pane &p, int32_t v, tschart_fmt_ctx_t ctx, char *buf,
                 size_t cap) {
    if (p.cfg.fmt) return p.cfg.fmt(v, ctx, buf, cap, p.cfg.fmt_ud);
    if (v == NONE) return (size_t)snprintf(buf, cap, "--");
    return (size_t)snprintf(buf, cap, "%ld", (long)v);
}

int32_t value_at(const tschart_layer &l, uint32_t t, size_t default_index,
                 bool default_src) {
    if (!l.vals) return NONE;
    if (default_src) return default_index < l.count ? l.vals[default_index] : NONE;
    const int32_t j = nearest_time(l.src->times, l.count, t, l.src->data.gap_seconds);
    return j < 0 ? NONE : l.vals[j];
}

// ---- Cache ----------------------------------------------------------------------------

bool cache_ready(lv_obj_t *c) {
    Impl *d = impl(c);
    return d->cache_valid && d->cache_key == current_key(c, d->start, d->end);
}

// While a new window loads, the image of the previous one (same length, same
// everything else) is drawn moved by the difference, so the old data already
// sits where it belongs and only the strip being loaded is blank.
bool cache_shift(lv_obj_t *c, int32_t *shift_px) {
    Impl *d = impl(c);
    if (!d->cache_valid || !d->have_plot) return false;
    uint32_t ts, te;
    target_window(d, &ts, &te);
    const CacheKey now = current_key(c, ts, te);
    CacheKey moved = d->cache_key;
    if (moved.end - moved.start != now.end - now.start) return false;
    moved.start = now.start;
    moved.end = now.end;
    if (!(moved == now)) return false;
    *shift_px = (int32_t)(((int64_t)d->cache_key.end - (int64_t)now.end) *
                          lv_area_get_width(&d->plot) / (int64_t)(now.end - now.start));
    return true;
}

void cache_free(Impl *d) {
    d->cache_valid = false;
#ifdef ESP_PLATFORM
    heap_caps_free(d->buf_data);
#else
    lv_free(d->buf_data);
#endif
    d->buf_data = nullptr;
    d->buf_size = 0;
}

void cache_build(lv_obj_t *c) {
    Impl *d = impl(c);
    layout(c);
    if (cache_ready(c) || !d->have_plot || !d->has_ds) return;
    const int64_t t0 = now_us();
    const CacheKey key = current_key(c, d->start, d->end);
    d->cache_valid = false;
    if (key.w <= 0 || key.h <= 0) return;
    const uint32_t stride = (uint32_t)key.w * 4;
    const size_t size = (size_t)stride * (size_t)key.h;
    if (size != d->buf_size) {
        cache_free(d);
#ifdef ESP_PLATFORM
        d->buf_data = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, size,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        d->buf_data = lv_malloc(size);
#endif
        d->buf_size = d->buf_data ? size : 0;
        if (d->buf_data == nullptr) return;  // keep drawing directly
    }
    memset(d->buf_data, 0, size);
    lv_draw_buf_init(&d->buf, (uint32_t)key.w, (uint32_t)key.h, LV_COLOR_FORMAT_ARGB8888,
                     stride, d->buf_data, size);
    if (d->canvas == nullptr) {
        d->canvas = lv_canvas_create(c);
        lv_obj_add_flag(d->canvas, LV_OBJ_FLAG_HIDDEN);
    }
    lv_canvas_set_draw_buf(d->canvas, &d->buf);
    lv_layer_t layer;
    lv_canvas_init_layer(d->canvas, &layer);
    lv_area_t coords;
    lv_obj_get_coords(c, &coords);
    const Geo g = make_geo(c, -coords.x1, -coords.y1);
    // Pose as a refresh of this layer, as lv_snapshot does, so each draw task
    // runs as it's added. Otherwise thousands queue up and LVGL's list
    // handling goes quadratic (8 h chart: ~10 s vs ~0.15 s).
    lv_display_t *disp = lv_obj_get_display(c);
    lv_display_t *disp_old = lv_refr_get_disp_refreshing();
    lv_layer_t *layer_old = disp->layer_head;
    disp->layer_head = &layer;
    lv_refr_set_disp_refreshing(disp);
    draw_static(c, &layer, g);
    layer.all_tasks_added = true;
    while (layer.draw_task_head) {
        lv_draw_dispatch_wait_for_request();
        lv_draw_dispatch();
    }
    disp->layer_head = layer_old;
    lv_refr_set_disp_refreshing(disp_old);
    lv_canvas_finish_layer(d->canvas, &layer);
    d->cache_key = key;
    d->cache_valid = true;
    d->prof.build_us += now_us() - t0;
}

// ---- Draw entry points -------------------------------------------------------------

void draw_main(lv_obj_t *c, lv_layer_t *layer) {
    Impl *d = impl(c);
    if (!d->have_plot) return;
    const int64_t t0 = now_us();
    lv_area_t coords;
    lv_obj_get_coords(c, &coords);
    const lv_area_t &P = d->plot;
    int32_t shift = 0;
    const bool cached = cache_shift(c, &shift);
    const int32_t dx = shift + ((d->panning || d->springing) ? d->dx : 0);
    if (cached) {
        lv_draw_image_dsc_t image;
        lv_draw_image_dsc_init(&image);
        image.src = &d->buf;
        if (dx == 0) {
            lv_draw_image(layer, &image, &coords);
        } else {
            // Margins stay put; the plot band (data, spans, time axis)
            // slides with the finger.
            const lv_area_t saved = layer->_clip_area;
            const lv_area_t fixed[2] = {
                {coords.x1, coords.y1, P.x1 - 1, coords.y2},
                {P.x2 + 1, coords.y1, coords.x2, coords.y2},
            };
            for (const auto &part : fixed) {
                if (lv_area_intersect(&layer->_clip_area, &saved, &part)) {
                    lv_draw_image(layer, &image, &coords);
                }
            }
            const lv_area_t band = {P.x1, coords.y1, P.x2, coords.y2};
            if (lv_area_intersect(&layer->_clip_area, &saved, &band)) {
                lv_area_t shifted = coords;
                lv_area_move(&shifted, dx, 0);
                lv_draw_image(layer, &image, &shifted);
            }
            layer->_clip_area = saved;
            edge_zones(c, layer, dx);
        }
    } else if (!d->pending) {
        draw_static(c, layer, make_geo(c, 0, 0));
    }
    if (d->axis_open) draw_gutter(c, layer);
    if (d->end_tags && d->cursor < 0 && dx == 0 && !d->pending) draw_end_tags(c, layer);
    {
        d->prof.cached = cached;
        d->prof.draw_calls++;
        d->prof.draw_us += now_us() - t0;
    }
}

void draw_post(lv_obj_t *c, lv_layer_t *layer) {
    Impl *d = impl(c);
    if (d->cursor < 0 || d->pending || !d->have_plot || !default_source(d)) return;
    const int64_t t0 = now_us();
    draw_cursor(c, layer);
    d->prof.draw_us += now_us() - t0;
}

void build_card(lv_obj_t *c) {
    Impl *d = impl(c);
    tschart_card &card = d->card;
    card = {};
    card.layers_at = -1;
    tschart_source *def = default_source(d);
    if (d->cursor < 0 || !def) return;
    const size_t idx = (size_t)d->cursor;
    const tschart_cursor_info_t info = {def->times[idx], def, idx};
    if (d->card_cb) d->card_cb(c, &info, &card, d->card_ud);

    tschart_card_row rows[TSCHART_MAX_LAYERS];
    uint8_t n = 0;
    for (uint8_t i = 0; i < d->nlayers; i++) {
        const tschart_layer &l = d->layers[i];
        if (!l.visible || !l.cfg.in_cursor) continue;
        size_t k = idx;
        if (l.src != def) {
            const int32_t j = nearest_time(l.src->times, l.count, info.time,
                                           l.src->data.gap_seconds);
            k = j < 0 ? SIZE_MAX : (size_t)j;
        }
        // The reading itself, not the interpolated line through it.
        const int32_t v = k == SIZE_MAX ? NONE : read_field(l, k);
        tschart_card_row &r = rows[n++];
        r.kind = TSCHART_ROW_VALUE;
        snprintf(r.label, sizeof(r.label), "%s", l.name);
        fmt_value(*l.pane, v, TSCHART_FMT_CURSOR, r.value, sizeof(r.value));
        r.color = l.cfg.color;
    }
    const uint8_t at = card.layers_at < 0 ? card.count : (uint8_t)card.layers_at;
    tschart_card_row merged[CARD_MAX_ROWS];
    uint8_t m = 0;
    bool overflow = card.overflow;
    auto push = [&](const tschart_card_row &r) {
        if (m < CARD_MAX_ROWS) merged[m++] = r;
        else overflow = true;
    };
    for (uint8_t i = 0; i < at; i++) push(card.rows[i]);
    for (uint8_t i = 0; i < n; i++) push(rows[i]);
    for (uint8_t i = at; i < card.count; i++) push(card.rows[i]);
    memcpy(card.rows, merged, sizeof(merged[0]) * m);
    card.count = m;
    card.overflow = overflow;
}

bool cursor_bounds(lv_obj_t *c, lv_area_t *out) {
    Impl *d = impl(c);
    if (d->cursor < 0 || !d->have_plot || !default_source(d)) return false;
    const int32_t x = cursor_x(d);
    out[0] = {x - 10, d->plot.y1 - 10, x + 10, d->plot.y2 + 10};
    out[1] = card_box(c);
    return true;
}

}  // namespace tschart
