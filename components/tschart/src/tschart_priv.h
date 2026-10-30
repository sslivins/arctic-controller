/* tschart internals shared by tschart.cpp (object, API, input) and
 * tschart_draw.cpp (rendering). */
#pragma once

#include "tschart.h"
#include "tschart_logic.h"
#include "tschart_lv.h"

namespace tschart {

static constexpr int32_t SIDE_PAD = 14;      // plot inset left/right
static constexpr int32_t HEADER_PAD = 10;    // header inset
static constexpr int32_t TIME_AXIS_H = 40;   // time labels under the plot
static constexpr int32_t NOTE_STRIP_H = 38;  // fault codes above the plot
static constexpr int32_t PANE_GAP = 10;
static constexpr int32_t GUTTER_W = 66;      // open y axis
static constexpr int32_t PAN_START_PX = 14;
static constexpr int32_t CURSOR_SNAP_PX = 30;
static constexpr uint32_t SPRING_MS = 180;
static constexpr uint32_t DOUBLE_TAP_AFTER_PAN_MS = 600;
static constexpr int32_t NOTE_LABEL_W = 100;
static constexpr int32_t CARD_ROW_H = 30;
static constexpr int32_t CARD_PAD = 10;
static constexpr int32_t CARD_MAX_ROWS = 10;
static constexpr size_t ID_LEN = 24;
static constexpr size_t TEXT_LEN = 40;
static constexpr size_t TAG_LEN = 64;     // test tag: <chart id>_<kind>_<id>

}  // namespace tschart

struct tschart_source {
    lv_obj_t *chart;
    char id[tschart::ID_LEN];
    tschart_source_data_t data;
    uint32_t *times;           // decoded timestamps, data.count of them
};

struct tschart_pane {
    lv_obj_t *chart;
    uint8_t index;
    char id[tschart::ID_LEN];
    char unit[16];
    tschart_pane_cfg_t cfg;    // id/unit_tag point at the copies above
    uint8_t groups;            // bit per group holding the pane
    bool visible;
    tschart::PaneSlot slot;    // screen coords
    tschart::Axis axis;
    lv_obj_t *pill;
    lv_obj_t *pill_label;
    char tag[tschart::TAG_LEN];
};

struct tschart_layer {
    lv_obj_t *chart;
    tschart_pane_t *pane;
    tschart_source_t *src;
    char id[tschart::ID_LEN];
    char name[tschart::TEXT_LEN];
    char tag_name[16];
    bool has_tag;
    tschart_layer_cfg_t cfg;
    uint8_t groups;
    bool visible;
    int32_t *vals;             // display units for drawing; NONE = missing
    size_t count;
};

struct tschart_group {
    lv_obj_t *chart;
    uint8_t index;
    char id[tschart::ID_LEN];
    char label[tschart::TEXT_LEN];
    lv_color_t color;
    tschart_swatch_t swatch;
    bool visible;
    lv_obj_t *chip;
    lv_obj_t *chip_swatch;
    lv_obj_t *chip_label;
    char tag[tschart::TAG_LEN];
};

struct tschart_card_row {
    tschart_row_kind_t kind;
    char label[tschart::TEXT_LEN];
    char value[24];
    lv_color_t color;
};

struct tschart_card {
    tschart_card_row rows[tschart::CARD_MAX_ROWS];
    uint8_t count;
    int8_t layers_at;          // -1 = append
    bool overflow;
};

namespace tschart {

struct Note {
    uint32_t time;
    char text[24];
    lv_color_t color;
};

struct Key {
    char id[ID_LEN];
    char label[TEXT_LEN];
    lv_color_t color;
    tschart_span_style_t style;
    lv_obj_t *obj;
};

struct SpanSet {
    tschart_span_t *spans;
    size_t count;
};

struct CacheKey {
    uint32_t static_gen, start, end;
    int32_t w, h;
    bool operator==(const CacheKey &) const = default;
};

struct Impl {
    lv_obj_t *self;
    char id[TAG_LEN];
    const lv_font_t *small_font;

