/*
 * tschart -- object class, public API, layout, header controls and input.
 * Rendering lives in tschart_draw.cpp.
 */
#include "tschart_priv.h"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace tschart;

lv_event_code_t TSCHART_EVENT_WINDOW_REQUEST;
lv_event_code_t TSCHART_EVENT_DOUBLE_TAP;
lv_event_code_t TSCHART_EVENT_GROUP_TOGGLED;
lv_event_code_t TSCHART_EVENT_AXIS_TOGGLED;
lv_event_code_t TSCHART_EVENT_TAGS_TOGGLED;
lv_event_code_t TSCHART_EVENT_CURSOR_CHANGED;

namespace {

lv_obj_class_t s_class;

constexpr lv_obj_flag_t flags(uint32_t f) { return static_cast<lv_obj_flag_t>(f); }

void copy_str(char *dst, size_t cap, const char *src) {
    if (cap == 0) return;
    snprintf(dst, cap, "%s", src ? src : "");
}

Impl *D(lv_obj_t *c) { return impl(c); }

void set_cursor(lv_obj_t *c, int32_t idx);
void update_status(lv_obj_t *c);
void update_pills(lv_obj_t *c);
void update_header(lv_obj_t *c);
void update_chip(tschart_group *g);
void retag(lv_obj_t *c);

// Anything under the static image changed: re-lay out now (child controls
// move outside rendering) and rebuild the image on the next timer tick.
void invalidate_static(lv_obj_t *c) {
    Impl *d = D(c);
    d->static_gen++;
    d->layout_dirty = true;
    layout(c);
    schedule(c);
    lv_obj_invalidate(c);
}

void timer_cb(lv_timer_t *t) {
    lv_obj_t *c = (lv_obj_t *)lv_timer_get_user_data(t);
    Impl *d = D(c);
    d->timer = nullptr;  // one-shot, deleted after this returns
    layout(c);
    if (!d->panning) cache_build(c);
    lv_obj_invalidate(c);
}

void update_visibility(Impl *d) {
    uint8_t shown = 0;
    for (uint8_t i = 0; i < d->ngroups; i++) {
        if (d->groups[i].visible) shown |= (uint8_t)(1u << i);
    }
    for (uint8_t i = 0; i < d->npanes; i++) d->panes[i].visible = false;
    bool pane_has_layer[TSCHART_MAX_PANES] = {};
    for (uint8_t i = 0; i < d->nlayers; i++) {
        tschart_layer &l = d->layers[i];
        const uint8_t g = l.groups | l.pane->groups;
        l.visible = g == 0 || (g & shown);
        pane_has_layer[l.pane->index] = true;
        if (l.visible) l.pane->visible = true;
    }
    for (uint8_t i = 0; i < d->npanes; i++) {
        tschart_pane &p = d->panes[i];
        if (!pane_has_layer[i]) p.visible = p.groups == 0 || (p.groups & shown);
    }
}

void compute_axes(Impl *d) {
    for (uint8_t i = 0; i < d->npanes; i++) {
        tschart_pane &p = d->panes[i];
        if (!p.visible) continue;
        int32_t lo = INT32_MAX, hi = INT32_MIN;
        for (uint8_t j = 0; j < d->nlayers; j++) {
            const tschart_layer &l = d->layers[j];
            if (l.pane != &p || !l.visible || !l.cfg.drives_range || !l.vals) continue;
            const uint32_t *t = l.src->times;
            for (size_t k = 0; k < l.count; k++) {
                if (t[k] < d->start || t[k] > d->end || l.vals[k] == NONE) continue;
                if (l.vals[k] < lo) lo = l.vals[k];
                if (l.vals[k] > hi) hi = l.vals[k];
            }
        }
        const RangeCfg rc = {p.cfg.auto_range, p.cfg.has_min, p.cfg.has_max,
                             p.cfg.min, p.cfg.max, p.cfg.min_span,
                             p.cfg.include_zero, p.cfg.pad_permille, p.cfg.min_pad,
                             p.cfg.min_step};
        const int32_t h = p.slot.y2 - p.slot.y1;
        const int32_t ticks = ticks_for_height(h, p.cfg.max_ticks ? p.cfg.max_ticks : 5, 36);
        p.axis = auto_range(rc, lo, hi, ticks);
    }
}

// ---- Child controls ----------------------------------------------------

void style_pill(lv_obj_t *o, lv_color_t border, lv_color_t bg) {
    lv_obj_remove_style_all(o);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(o, 2, 0);
    lv_obj_set_style_border_color(o, border, 0);
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, flags(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE |
                                    LV_OBJ_FLAG_SCROLL_ON_FOCUS));
}

