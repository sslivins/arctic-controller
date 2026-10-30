/*
 * tschart -- reusable LVGL 9.5 time-series chart.
 *
 * Stacked panes sharing one time axis, each with its own y scale; a chip
 * header that is both legend and visibility toggles; a collapsible y-axis
 * gutter; line-end value tags; pan with snap and rubber band; tap/scrub
 * cursor with a readout card. Everything but the cursor and overlays is
 * drawn once into a cached image, so scrubbing and panning stay smooth.
 *
 * Contract:
 *  - Every tschart_* call runs on the LVGL thread / under the LVGL lock.
 *  - Config structs and strings are copied. Call the setter again to change
 *    text (e.g. after a language switch).
 *  - Sample memory is borrowed: it must stay valid and unchanged until the
 *    dataset's release callback runs (dataset replaced or chart deleted).
 *  - Handles (pane, layer, group, source) are owned by the chart.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CONFIG_TSCHART_MAX_SOURCES
#define CONFIG_TSCHART_MAX_SOURCES 4
#endif
#define TSCHART_MAX_SOURCES CONFIG_TSCHART_MAX_SOURCES

#define TSCHART_MAX_PANES  6
#define TSCHART_MAX_LAYERS 12
#define TSCHART_MAX_GROUPS 8
#define TSCHART_MAX_KEYS   8
#define TSCHART_MAX_SPAN_SETS 4

/** "No value" in derived arrays and formatter input. */
#define TSCHART_NONE INT32_MIN
#define TSCHART_NO_FLAGS SIZE_MAX

typedef uint32_t tschart_time_t;  /* unix seconds */

typedef struct tschart_source tschart_source_t;
typedef struct tschart_pane tschart_pane_t;
typedef struct tschart_layer tschart_layer_t;
typedef struct tschart_group tschart_group_t;
typedef struct tschart_card tschart_card_t;

/* ---- Creation ---------------------------------------------------------- */

lv_obj_t *tschart_create(lv_obj_t *parent);

/** Test-tag prefix, e.g. "temperature_history_chart". Children get
 *  <id>_chip_<group>, <id>_tags_btn, <id>_axis_<pane>, <id>_spinner,
 *  <id>_status. */
void tschart_set_id(lv_obj_t *c, const char *id);

/** Font for axis labels, end tags and time labels. The object's text_font
 *  is used for the card, chips and status. */
void tschart_set_small_font(lv_obj_t *c, const lv_font_t *font);

/* ---- Time, bounds and windows ------------------------------------------- */

typedef size_t (*tschart_time_fmt_cb_t)(tschart_time_t t, uint32_t tick_step,
                                        char *buf, size_t cap, void *ud);

void tschart_set_bounds(lv_obj_t *c, tschart_time_t earliest, tschart_time_t latest);
/** Labels for the hatched zones before `earliest` / after `latest`. */
void tschart_set_edge_text(lv_obj_t *c, const char *before, const char *after);
void tschart_set_time_formatter(lv_obj_t *c, tschart_time_fmt_cb_t cb, void *ud);
void tschart_set_pan_snap(lv_obj_t *c, uint32_t snap_seconds);  /* 0 = none */

/** Host-initiated navigation (buttons, zoom, initial load). Marks the
 *  window pending (spinner; the old image shown moved into place when the
 *  length matches) and returns the request id to put in the dataset. */
uint32_t tschart_request_window(lv_obj_t *c, tschart_time_t start, tschart_time_t end);
/** Drops the pending window (e.g. the query failed). */
void tschart_cancel_pending(lv_obj_t *c);
/** Committed window (the one the shown data is for). */
void tschart_get_window(const lv_obj_t *c, tschart_time_t *start, tschart_time_t *end);
/** Pending window if a request is in flight, else the committed one. */
void tschart_get_target_window(const lv_obj_t *c, tschart_time_t *start,
                               tschart_time_t *end);

/* ---- Data ---------------------------------------------------------------- */

typedef enum { TSCHART_I16, TSCHART_I32, TSCHART_U16, TSCHART_U32 } tschart_num_t;

typedef struct {
    const void *base;
    size_t count;
    size_t stride;        /* sizeof(sample) */
    size_t time_offset;   /* offsetof(sample, timestamp), u32 */
    uint32_t gap_seconds; /* lines break across larger gaps */
} tschart_source_data_t;

typedef int32_t (*tschart_xform_cb_t)(int32_t raw, void *ud);

typedef struct {
    size_t offset;          /* offsetof(sample, field) */
    tschart_num_t type;
    size_t flags_offset;    /* u8 validity bits, TSCHART_NO_FLAGS = always valid */
    uint8_t flags_mask;
    tschart_xform_cb_t xform; /* optional, to display units (deci-C -> deci-F) */
    void *xform_ud;
} tschart_field_t;