    uint32_t earliest, latest;
    char edge_before[TEXT_LEN], edge_after[TEXT_LEN];
    tschart_time_fmt_cb_t time_fmt;
    void *time_fmt_ud;
    uint32_t snap;

    // Windows: committed = what the data is for; pending = requested.
    uint32_t start, end;
    bool pending;
    uint32_t pend_start, pend_end;
    uint32_t request_id;

    tschart_source sources[TSCHART_MAX_SOURCES];
    uint8_t nsources;
    tschart_dataset_t ds;
    bool has_ds;
    tschart_dataset_t deferred;
    bool has_deferred;

    tschart_pane panes[TSCHART_MAX_PANES];
    uint8_t npanes;
    tschart_layer layers[TSCHART_MAX_LAYERS];
    uint8_t nlayers;
    tschart_group groups[TSCHART_MAX_GROUPS];
    uint8_t ngroups;

    SpanSet spans[TSCHART_MAX_SPAN_SETS];
    Note *notes;
    size_t nnotes;
    char note_prefix[12];
    Key keys[TSCHART_MAX_KEYS];
    uint8_t nkeys;

    uint8_t header_flags;
    lv_obj_t *header;
    lv_obj_t *key_row;
    lv_obj_t *tags_btn;
    char tags_btn_tag[TAG_LEN];
    bool axis_open;
    bool end_tags;

    tschart_status_t status;
    char status_text[64];
    lv_obj_t *spinner;
    lv_obj_t *status_label;
    char spinner_tag[TAG_LEN];
    char status_tag[TAG_LEN];

    // Geometry (screen coords), recomputed by layout().
    lv_area_t plot;
    bool have_plot;

    // Cursor.
    tschart_card_cb_t card_cb;
    void *card_ud;
    int32_t cursor;            // index into the default source, -1 = none
    tschart_card card;
    lv_area_t cursor_area[2];
    bool cursor_area_valid;

    // Static image cache.
    uint32_t static_gen;
    bool layout_dirty;
    lv_timer_t *timer;
    lv_obj_t *canvas;
    lv_draw_buf_t buf;
    void *buf_data;
    size_t buf_size;
    bool cache_valid;
    CacheKey cache_key;

    // Gesture.
    int32_t x0, dx;
    bool panning, scrubbing, down, springing, press_in_plot;
    uint32_t pan_ended_ms;

    tschart_profile_t prof;
};

struct Chart {
    lv_obj_t obj;
    Impl *d;
};

inline Impl *impl(const lv_obj_t *c) { return ((const Chart *)c)->d; }

// tschart.cpp
void layout(lv_obj_t *c);
void schedule(lv_obj_t *c);
bool loading(const Impl *d);
void target_window(const Impl *d, uint32_t *start, uint32_t *end);
tschart_source_t *default_source(Impl *d);
int32_t read_field(const tschart_layer &l, size_t i);

// tschart_draw.cpp
void draw_main(lv_obj_t *c, lv_layer_t *layer);
void draw_post(lv_obj_t *c, lv_layer_t *layer);
void cache_build(lv_obj_t *c);
bool cache_ready(lv_obj_t *c);
bool cache_shift(lv_obj_t *c, int32_t *shift_px);
void cache_free(Impl *d);
void build_card(lv_obj_t *c);
bool cursor_bounds(lv_obj_t *c, lv_area_t *out);
int32_t value_at(const tschart_layer &l, uint32_t t, size_t default_index,
                 bool default_src);
lv_color_t col_panel(lv_obj_t *c);
lv_color_t col_grid(lv_obj_t *c);
lv_color_t col_text(lv_obj_t *c);
lv_color_t col_dim();
size_t fmt_value(const tschart_pane &p, int32_t v, tschart_fmt_ctx_t ctx,
                 char *buf, size_t cap);

}  // namespace tschart