lv_obj_t *passive_child(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, flags(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    return o;
}

void pill_cb(lv_event_t *e) {
    lv_obj_t *c = (lv_obj_t *)lv_event_get_user_data(e);
    tschart_set_axis_open(c, !D(c)->axis_open);
    tschart_toggled_t p = {D(c)->axis_open};
    lv_obj_send_event(c, TSCHART_EVENT_AXIS_TOGGLED, &p);
}

void chip_cb(lv_event_t *e) {
    tschart_group *g = (tschart_group *)lv_event_get_user_data(e);
    tschart_group_set_visible(g, !g->visible);
    tschart_group_toggled_t p = {g, g->visible};
    lv_obj_send_event(g->chart, TSCHART_EVENT_GROUP_TOGGLED, &p);
}

void tags_btn_cb(lv_event_t *e) {
    lv_obj_t *c = (lv_obj_t *)lv_event_get_user_data(e);
    tschart_set_end_tags(c, !D(c)->end_tags);
    tschart_toggled_t p = {D(c)->end_tags};
    lv_obj_send_event(c, TSCHART_EVENT_TAGS_TOGGLED, &p);
}

// A price-tag outline: body with a pointed left end and a punched hole.
void tags_icon_cb(lv_event_t *e) {
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_current_target(e);
    lv_obj_t *c = (lv_obj_t *)lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(btn, &a);
    const int32_t cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
    const lv_color_t fg = D(c)->end_tags ? col_text(c) : col_dim();
    const lv_area_t body = {cx - 4, cy - 7, cx + 11, cy + 7};
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = fg;
    r.radius = 2;
    lv_draw_rect(layer, &r, &body);
    lv_draw_triangle_dsc_t tri;
    lv_draw_triangle_dsc_init(&tri);
    tri.color = fg;
    tri.p[0] = {(lv_value_precise_t)(cx - 11), (lv_value_precise_t)cy};
    tri.p[1] = {(lv_value_precise_t)(cx - 4), (lv_value_precise_t)(cy - 7)};
    tri.p[2] = {(lv_value_precise_t)(cx - 4), (lv_value_precise_t)(cy + 8)};
    lv_draw_triangle(layer, &tri);
    lv_draw_rect_dsc_t hole;
    lv_draw_rect_dsc_init(&hole);
    hole.bg_color = col_panel(c);
    hole.radius = LV_RADIUS_CIRCLE;
    const lv_area_t h = {cx - 4, cy - 2, cx, cy + 2};
    lv_draw_rect(layer, &hole, &h);
}

void update_chip(tschart_group *g) {
    if (!g->chip) return;
    lv_obj_t *c = g->chart;
    lv_obj_set_style_border_color(g->chip, g->visible ? g->color : col_grid(c), 0);
    lv_obj_set_style_text_color(g->chip_label, g->visible ? col_text(c) : col_dim(), 0);
    lv_obj_set_style_bg_opa(g->chip_swatch, g->visible ? LV_OPA_COVER : LV_OPA_30, 0);
    lv_label_set_text(g->chip_label, g->label);
}

void update_pills(lv_obj_t *c) {
    Impl *d = D(c);
    lv_area_t coords;
    lv_obj_get_coords(c, &coords);
    for (uint8_t i = 0; i < d->npanes; i++) {
        tschart_pane &p = d->panes[i];
        if (!p.pill) continue;
        if (!p.visible || !d->have_plot || p.slot.y2 <= p.slot.y1) {
            lv_obj_add_flag(p.pill, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(p.pill, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(p.pill_label, "%s %s", p.unit,
                              d->axis_open ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_font(p.pill_label, d->small_font, 0);
        lv_obj_set_style_text_color(p.pill_label, p.cfg.color, 0);
        lv_obj_set_style_border_color(p.pill, p.cfg.color, 0);
        lv_obj_set_style_bg_color(p.pill, col_panel(c), 0);
        lv_obj_set_pos(p.pill,
                       d->plot.x1 - coords.x1 + 4 + (d->axis_open ? GUTTER_W : 0),
                       p.slot.y1 - coords.y1 + 4);
    }
}

void update_header(lv_obj_t *c) {
    Impl *d = D(c);
    const uint8_t f = d->header_flags;
    if (f == 0) {
        lv_obj_add_flag(d->header, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(d->header, LV_OBJ_FLAG_HIDDEN);
    for (uint8_t i = 0; i < d->ngroups; i++) {
        lv_obj_set_flag(d->groups[i].chip, LV_OBJ_FLAG_HIDDEN, !(f & TSCHART_HEADER_CHIPS));
        update_chip(&d->groups[i]);
    }
    lv_obj_t *spacer = lv_obj_get_child(d->header, 0);
    lv_obj_set_flag(spacer, LV_OBJ_FLAG_HIDDEN, !(f & TSCHART_HEADER_TAG_BTN));
    lv_obj_set_flag(d->tags_btn, LV_OBJ_FLAG_HIDDEN, !(f & TSCHART_HEADER_TAG_BTN));
    lv_obj_set_style_border_color(d->tags_btn, d->end_tags ? col_text(c) : col_grid(c), 0);
    lv_obj_set_flag(d->key_row, LV_OBJ_FLAG_HIDDEN, !(f & TSCHART_HEADER_KEY) || d->nkeys == 0);
    for (uint8_t i = 0; i < d->nkeys; i++) {
        lv_obj_t *label = lv_obj_get_child(d->keys[i].obj, 1);
        lv_obj_set_style_text_font(label, d->small_font, 0);
    }
}

void update_status(lv_obj_t *c) {
    Impl *d = D(c);
    const bool spin = d->pending || d->status == TSCHART_STATUS_LOADING;
    const bool msg = !spin && (d->status == TSCHART_STATUS_EMPTY ||
                               d->status == TSCHART_STATUS_ERROR);
    lv_area_t coords;
    lv_obj_get_coords(c, &coords);
    const lv_area_t &plot = d->plot;
    if (spin && d->have_plot) {
        if (!d->spinner) {
            d->spinner = lv_spinner_create(c);
            lv_spinner_set_anim_params(d->spinner, 800, 90);
            lv_obj_set_size(d->spinner, 44, 44);
            lv_obj_set_style_arc_width(d->spinner, 5, LV_PART_MAIN);
            lv_obj_set_style_arc_width(d->spinner, 5, LV_PART_INDICATOR);
            lv_obj_remove_flag(d->spinner, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(d->spinner, d->spinner_tag);
        }
        const int32_t permille =
            d->pending ? uncovered_permille(d->start, d->end, d->pend_start, d->pend_end)
                       : 500;
        const int32_t x = plot.x1 + lv_area_get_width(&plot) * permille / 1000;
        const int32_t y = (plot.y1 + plot.y2) / 2;
        lv_obj_set_pos(d->spinner, x - coords.x1 - 22, y - coords.y1 - 22);
        lv_obj_remove_flag(d->spinner, LV_OBJ_FLAG_HIDDEN);
    } else if (d->spinner) {
        lv_obj_add_flag(d->spinner, LV_OBJ_FLAG_HIDDEN);
    }
    if (msg && d->have_plot) {
        lv_label_set_text(d->status_label, d->status_text);
        lv_obj_set_width(d->status_label, lv_area_get_width(&plot) * 8 / 10);
        lv_obj_update_layout(d->status_label);
        lv_obj_set_pos(d->status_label,
                       (plot.x1 + plot.x2) / 2 - coords.x1 - lv_obj_get_width(d->status_label) / 2,
                       (plot.y1 + plot.y2) / 2 - coords.y1 - lv_obj_get_height(d->status_label) / 2);
        lv_obj_remove_flag(d->status_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(d->status_label, LV_OBJ_FLAG_HIDDEN);
    }
}

void make_tag(char *dst, const char *id, const char *suffix, const char *extra = "") {
    const int n = snprintf(dst, TAG_LEN, "%s%s%s", id, suffix, extra);
    if (n >= TAG_LEN) dst[TAG_LEN - 1] = '\0';
}

void retag(lv_obj_t *c) {
    Impl *d = D(c);
    make_tag(d->tags_btn_tag, d->id, "_tags_btn");
    make_tag(d->spinner_tag, d->id, "_spinner");
    make_tag(d->status_tag, d->id, "_status");
    for (uint8_t i = 0; i < d->ngroups; i++) make_tag(d->groups[i].tag, d->id, "_chip_", d->groups[i].id);
    for (uint8_t i = 0; i < d->npanes; i++) make_tag(d->panes[i].tag, d->id, "_axis_", d->panes[i].id);
}

// ---- Cursor and gestures -------------------------------------------------

int32_t cursor_at(lv_obj_t *c, int32_t px) {
    Impl *d = D(c);
    if (!d->has_ds || !d->have_plot || px < d->plot.x1 || px > d->plot.x2) return -1;
    tschart_source *src = default_source(d);
    if (!src || !src->times) return -1;
    int32_t best = -1, best_dx = INT32_MAX;
    for (size_t i = 0; i < src->data.count; i++) {
        bool ok = false;
        for (uint8_t j = 0; j < d->nlayers && !ok; j++) {
            const tschart_layer &l = d->layers[j];
            ok = l.visible && l.cfg.in_cursor && l.src == src && l.vals &&
                 l.vals[i] != NONE;
        }
        if (!ok) continue;
        const int32_t dx = abs(map_x(src->times[i], d->start, d->end, d->plot.x1,
                                     d->plot.x2) - px);
        if (dx < best_dx) {
            best_dx = dx;
            best = (int32_t)i;
        }
    }
    return best_dx > CURSOR_SNAP_PX ? -1 : best;
}

void invalidate_cursor(lv_obj_t *c) {
    Impl *d = D(c);
    if (!d->cursor_area_valid) return;
    for (auto &a : d->cursor_area) lv_obj_invalidate_area(c, &a);
}

void set_cursor(lv_obj_t *c, int32_t idx) {
    Impl *d = D(c);
    if (idx == d->cursor) return;
    const bool was = d->cursor >= 0;
    if (idx >= 0) cache_build(c);
    invalidate_cursor(c);
    d->cursor = idx;
    if (idx >= 0) build_card(c);
    d->cursor_area_valid = idx >= 0 && cursor_bounds(c, d->cursor_area);
    invalidate_cursor(c);
    // End tags hide while the card shows.
    if (was != (idx >= 0) && d->end_tags) lv_obj_invalidate(c);
    tschart_cursor_changed_t p = {idx >= 0, 0, idx >= 0 ? (size_t)idx : 0};
    if (idx >= 0) p.time = default_source(d)->times[idx];
    lv_obj_send_event(c, TSCHART_EVENT_CURSOR_CHANGED, &p);
}

uint32_t request(lv_obj_t *c, uint32_t start, uint32_t end, bool emit) {
    Impl *d = D(c);
    set_cursor(c, -1);
    d->pending = true;
    d->pend_start = start;
    d->pend_end = end;
    if (++d->request_id == 0) d->request_id = 1;
    update_status(c);
    lv_obj_invalidate(c);
    if (emit) {
        tschart_window_request_t p = {d->request_id, start, end};
        lv_obj_send_event(c, TSCHART_EVENT_WINDOW_REQUEST, &p);
    }
    return d->request_id;
}

void apply_deferred(lv_obj_t *c) {
    Impl *d = D(c);
    if (!d->has_deferred || d->down || d->springing) return;
    tschart_dataset_t ds = d->deferred;
    d->has_deferred = false;
    tschart_set_dataset(c, &ds);
}

void spring_exec_cb(void *var, int32_t v) {
    Impl *d = (Impl *)var;
    d->dx = v;
    lv_obj_invalidate(d->self);
}

void spring_done_cb(lv_anim_t *a) {
    Impl *d = (Impl *)a->var;
    d->springing = false;
    d->dx = 0;
    lv_obj_invalidate(d->self);
    apply_deferred(d->self);
}

void spring_stop(Impl *d) {
    if (!d->springing) return;
    lv_anim_delete(d, spring_exec_cb);
    d->springing = false;
    d->dx = 0;
}

void pan_end(lv_obj_t *c, bool apply) {
    Impl *d = D(c);
    if (!d->panning) return;
    d->panning = false;
    d->pan_ended_ms = lv_tick_get();
    const int32_t shown = d->dx;
    uint32_t ts, te;
    target_window(d, &ts, &te);
    uint32_t new_end = te;
    const bool move = apply && pan_target(shown, ts, te, d->earliest, d->latest,
                                          lv_area_get_width(&d->plot), d->snap, &new_end);
    d->dx = 0;
    if (!move) {
        // Nowhere new to go (too small a drag, or pulled past either end):
        // ease back into place.
        if (shown == 0 || !apply) {
            lv_obj_invalidate(c);
            return;
        }
        d->springing = true;
        d->dx = shown;
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, d);
        lv_anim_set_exec_cb(&anim, spring_exec_cb);
        lv_anim_set_values(&anim, shown, 0);
        lv_anim_set_duration(&anim, SPRING_MS);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
        lv_anim_set_completed_cb(&anim, spring_done_cb);
        lv_anim_start(&anim);
        return;
    }
    request(c, new_end - (te - ts), new_end, true);
}

bool in_plot_band(const Impl *d, const lv_point_t &p) {
    const int32_t top = d->plot.y1 - (d->nnotes ? NOTE_STRIP_H : 0);
    return d->have_plot && p.y >= top && p.x >= d->plot.x1 - SIDE_PAD &&
           p.x <= d->plot.x2 + SIDE_PAD;
}

void on_press(lv_obj_t *c, lv_event_code_t code) {
    Impl *d = D(c);
    lv_indev_t *indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    switch (code) {
        case LV_EVENT_PRESSED:
            d->down = true;
            spring_stop(d);
            d->x0 = point.x;
            d->dx = 0;
            d->panning = false;
            d->scrubbing = false;
            d->press_in_plot = in_plot_band(d, point);
            break;
        case LV_EVENT_LONG_PRESSED:
            if (!d->panning) d->scrubbing = true;
            return;
        case LV_EVENT_RELEASED:
        case LV_EVENT_PRESS_LOST:
            pan_end(c, code == LV_EVENT_RELEASED);
            d->scrubbing = false;
            d->down = false;
            apply_deferred(c);
            if (d->timer == nullptr && !d->cache_valid) schedule(c);
            return;
        default:
            break;
    }
    if (!d->press_in_plot) return;
    if (d->panning) {
        uint32_t ts, te;
        target_window(d, &ts, &te);
        const PanLimits lim = pan_limits(ts, te, d->earliest, d->latest,
                                         lv_area_get_width(&d->plot));
        const int32_t dx = rubber_band(point.x - d->x0, lim);
        if (dx != d->dx) {
            d->dx = dx;
            lv_obj_invalidate(c);
        }
        return;
    }
    if (!d->scrubbing && abs(point.x - d->x0) > PAN_START_PX) {
        set_cursor(c, -1);
        // Mid-load the old window's image, drawn moved into place, is what
        // slides; building now would draw stale data into the new window.
        int32_t shift;
        const bool ready = loading(d) ? cache_shift(c, &shift)
                                      : (cache_build(c), cache_ready(c));
        if (ready) {
            d->panning = true;
            d->x0 = point.x;  // start sliding from here, no jump
            return;
        }
    }
    if (!d->has_ds || loading(d)) return;
    set_cursor(c, cursor_at(c, point.x));
}

void on_double(lv_obj_t *c) {
    Impl *d = D(c);
    lv_indev_t *indev = lv_indev_active();
    if (indev == nullptr || !d->press_in_plot) return;
    if (lv_tick_elaps(d->pan_ended_ms) < DOUBLE_TAP_AFTER_PAN_MS) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    uint32_t ts, te;
    target_window(d, &ts, &te);
    tschart_double_tap_t p = {time_at(point.x, ts, te, d->plot.x1, d->plot.x2)};
    lv_obj_send_event(c, TSCHART_EVENT_DOUBLE_TAP, &p);
}

// ---- Class ---------------------------------------------------------------

void release_ds(tschart_dataset_t *ds) {
    if (ds->release) ds->release(ds->release_ud);
    ds->release = nullptr;
}

void constructor_cb(const lv_obj_class_t *, lv_obj_t *obj) {
    void *mem = lv_malloc_zeroed(sizeof(Impl));
    LV_ASSERT_MALLOC(mem);
    Impl *d = new (mem) Impl();
    ((Chart *)obj)->d = d;
    d->self = obj;
    d->cursor = -1;
    d->end_tags = true;
    d->layout_dirty = true;
    d->small_font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
    lv_obj_remove_flag(obj, flags(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_MOMENTUM |
                                      LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_GESTURE_BUBBLE));
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    d->header = passive_child(obj);
    lv_obj_set_height(d->header, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(d->header, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(d->header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(d->header, 10, 0);
    lv_obj_set_style_pad_row(d->header, 6, 0);
    lv_obj_add_flag(d->header, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *spacer = passive_child(d->header);  // child 0: pushes tags right
    lv_obj_set_size(spacer, 0, 1);
    lv_obj_set_flex_grow(spacer, 1);

    d->tags_btn = lv_obj_create(d->header);
    style_pill(d->tags_btn, lv_color_white(), lv_color_black());
    lv_obj_set_size(d->tags_btn, 52, 40);
    lv_obj_set_style_bg_opa(d->tags_btn, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(d->tags_btn, tags_btn_cb, LV_EVENT_CLICKED, obj);
    lv_obj_add_event_cb(d->tags_btn, tags_icon_cb, LV_EVENT_DRAW_MAIN_END, obj);
    lv_obj_set_user_data(d->tags_btn, d->tags_btn_tag);
    lv_obj_set_ext_click_area(d->tags_btn, 8);

    d->key_row = passive_child(d->header);
    lv_obj_set_size(d->key_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(d->key_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(d->key_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(d->key_row, 18, 0);
    lv_obj_set_style_pad_row(d->key_row, 4, 0);
    lv_obj_add_flag(d->key_row, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);

    d->status_label = lv_label_create(obj);
    lv_label_set_long_mode(d->status_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(d->status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(d->status_label, col_dim(), 0);
    lv_obj_add_flag(d->status_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_user_data(d->status_label, d->status_tag);
}

void destructor_cb(const lv_obj_class_t *, lv_obj_t *obj) {
    Impl *d = D(obj);
    if (d == nullptr) return;
    lv_anim_delete(d, nullptr);
    if (d->timer) lv_timer_delete(d->timer);
    if (d->has_ds) release_ds(&d->ds);
    if (d->has_deferred) release_ds(&d->deferred);
    for (auto &s : d->sources) lv_free(s.times);
    for (auto &l : d->layers) lv_free(l.vals);
    for (auto &s : d->spans) lv_free(s.spans);
    lv_free(d->notes);
    d->canvas = nullptr;  // child, already deleted
    cache_free(d);
    d->~Impl();
    lv_free(d);
    ((Chart *)obj)->d = nullptr;
}

void event_cb(const lv_obj_class_t *, lv_event_t *e) {
    if (lv_obj_event_base(&s_class, e) != LV_RESULT_OK) return;
    const lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *c = (lv_obj_t *)lv_event_get_current_target(e);
    switch (code) {
        case LV_EVENT_DRAW_MAIN:
            draw_main(c, lv_event_get_layer(e));
            break;
        case LV_EVENT_DRAW_POST:
            draw_post(c, lv_event_get_layer(e));
            break;
        case LV_EVENT_SIZE_CHANGED:
        case LV_EVENT_STYLE_CHANGED:
            D(c)->static_gen++;
            D(c)->layout_dirty = true;
            schedule(c);
            break;
        case LV_EVENT_PRESSED:
        case LV_EVENT_PRESSING:
        case LV_EVENT_LONG_PRESSED:
        case LV_EVENT_RELEASED:
        case LV_EVENT_PRESS_LOST:
            on_press(c, code);
            break;
        case LV_EVENT_DOUBLE_CLICKED:
            on_double(c);
            break;
        default:
            break;
    }
}

void class_init() {
    if (s_class.name) return;
    s_class.base_class = &lv_obj_class;
    s_class.constructor_cb = constructor_cb;
    s_class.destructor_cb = destructor_cb;
    s_class.event_cb = event_cb;
    s_class.width_def = LV_PCT(100);
    s_class.height_def = 400;
    s_class.group_def = LV_OBJ_CLASS_GROUP_DEF_FALSE;
    s_class.instance_size = sizeof(Chart);
    s_class.name = "tschart";
    TSCHART_EVENT_WINDOW_REQUEST = (lv_event_code_t)lv_event_register_id();
    TSCHART_EVENT_DOUBLE_TAP = (lv_event_code_t)lv_event_register_id();
    TSCHART_EVENT_GROUP_TOGGLED = (lv_event_code_t)lv_event_register_id();
    TSCHART_EVENT_AXIS_TOGGLED = (lv_event_code_t)lv_event_register_id();
    TSCHART_EVENT_TAGS_TOGGLED = (lv_event_code_t)lv_event_register_id();
    TSCHART_EVENT_CURSOR_CHANGED = (lv_event_code_t)lv_event_register_id();
}

}  // namespace

// ---- Internal API (tschart_priv.h) -------------------------------------------

namespace tschart {

void schedule(lv_obj_t *c) {
    Impl *d = D(c);
    if (d->timer) return;
    d->timer = lv_timer_create(timer_cb, 0, c);
    lv_timer_set_repeat_count(d->timer, 1);
}

bool loading(const Impl *d) { return d->pending || d->has_deferred; }

void target_window(const Impl *d, uint32_t *start, uint32_t *end) {
    *start = d->pending ? d->pend_start : d->start;
    *end = d->pending ? d->pend_end : d->end;
}

tschart_source_t *default_source(Impl *d) {
    return d->nsources ? &d->sources[0] : nullptr;
}

int32_t read_field(const tschart_layer &l, size_t i) {
    const tschart_source_data_t &s = l.src->data;
    if (!s.base || i >= s.count) return NONE;
    const uint8_t *p = (const uint8_t *)s.base + i * s.stride;
    const tschart_field_t &f = l.cfg.field;
    if (f.flags_offset != TSCHART_NO_FLAGS && !(p[f.flags_offset] & f.flags_mask)) {
        return NONE;
    }
    int32_t raw;
    switch (f.type) {
        case TSCHART_I16: { int16_t v; memcpy(&v, p + f.offset, 2); raw = v; break; }
        case TSCHART_U16: { uint16_t v; memcpy(&v, p + f.offset, 2); raw = v; break; }
        case TSCHART_U32: { uint32_t v; memcpy(&v, p + f.offset, 4);
                            raw = v > INT32_MAX ? INT32_MAX : (int32_t)v; break; }
        default: { int32_t v; memcpy(&v, p + f.offset, 4); raw = v; break; }
    }
    if (raw == NONE) return NONE;
    return f.xform ? f.xform(raw, f.xform_ud) : raw;
}

void layout(lv_obj_t *c) {
    Impl *d = D(c);
    if (!d->layout_dirty) return;
    d->layout_dirty = false;
    lv_area_t coords;
    lv_obj_get_coords(c, &coords);
    const int32_t w = lv_area_get_width(&coords);
    if (w <= 2 * SIDE_PAD || lv_area_get_height(&coords) <= TIME_AXIS_H) {
        d->have_plot = false;
        return;
    }
    int32_t top = coords.y1 + 12;
    if (d->header_flags) {
        lv_obj_set_pos(d->header, HEADER_PAD, HEADER_PAD);
        lv_obj_set_width(d->header, w - 2 * HEADER_PAD);
        lv_obj_update_layout(d->header);
        lv_area_t h;
        lv_obj_get_coords(d->header, &h);
        top = h.y2 + 10;
    }
    if (d->nnotes) top += NOTE_STRIP_H;
    d->plot = {coords.x1 + SIDE_PAD, top, coords.x2 - SIDE_PAD, coords.y2 - TIME_AXIS_H};
    d->have_plot = d->plot.y2 > d->plot.y1 + 20;

    update_visibility(d);
    uint16_t weights[TSCHART_MAX_PANES], mins[TSCHART_MAX_PANES];
    bool vis[TSCHART_MAX_PANES];
    PaneSlot slots[TSCHART_MAX_PANES];
    for (uint8_t i = 0; i < d->npanes; i++) {
        weights[i] = d->panes[i].cfg.weight ? d->panes[i].cfg.weight : 1;
        mins[i] = d->panes[i].cfg.min_height;
        vis[i] = d->panes[i].visible;
    }
    split_panes(d->plot.y1, d->plot.y2, PANE_GAP, weights, mins, vis, d->npanes, slots);
    for (uint8_t i = 0; i < d->npanes; i++) d->panes[i].slot = slots[i];
    compute_axes(d);
    update_pills(c);
    update_status(c);
}

}  // namespace tschart

// ---- Public API ----------------------------------------------------------------

lv_obj_t *tschart_create(lv_obj_t *parent) {
    class_init();
    lv_obj_t *obj = lv_obj_class_create_obj(&s_class, parent);
    lv_obj_class_init_obj(obj);
    return obj;
}

void tschart_set_id(lv_obj_t *c, const char *id) {
    Impl *d = D(c);
    copy_str(d->id, sizeof(d->id), id);
    lv_obj_set_user_data(c, d->id);
    retag(c);
}

void tschart_set_small_font(lv_obj_t *c, const lv_font_t *font) {
    D(c)->small_font = font;
    update_header(c);
    invalidate_static(c);
}

void tschart_set_bounds(lv_obj_t *c, tschart_time_t earliest, tschart_time_t latest) {
    D(c)->earliest = earliest;
    D(c)->latest = latest;
    lv_obj_invalidate(c);
}

void tschart_set_edge_text(lv_obj_t *c, const char *before, const char *after) {
    copy_str(D(c)->edge_before, TEXT_LEN, before);
    copy_str(D(c)->edge_after, TEXT_LEN, after);
    lv_obj_invalidate(c);
}

void tschart_set_time_formatter(lv_obj_t *c, tschart_time_fmt_cb_t cb, void *ud) {
    D(c)->time_fmt = cb;
    D(c)->time_fmt_ud = ud;
    invalidate_static(c);
}

void tschart_set_pan_snap(lv_obj_t *c, uint32_t snap_seconds) { D(c)->snap = snap_seconds; }

uint32_t tschart_request_window(lv_obj_t *c, tschart_time_t start, tschart_time_t end) {
    return request(c, start, end, false);
}

void tschart_cancel_pending(lv_obj_t *c) {
    Impl *d = D(c);
    if (!d->pending) return;
    d->pending = false;
    update_status(c);
    lv_obj_invalidate(c);
}

void tschart_get_window(const lv_obj_t *c, tschart_time_t *start, tschart_time_t *end) {
    *start = impl(c)->start;
    *end = impl(c)->end;
}

void tschart_get_target_window(const lv_obj_t *c, tschart_time_t *start,
                               tschart_time_t *end) {
    target_window(impl(c), start, end);
}

tschart_source_t *tschart_add_source(lv_obj_t *c, const char *id) {
    Impl *d = D(c);
    if (d->nsources >= TSCHART_MAX_SOURCES) return nullptr;
    tschart_source &s = d->sources[d->nsources++];
    s.chart = c;
    copy_str(s.id, sizeof(s.id), id);
    return &s;
}

static void apply_dataset(lv_obj_t *c, const tschart_dataset_t *ds) {
    Impl *d = D(c);
    set_cursor(c, -1);
    if (d->has_ds) release_ds(&d->ds);
    d->ds = *ds;
    d->has_ds = true;
    d->start = ds->start;
    d->end = ds->end;
    d->pending = false;
    for (uint8_t i = 0; i < d->nsources; i++) {
        tschart_source &s = d->sources[i];
        s.data = {};
        for (uint8_t j = 0; j < ds->source_count; j++) {
            if (ds->sources[j].src == &s) s.data = ds->sources[j].data;
        }
        lv_free(s.times);
        s.times = nullptr;
        if (s.data.count == 0 || s.data.base == nullptr) {
            s.data.count = 0;
            continue;
        }
        s.times = (uint32_t *)lv_malloc(s.data.count * sizeof(uint32_t));
        if (s.times == nullptr) {
            s.data.count = 0;
            continue;
        }
        const uint8_t *base = (const uint8_t *)s.data.base;
        for (size_t k = 0; k < s.data.count; k++) {
            memcpy(&s.times[k], base + k * s.data.stride + s.data.time_offset, 4);
        }
    }
    for (uint8_t i = 0; i < d->nlayers; i++) {
        tschart_layer &l = d->layers[i];
        lv_free(l.vals);
        l.vals = nullptr;
        l.count = l.src->data.count;
        if (l.count == 0) continue;
        l.vals = (int32_t *)lv_malloc(l.count * sizeof(int32_t));
        if (l.vals == nullptr) {
            l.count = 0;
            continue;
        }
        for (size_t k = 0; k < l.count; k++) l.vals[k] = read_field(l, k);
        if (l.cfg.interp == TSCHART_INTERP_PLATEAU) {
            int32_t *raw = (int32_t *)lv_malloc(l.count * sizeof(int32_t));
            if (raw) {
                memcpy(raw, l.vals, l.count * sizeof(int32_t));
                interpolate_plateaus(l.src->times, raw, l.count, l.src->data.gap_seconds,
                                     l.vals);
                lv_free(raw);
            }
        }
    }
    if (d->ds.applied) d->ds.applied(c, d->ds.release_ud);
    update_status(c);
    invalidate_static(c);
}

bool tschart_set_dataset(lv_obj_t *c, const tschart_dataset_t *ds) {
    Impl *d = D(c);
    tschart_dataset_t copy = *ds;
    if (ds->source_count > TSCHART_MAX_SOURCES) {
        LV_LOG_WARN("tschart: %u sources > TSCHART_MAX_SOURCES", ds->source_count);
        release_ds(&copy);
        return false;
    }
    const bool stale = ds->request_id != 0
                           ? (!d->pending || ds->request_id != d->request_id)
                           : d->pending;
    if (stale) {
        release_ds(&copy);
        return false;
    }
    if (d->down || d->springing) {
        if (d->has_deferred) release_ds(&d->deferred);
        d->deferred = copy;
        d->has_deferred = true;
        return true;
    }
    apply_dataset(c, &copy);
    return true;
}

void tschart_set_status(lv_obj_t *c, tschart_status_t st, const char *text) {
    Impl *d = D(c);
    d->status = st;
    copy_str(d->status_text, sizeof(d->status_text), text);
    update_status(c);
}

tschart_pane_t *tschart_add_pane(lv_obj_t *c, const tschart_pane_cfg_t *cfg) {
    Impl *d = D(c);
    if (d->npanes >= TSCHART_MAX_PANES) return nullptr;
    tschart_pane &p = d->panes[d->npanes];
    p.chart = c;
    p.index = d->npanes++;
    p.cfg = *cfg;
    copy_str(p.id, sizeof(p.id), cfg->id);
    copy_str(p.unit, sizeof(p.unit), cfg->unit_tag);
    p.cfg.id = p.id;
    p.cfg.unit_tag = p.unit;
    p.pill = lv_obj_create(c);
    style_pill(p.pill, cfg->color, col_panel(c));
    lv_obj_set_size(p.pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(p.pill, 10, 0);
    lv_obj_set_style_pad_ver(p.pill, 2, 0);
    lv_obj_set_ext_click_area(p.pill, 8);
    p.pill_label = lv_label_create(p.pill);
    lv_obj_add_event_cb(p.pill, pill_cb, LV_EVENT_CLICKED, c);
    lv_obj_add_flag(p.pill, LV_OBJ_FLAG_HIDDEN);
    retag(c);
    lv_obj_set_user_data(p.pill, p.tag);
    invalidate_static(c);
    return &p;
}

tschart_layer_t *tschart_add_layer(tschart_pane_t *p, const tschart_layer_cfg_t *cfg) {
    lv_obj_t *c = p->chart;
    Impl *d = D(c);
    if (d->nlayers >= TSCHART_MAX_LAYERS) return nullptr;
    tschart_layer &l = d->layers[d->nlayers++];
    l.chart = c;
    l.pane = p;
    l.cfg = *cfg;
    copy_str(l.id, sizeof(l.id), cfg->id);
    l.cfg.id = l.id;
    tschart_layer_set_names(&l, cfg->name, cfg->tag_name);
    l.src = cfg->source;
    if (l.src == nullptr) {
        if (d->nsources == 0) tschart_add_source(c, "default");
        l.src = default_source(d);
    }
    if (l.cfg.width == 0) l.cfg.width = 3;
    invalidate_static(c);
    return &l;
}

void tschart_pane_set_unit_tag(tschart_pane_t *p, const char *unit_tag) {
    copy_str(p->unit, sizeof(p->unit), unit_tag);
    update_pills(p->chart);
}

void tschart_layer_set_names(tschart_layer_t *l, const char *name, const char *tag_name) {
    copy_str(l->name, sizeof(l->name), name);
    copy_str(l->tag_name, sizeof(l->tag_name), tag_name);
    l->has_tag = tag_name != nullptr;
    l->cfg.name = l->name;
    l->cfg.tag_name = l->has_tag ? l->tag_name : nullptr;
    Impl *d = D(l->chart);
    if (d->cursor >= 0) {
        build_card(l->chart);
        lv_obj_invalidate(l->chart);
    }
}

tschart_group_t *tschart_add_group(lv_obj_t *c, const char *id, const char *label,
                                   lv_color_t color, tschart_swatch_t swatch) {
    Impl *d = D(c);
    if (d->ngroups >= TSCHART_MAX_GROUPS) return nullptr;
    tschart_group &g = d->groups[d->ngroups];
    g.chart = c;
    g.index = d->ngroups++;
    copy_str(g.id, sizeof(g.id), id);
    copy_str(g.label, sizeof(g.label), label);
    g.color = color;
    g.swatch = swatch;
    g.visible = true;

    g.chip = lv_obj_create(d->header);
    style_pill(g.chip, color, col_panel(c));
    lv_obj_set_size(g.chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(g.chip, 14, 0);
    lv_obj_set_style_pad_ver(g.chip, 5, 0);
    lv_obj_set_style_pad_column(g.chip, 8, 0);
    lv_obj_set_flex_flow(g.chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g.chip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    g.chip_swatch = passive_child(g.chip);
    lv_obj_set_style_bg_color(g.chip_swatch, color, 0);
    if (swatch == TSCHART_SWATCH_FILL) {
        lv_obj_set_size(g.chip_swatch, 16, 16);
        lv_obj_set_style_radius(g.chip_swatch, 3, 0);
    } else {
        lv_obj_set_size(g.chip_swatch, 20, 5);
        lv_obj_set_style_radius(g.chip_swatch, 2, 0);
    }
    g.chip_label = lv_label_create(g.chip);
    // Chips go before the spacer, tags button and key row.
    lv_obj_move_to_index(g.chip, g.index);
    lv_obj_add_event_cb(g.chip, chip_cb, LV_EVENT_CLICKED, &g);
    retag(c);
    lv_obj_set_user_data(g.chip, g.tag);
    update_header(c);
    invalidate_static(c);
    return &g;
}

void tschart_group_add_pane(tschart_group_t *g, tschart_pane_t *p) {
    p->groups |= (uint8_t)(1u << g->index);
    invalidate_static(g->chart);
}

void tschart_group_add_layer(tschart_group_t *g, tschart_layer_t *l) {
    l->groups |= (uint8_t)(1u << g->index);
    invalidate_static(g->chart);
}

void tschart_group_set_visible(tschart_group_t *g, bool on) {
    if (g->visible == on) return;
    g->visible = on;
    set_cursor(g->chart, -1);
    update_chip(g);
    invalidate_static(g->chart);
}

bool tschart_group_get_visible(const tschart_group_t *g) { return g->visible; }

void tschart_group_set_label(tschart_group_t *g, const char *label) {
    copy_str(g->label, sizeof(g->label), label);
    update_chip(g);
    invalidate_static(g->chart);  // the header may rewrap
}

tschart_group_t *tschart_get_group(lv_obj_t *c, const char *id) {
    Impl *d = D(c);
    for (uint8_t i = 0; i < d->ngroups; i++) {
        if (strcmp(d->groups[i].id, id) == 0) return &d->groups[i];
    }
    return nullptr;
}

const char *tschart_group_get_id(const tschart_group_t *g) { return g->id; }

void tschart_set_spans(lv_obj_t *c, uint8_t set_id, const tschart_span_t *s, size_t n) {
    Impl *d = D(c);
    if (set_id >= TSCHART_MAX_SPAN_SETS) return;
    SpanSet &set = d->spans[set_id];
    lv_free(set.spans);
    set.spans = nullptr;
    set.count = 0;
    if (n && s) {
        set.spans = (tschart_span_t *)lv_malloc(n * sizeof(*s));
        if (set.spans) {
            memcpy(set.spans, s, n * sizeof(*s));
            set.count = n;
        }
    }
    invalidate_static(c);
}

void tschart_set_notes(lv_obj_t *c, const tschart_note_t *n, size_t count,
                       const char *prefix) {
    Impl *d = D(c);
    lv_free(d->notes);
    d->notes = nullptr;
    d->nnotes = 0;
    copy_str(d->note_prefix, sizeof(d->note_prefix), prefix);
    if (count && n) {
        d->notes = (Note *)lv_malloc(count * sizeof(Note));
        if (d->notes) {
            for (size_t i = 0; i < count; i++) {
                d->notes[i].time = n[i].time;
                copy_str(d->notes[i].text, sizeof(d->notes[i].text), n[i].text);
                d->notes[i].color = n[i].color;
            }
            d->nnotes = count;
        }
    }
    invalidate_static(c);
}

void tschart_add_key(lv_obj_t *c, const char *id, const char *label, lv_color_t color,
                     tschart_span_style_t style) {
    Impl *d = D(c);
    if (d->nkeys >= TSCHART_MAX_KEYS) return;
    Key &k = d->keys[d->nkeys++];
    copy_str(k.id, sizeof(k.id), id);
    copy_str(k.label, sizeof(k.label), label);
    k.color = color;
    k.style = style;
    k.obj = passive_child(d->key_row);
    lv_obj_set_size(k.obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(k.obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(k.obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(k.obj, 6, 0);
    lv_obj_t *sw = passive_child(k.obj);
    lv_obj_set_size(sw, 12, 12);
    lv_obj_set_style_radius(sw, 2, 0);
    lv_obj_set_style_bg_color(sw, color, 0);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
    if (style == TSCHART_SPAN_HATCH) {
        lv_obj_set_style_bg_opa(sw, LV_OPA_30, 0);
        lv_obj_set_style_border_color(sw, color, 0);
        lv_obj_set_style_border_width(sw, 2, 0);
    }
    lv_obj_t *label_obj = lv_label_create(k.obj);
    lv_label_set_text(label_obj, k.label);
    lv_obj_set_style_text_color(label_obj, col_dim(), 0);
    update_header(c);
    invalidate_static(c);
}

void tschart_clear_keys(lv_obj_t *c) {
    Impl *d = D(c);
    for (uint8_t i = 0; i < d->nkeys; i++) lv_obj_delete(d->keys[i].obj);
    d->nkeys = 0;
    update_header(c);
    invalidate_static(c);
}

void tschart_set_header(lv_obj_t *c, uint8_t flags) {
    D(c)->header_flags = flags;
    update_header(c);
    invalidate_static(c);
}

void tschart_get_plot_area(const lv_obj_t *c, lv_area_t *out) { *out = impl(c)->plot; }

void tschart_set_axis_open(lv_obj_t *c, bool open) {
    D(c)->axis_open = open;
    update_pills(c);
    lv_obj_invalidate(c);
}

bool tschart_get_axis_open(const lv_obj_t *c) { return impl(c)->axis_open; }

void tschart_set_end_tags(lv_obj_t *c, bool on) {
    D(c)->end_tags = on;
    update_header(c);
    lv_obj_invalidate(c);
}

bool tschart_get_end_tags(const lv_obj_t *c) { return impl(c)->end_tags; }

void tschart_card_add(tschart_card_t *card, tschart_row_kind_t kind, const char *label,
                      const char *value, lv_color_t color) {
    if (card->count >= CARD_MAX_ROWS) {
        card->overflow = true;
        return;
    }
    tschart_card_row &r = card->rows[card->count++];
    r.kind = kind;
    copy_str(r.label, sizeof(r.label), label);
    copy_str(r.value, sizeof(r.value), value);
    r.color = color;
}

void tschart_card_add_layers(tschart_card_t *card) { card->layers_at = (int8_t)card->count; }

void tschart_set_card_cb(lv_obj_t *c, tschart_card_cb_t cb, void *ud) {
    D(c)->card_cb = cb;
    D(c)->card_ud = ud;
}

void tschart_clear_cursor(lv_obj_t *c) { set_cursor(c, -1); }

int32_t tschart_set_cursor_x(lv_obj_t *c, int32_t x) {
    set_cursor(c, cursor_at(c, x));
    return D(c)->cursor;
}

void tschart_profile_reset(lv_obj_t *c) { D(c)->prof = {}; }

void tschart_profile_get(const lv_obj_t *c, tschart_profile_t *out) { *out = impl(c)->prof; }