typedef struct {
    tschart_source_t *src;
    tschart_source_data_t data;
} tschart_dataset_source_t;

typedef struct {
    uint32_t request_id;    /* 0 = unsolicited (live update) */
    tschart_time_t start, end;
    tschart_dataset_source_t sources[TSCHART_MAX_SOURCES];
    uint8_t source_count;
    void (*release)(void *ud);
    void *release_ud;
    /* Optional: runs when the dataset becomes the shown one (possibly later
     * than set_dataset, after a gesture), before the redraw -- the place to
     * set this window's spans, notes and status. ud = release_ud. */
    void (*applied)(lv_obj_t *c, void *ud);
} tschart_dataset_t;

tschart_source_t *tschart_add_source(lv_obj_t *c, const char *id); /* first = default */

/** Swaps window and data atomically. Returns false (and releases ds) when
 *  the request id is stale. Arriving mid-gesture, it's applied when the
 *  finger lifts. */
bool tschart_set_dataset(lv_obj_t *c, const tschart_dataset_t *ds);

typedef enum {
    TSCHART_STATUS_OK,
    TSCHART_STATUS_LOADING,
    TSCHART_STATUS_EMPTY,
    TSCHART_STATUS_ERROR,
} tschart_status_t;

/** Centred message in the plot (EMPTY/ERROR); LOADING shows the spinner. */
void tschart_set_status(lv_obj_t *c, tschart_status_t st, const char *text);

/* ---- Panes and layers ---------------------------------------------------- */

typedef enum { TSCHART_FMT_AXIS, TSCHART_FMT_TAG, TSCHART_FMT_CURSOR } tschart_fmt_ctx_t;
/** v is in display units (after xform); TSCHART_NONE = no value. */
typedef size_t (*tschart_value_fmt_cb_t)(int32_t v, tschart_fmt_ctx_t ctx,
                                         char *buf, size_t cap, void *ud);

typedef struct {
    const char *id;
    uint16_t weight;
    uint16_t min_height;
    const char *unit_tag;      /* pill text, e.g. "°C", "kW" */
    lv_color_t color;          /* pill and open-axis colour */
    bool auto_range;
    bool has_min, has_max;     /* fixed bounds; in auto mode, always included */
    int32_t min, max;
    int32_t min_span;          /* auto: never tighter */
    bool include_zero;
    uint16_t pad_permille;     /* auto: padding each side */
    int32_t min_pad;
    int32_t min_step;          /* smallest tick step */
    uint8_t max_ticks;
    tschart_value_fmt_cb_t fmt;
    void *fmt_ud;
} tschart_pane_cfg_t;

typedef enum { TSCHART_GEOM_LINE, TSCHART_GEOM_STEP } tschart_geom_t;
typedef enum { TSCHART_INTERP_NONE, TSCHART_INTERP_PLATEAU } tschart_interp_t;

typedef struct {
    const char *id;
    const char *name;          /* cursor card row */
    const char *tag_name;      /* end tag prefix; NULL = no tag */
    tschart_source_t *source;  /* NULL = default */
    tschart_field_t field;
    tschart_geom_t geom;
    tschart_interp_t interp;
    bool fill;                 /* to baseline, max per pixel column */
    lv_opa_t fill_opa;
    lv_color_t color;
    uint8_t width;
    bool in_cursor;
    bool drives_range;
} tschart_layer_cfg_t;

tschart_pane_t *tschart_add_pane(lv_obj_t *c, const tschart_pane_cfg_t *cfg);
tschart_layer_t *tschart_add_layer(tschart_pane_t *p, const tschart_layer_cfg_t *cfg);
void tschart_pane_set_unit_tag(tschart_pane_t *p, const char *unit_tag);
void tschart_layer_set_names(tschart_layer_t *l, const char *name, const char *tag_name);

/* ---- Groups (chips) ------------------------------------------------------ */

typedef enum { TSCHART_SWATCH_LINE, TSCHART_SWATCH_FILL } tschart_swatch_t;

tschart_group_t *tschart_add_group(lv_obj_t *c, const char *id, const char *label,
                                   lv_color_t color, tschart_swatch_t swatch);
void tschart_group_add_pane(tschart_group_t *g, tschart_pane_t *p);
void tschart_group_add_layer(tschart_group_t *g, tschart_layer_t *l);
void tschart_group_set_visible(tschart_group_t *g, bool on);  /* no event */
bool tschart_group_get_visible(const tschart_group_t *g);
void tschart_group_set_label(tschart_group_t *g, const char *label);
tschart_group_t *tschart_get_group(lv_obj_t *c, const char *id);
const char *tschart_group_get_id(const tschart_group_t *g);

/* ---- Overlays ------------------------------------------------------------ */

typedef enum {
    TSCHART_SPAN_FILL,
    TSCHART_SPAN_HATCH,        /* fault: hatched with a solid start line */
} tschart_span_style_t;

typedef struct {
    tschart_time_t start, end; /* end == 0: open to the window end */
    lv_color_t color;
    lv_opa_t opa;
    uint8_t style;             /* tschart_span_style_t */
    uint8_t min_width;         /* px, so short spans still show */
} tschart_span_t;

typedef struct {
    tschart_time_t time;
    const char *text;          /* copied */
    lv_color_t color;
} tschart_note_t;

/** Spans are copied; sets draw in set_id order (0 first). */
void tschart_set_spans(lv_obj_t *c, uint8_t set_id, const tschart_span_t *s, size_t n);
/** Notes above the plot (fault codes); notes that would collide share one
 *  label, the text if they're all the same, otherwise "×N". */
void tschart_set_notes(lv_obj_t *c, const tschart_note_t *n, size_t count,
                       const char *prefix);
void tschart_add_key(lv_obj_t *c, const char *id, const char *label,
                     lv_color_t color, tschart_span_style_t style);
void tschart_clear_keys(lv_obj_t *c);

/* ---- Header, axis, tags -------------------------------------------------- */

enum { TSCHART_HEADER_CHIPS = 1, TSCHART_HEADER_KEY = 2, TSCHART_HEADER_TAG_BTN = 4 };
void tschart_set_header(lv_obj_t *c, uint8_t flags);
void tschart_get_plot_area(const lv_obj_t *c, lv_area_t *out);

void tschart_set_axis_open(lv_obj_t *c, bool open);  /* no event */
bool tschart_get_axis_open(const lv_obj_t *c);
void tschart_set_end_tags(lv_obj_t *c, bool on);     /* no event */
bool tschart_get_end_tags(const lv_obj_t *c);

/* ---- Cursor -------------------------------------------------------------- */

typedef enum {
    TSCHART_ROW_TITLE,    /* dim, top */
    TSCHART_ROW_BLOCK,    /* block swatch + coloured label, value right */
    TSCHART_ROW_TEXT,     /* dim text */
    TSCHART_ROW_VALUE,    /* line swatch + label, value right */
    TSCHART_ROW_WARNING,  /* error colour */
} tschart_row_kind_t;

void tschart_card_add(tschart_card_t *card, tschart_row_kind_t kind,
                      const char *label, const char *value, lv_color_t color);
/** Inserts the in_cursor layers' rows here (else they're appended). */
void tschart_card_add_layers(tschart_card_t *card);

typedef struct {
    tschart_time_t time;
    tschart_source_t *src;
    size_t index;               /* nearest sample in the default source */
} tschart_cursor_info_t;

typedef void (*tschart_card_cb_t)(lv_obj_t *c, const tschart_cursor_info_t *info,
                                  tschart_card_t *card, void *ud);
void tschart_set_card_cb(lv_obj_t *c, tschart_card_cb_t cb, void *ud);
void tschart_clear_cursor(lv_obj_t *c);
/** Moves the cursor as a tap at screen x would; returns the sample index
 *  or -1. */
int32_t tschart_set_cursor_x(lv_obj_t *c, int32_t x);

/* ---- Events -------------------------------------------------------------- */

/* Registered on the first tschart_create(). */
extern lv_event_code_t TSCHART_EVENT_WINDOW_REQUEST;
extern lv_event_code_t TSCHART_EVENT_DOUBLE_TAP;
extern lv_event_code_t TSCHART_EVENT_GROUP_TOGGLED;
extern lv_event_code_t TSCHART_EVENT_AXIS_TOGGLED;
extern lv_event_code_t TSCHART_EVENT_TAGS_TOGGLED;
extern lv_event_code_t TSCHART_EVENT_CURSOR_CHANGED;

typedef struct { uint32_t request_id; tschart_time_t start, end; } tschart_window_request_t;
typedef struct { tschart_time_t time; } tschart_double_tap_t;
typedef struct { tschart_group_t *group; bool visible; } tschart_group_toggled_t;
typedef struct { bool on; } tschart_toggled_t;
typedef struct { bool active; tschart_time_t time; size_t index; } tschart_cursor_changed_t;

/* ---- Profiling ----------------------------------------------------------- */

typedef struct {
    uint32_t draw_calls;
    int64_t draw_us;
    int64_t build_us;
    bool cached;
} tschart_profile_t;

/** Counters since the last reset. */
void tschart_profile_reset(lv_obj_t *c);
void tschart_profile_get(const lv_obj_t *c, tschart_profile_t *out);

#ifdef __cplusplus
}
#endif
