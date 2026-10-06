/*
 * Arctic Heat Pump Controller - Startup Animation
 *
 * The Arctic mark sits frozen in a block of ice; the heat pump "comes on" (a
 * warm glow rises from below and HEAT PUMPS lights up), the icicles start to
 * drip and the ice melts away from the top down, revealing the crisp logo.
 *
 * The static layers (logo, frozen logo, tagline, ice texture) are pre-rendered
 * by tools/boot_art/gen_boot_art.py and embedded LZ4-compressed
 * (main/boot_art/boot_art.bin). The timeline mirrors the approved preview and
 * is driven by wall-clock time, so a slow frame skips ahead rather than
 * stretching the animation.
 *
 * Performance: LVGL renders in software into PSRAM, where blending a large
 * layer costs tens of milliseconds, so the scene (background, heat glow, logo
 * and ice) is composited here with integer math into one opaque RGB565 canvas
 * that LVGL merely copies, and each frame only recomposites what changed:
 *  - the frozen logo is baked into the ice texture, so the melt edge reveals
 *    the crisp logo underneath and only the band around the edge is redrawn;
 *  - the heat glow is redrawn only while its level changes, at most ~15 Hz;
 *  - the frost sheen redraws just the strips it crosses;
 *  - styles are only set when their value changes, since every style set
 *    invalidates the object.
 */
#include "startup_anim.h"

#include <lvgl.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

static const char* TAG = "startup_anim";

extern const uint8_t boot_art_bin_start[] asm("_binary_boot_art_bin_start");
extern const uint8_t boot_art_bin_end[] asm("_binary_boot_art_bin_end");

// Geometry the art was rendered for (see gen_boot_art.py).
#define ART_W 720
#define ART_H 1280
#define ICE_X 60
#define ICE_Y 260
#define ICE_W 600
#define ICE_H 570
#define ICICLE_ZONE 150                     // canvas rows below the block for icicles
#define CANVAS_H (ICE_H + ICICLE_ZONE)
#define ICICLE_TOP (ICE_H - 16)             // icicles grow out of the block's underside
#define GLOW_Y0 724                         // first screen row the heat glow can reach
#define GLOW_MAX 141                        // peak glow intensity (0.55 * 255) + 1
#define GLOW_PERIOD 0.066f                  // min seconds between glow redraws

#define BG_TOP_R 10
#define BG_TOP_G 16
#define BG_TOP_B 30
#define BG_BOT_R 27
#define BG_BOT_G 48
#define BG_BOT_B 80

#define NUM_CRYSTALS 10
#define MAX_ICICLES 16
#define MAX_DROPS 24
#define DROP_SPRITE 24
#define DROP_R 10.0f

#define EDGE_LUT_STEPS 8                    // melt-edge LUT resolution (1/8 px)
#define EDGE_LUT_LEN (20 * EDGE_LUT_STEPS)  // edge effects vanish 20 px into the ice
#define SHEEN_HALF 120                      // sheen band half-width in px
#define SHEEN_STRIP 96                      // rows per sheen invalidation strip
#define U_LUT 256                           // icicle cross-section LUT resolution

static constexpr float T_TOTAL = 3.4f;
static constexpr float SHEEN_END = 1.1f;

enum ArtLayer { ART_LOGO = 0, ART_FROZEN, ART_TAG, ART_ICE, ART_COUNT };

struct Icicle {
    float x, hw, ln, bend, ph;
    float drips[2];
    int ndrips;
};

struct Crystal {
    lv_obj_t* obj;
    float x, y, r, ph;
};

static struct {
    bool running = false;
    void (*on_complete)(void) = nullptr;
    int64_t start_us = 0;  // set on the first update so the whole timeline plays
    int64_t last_frame_us = 0;
    int ox = 0, oy = 0;  // offset of the 720x1280 art on the actual screen

    lv_image_dsc_t art[ART_COUNT] = {};
    int16_t art_x[ART_COUNT] = {}, art_y[ART_COUNT] = {};
    uint8_t* art_buf[ART_COUNT] = {};

    uint32_t* ice_base = nullptr;  // ARGB8888 ice with the frozen logo baked in
    uint16_t* bg_buf = nullptr;    // RGB565 displayed scene: background + glow + logo + ice
    uint8_t* glow_i = nullptr;     // glow intensity per pixel for rows >= GLOW_Y0
    uint32_t* drop_buf = nullptr;
    lv_image_dsc_t drop_dsc = {};
    int glow_y_min = ART_H;  // first row with any glow
    int glow_level = -1;
    float glow_t = -1.0f;    // time of the last glow redraw

    lv_obj_t* bg = nullptr;
    lv_obj_t* tag = nullptr;
    lv_obj_t* drops[MAX_DROPS] = {};
    Crystal crystals[NUM_CRYSTALS] = {};
    int logo_opa = 255, tag_opa = -1;
    bool crystals_hidden = false;

    Icicle icicles[MAX_ICICLES] = {};
    int nicicles = 0;
    int icicle_c0 = 0, icicle_c1 = ICE_W;  // column span of all icicles
    int melt_r0 = 0, melt_r1 = 0;  // rows of last frame's melt band (empty when equal)
    float sheen_prev = NAN;        // last drawn sheen position, NAN when none
    float last_shrink = 0.0f;
    bool ice_hidden = false;
    // Ice state used by compose(), published by update_ice().
    bool ice_melting = false, ice_sheen = false;
    int ice_sx = 0;
    float shrink = 0.0f;

    // Compose worker on the second core.
    TaskHandle_t worker = nullptr;
    SemaphoreHandle_t job_done = nullptr;
    volatile bool worker_quit = false;
    int job[4] = {};
} ctx;

// Bulky per-row/per-column state and lookup tables. Allocated in PSRAM for
// the animation's lifetime only: as statics they cost ~13 KB of internal RAM,
// which the HTTPS server needs for its task stack.
struct AnimTables {
    uint8_t bg_rgb[ART_H][3];
    float top[ICE_W];
    float wobble[ICE_W];
    uint16_t edge_soft[EDGE_LUT_LEN];  // alpha scale, 0..256
    uint8_t edge_wet_a[EDGE_LUT_LEN];  // wet highlight alpha, 0..255
    uint8_t edge_wet_c[EDGE_LUT_LEN];  // wet highlight brightening, 0..255
    uint8_t sheen_lut[2 * SHEEN_HALF + 1];
    float u_inside[U_LUT], u_shade[U_LUT], u_hi[U_LUT];
};
static AnimTables* tab = nullptr;


// ---------------------------------------------------------------- helpers ----

static uint32_t s_rng = 0x2545F491u;
static float frand(float lo, float hi)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return lo + (hi - lo) * (float)(s_rng & 0xFFFFFF) / (float)0x1000000;
}

static inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
static inline float phase(float t, float a, float b) { return clamp01((t - a) / (b - a)); }
static inline float ease_out(float t) { return 1 - (1 - t) * (1 - t) * (1 - t); }
static inline float ease_in_out(float t) { return 3 * t * t - 2 * t * t * t; }
static inline float smoothstep(float e0, float e1, float v)
{
    v = clamp01((v - e0) / (e1 - e0));
    return v * v * (3 - 2 * v);
}
static inline lv_opa_t to_opa(float v) { return (lv_opa_t)(clamp01(v) * 255.0f + 0.5f); }
static inline uint16_t rgb565(uint32_t r, uint32_t g, uint32_t b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
// Ordered (4x4 Bayer) dither to RGB565; without it the dark gradient bands.
static const uint8_t BAYER4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
static inline uint16_t rgb565_dither(uint32_t r, uint32_t g, uint32_t b, int x, int y)
{
    const uint32_t d = BAYER4[y & 3][x & 3];
    r = std::min<uint32_t>(255, r + (d >> 1));
    g = std::min<uint32_t>(255, g + (d >> 2));
    b = std::min<uint32_t>(255, b + (d >> 1));
    return rgb565(r, g, b);
}

static void* psram_alloc(size_t n)
{
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void set_image_opa(lv_obj_t* obj, int* cache, lv_opa_t opa)
{
    if (*cache == opa) return;
    *cache = opa;
    lv_obj_set_style_image_opa(obj, opa, 0);
}

// ------------------------------------------------------------- art blob ----

// Minimal LZ4 block decoder (the art is raw LZ4 blocks, no frame header).
// Returns the decoded size, or -1 on malformed input.
static int lz4_decode(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_cap)
{
    const uint8_t* ip = src;
    const uint8_t* const iend = src + src_len;
    uint8_t* op = dst;
    uint8_t* const oend = dst + dst_cap;
    auto read_len = [&](long len) -> long {
        if (len != 15) return len;
        uint8_t b;
        do {
            if (ip >= iend) return -1;
            b = *ip++;
            len += b;
        } while (b == 255);
        return len;
    };
    while (ip < iend) {
        uint8_t token = *ip++;
        long lit = read_len(token >> 4);
        if (lit < 0 || lit > iend - ip || lit > oend - op) return -1;
        memcpy(op, ip, (size_t)lit);
        ip += lit;
        op += lit;
        if (ip >= iend) break;  // the last sequence carries literals only
        if (iend - ip < 2) return -1;
        long off = ip[0] | (ip[1] << 8);
        ip += 2;
        if (off == 0 || off > op - dst) return -1;
        long mlen = read_len(token & 0x0F);
        if (mlen < 0) return -1;
        mlen += 4;
        if (mlen > oend - op) return -1;
        const uint8_t* m = op - off;
        for (long i = 0; i < mlen; i++) *op++ = m[i];  // byte copy: matches may overlap
    }
    return (int)(op - dst);
}

static bool load_art()
{
    const uint8_t* blob = boot_art_bin_start;
    size_t blob_len = (size_t)(boot_art_bin_end - boot_art_bin_start);
    if (blob_len < 8 || memcmp(blob, "BART", 4) != 0) {
        ESP_LOGE(TAG, "boot art blob missing or corrupt");
        return false;
    }
    uint16_t count;
    memcpy(&count, blob + 6, 2);
    for (uint16_t i = 0; i < count; i++) {
        const uint8_t* e = blob + 8 + i * 24;
        uint16_t id, w, h;
        int16_t x, y;
        uint32_t raw_size, offset, comp_size;
        memcpy(&id, e, 2);
        memcpy(&x, e + 4, 2);
        memcpy(&y, e + 6, 2);
        memcpy(&w, e + 8, 2);
        memcpy(&h, e + 10, 2);
        memcpy(&raw_size, e + 12, 4);
        memcpy(&offset, e + 16, 4);
        memcpy(&comp_size, e + 20, 4);
        if (id >= ART_COUNT || offset + comp_size > blob_len || raw_size != (uint32_t)w * h * 3) {
            ESP_LOGE(TAG, "boot art layer %u invalid", id);
            return false;
        }
        uint8_t* buf = (uint8_t*)psram_alloc(raw_size);
        if (!buf) return false;
        int n = lz4_decode(blob + offset, comp_size, buf, raw_size);
        if (n != (int)raw_size) {
            ESP_LOGE(TAG, "boot art layer %u failed to decompress (%d)", id, n);
            heap_caps_free(buf);
            return false;
        }
        ctx.art_buf[id] = buf;
        ctx.art_x[id] = x;
        ctx.art_y[id] = y;
        lv_image_dsc_t& d = ctx.art[id];
        d.header.magic = LV_IMAGE_HEADER_MAGIC;
        d.header.cf = LV_COLOR_FORMAT_RGB565A8;
        d.header.w = w;
        d.header.h = h;
        d.header.stride = w * 2;
        d.data_size = raw_size;
        d.data = buf;
    }
    for (int i = 0; i < ART_COUNT; i++) {
        if (!ctx.art_buf[i]) {
            ESP_LOGE(TAG, "boot art layer %d missing", i);
            return false;
        }
    }
    return ctx.art[ART_ICE].header.w == ICE_W && ctx.art[ART_ICE].header.h == ICE_H;
}

static inline void rgb565a8_px(const uint8_t* buf, int w, int h, int i, float* r, float* g, float* b, float* a)
{
    uint16_t c = ((const uint16_t*)buf)[i];
    *r = (float)((c >> 11) & 0x1F) / 31.0f;
    *g = (float)((c >> 5) & 0x3F) / 63.0f;
    *b = (float)(c & 0x1F) / 31.0f;
    *a = (float)buf[w * h * 2 + i] / 255.0f;
}

// Composite ice over the frozen logo into one ARGB8888 texture. Melting the
// ice then also removes the frosted logo, revealing the crisp one beneath.
static bool build_ice_base()
{
    ctx.ice_base = (uint32_t*)psram_alloc(ICE_W * ICE_H * 4);
    if (!ctx.ice_base) return false;
    const lv_image_header_t& fh = ctx.art[ART_FROZEN].header;
    const int fx = ctx.art_x[ART_FROZEN] - ICE_X, fy = ctx.art_y[ART_FROZEN] - ICE_Y;
    for (int y = 0; y < ICE_H; y++) {
        for (int x = 0; x < ICE_W; x++) {
            float ir, ig, ib, ia;
            rgb565a8_px(ctx.art_buf[ART_ICE], ICE_W, ICE_H, y * ICE_W + x, &ir, &ig, &ib, &ia);
            int lx = x - fx, ly = y - fy;
            if (lx >= 0 && ly >= 0 && lx < (int)fh.w && ly < (int)fh.h) {
                float lr, lg, lb, la;
                rgb565a8_px(ctx.art_buf[ART_FROZEN], fh.w, fh.h, ly * fh.w + lx, &lr, &lg, &lb, &la);
                float k = la * (1 - ia), oa = ia + k;
                if (oa > 0) {
                    ir = (ir * ia + lr * k) / oa;
                    ig = (ig * ia + lg * k) / oa;
                    ib = (ib * ia + lb * k) / oa;
                }
                ia = oa;
            }
            ctx.ice_base[y * ICE_W + x] =
                ((uint32_t)to_opa(ia) << 24) | ((uint32_t)to_opa(ir) << 16) | ((uint32_t)to_opa(ig) << 8) | to_opa(ib);
        }
    }
    // The ice and frozen layers live on only inside ice_base.
    heap_caps_free(ctx.art_buf[ART_ICE]);
    heap_caps_free(ctx.art_buf[ART_FROZEN]);
    ctx.art_buf[ART_ICE] = ctx.art_buf[ART_FROZEN] = nullptr;
    return true;
}

// Background gradient + per-pixel glow intensity of the warm radial glow
// rising from below the screen.
static bool build_background()
{
    ctx.bg_buf = (uint16_t*)psram_alloc(ART_W * ART_H * 2);
    ctx.glow_i = (uint8_t*)psram_alloc(ART_W * (ART_H - GLOW_Y0));
    if (!ctx.bg_buf || !ctx.glow_i) return false;
    for (int y = 0; y < ART_H; y++) {
        float f = (float)y / (ART_H - 1);
        uint8_t* c = tab->bg_rgb[y];
        c[0] = (uint8_t)(BG_TOP_R + (BG_BOT_R - BG_TOP_R) * f + 0.5f);
        c[1] = (uint8_t)(BG_TOP_G + (BG_BOT_G - BG_TOP_G) * f + 0.5f);
        c[2] = (uint8_t)(BG_TOP_B + (BG_BOT_B - BG_TOP_B) * f + 0.5f);
    }
    ctx.glow_y_min = ART_H;
    for (int y = GLOW_Y0; y < ART_H; y++) {
        float dy = ((float)y - ART_H * 1.05f) / 620.0f;
        for (int x = 0; x < ART_W; x++) {
            float dx = ((float)x - ART_W / 2.0f) / 520.0f;
            float r = sqrtf(dx * dx + dy * dy);
            uint8_t v = r < 1 ? (uint8_t)((1 - r) * (1 - r) * 0.55f * 255.0f + 0.5f) : 0;
            if (v < 3) v = 0;  // invisible anyway; keeps the redraw region small
            ctx.glow_i[(y - GLOW_Y0) * ART_W + x] = v;
            if (v && y < ctx.glow_y_min) ctx.glow_y_min = y;
        }
    }
    ctx.glow_level = 0;
    ctx.glow_t = -1.0f;
    return true;
}

// A small water bead with a specular highlight; scaled per frame.
static bool build_drop()
{
    ctx.drop_buf = (uint32_t*)psram_alloc(DROP_SPRITE * DROP_SPRITE * 4);
    if (!ctx.drop_buf) return false;
    const float c = DROP_SPRITE / 2.0f, r = DROP_R;
    for (int y = 0; y < DROP_SPRITE; y++) {
        for (int x = 0; x < DROP_SPRITE; x++) {
            float xs = x + 0.5f - c, ys = y + 0.5f - c;
            float d = sqrtf((xs / r) * (xs / r) + (ys / r) * (ys / r));
            float a = smoothstep(1.0f, 0.8f, d) * 0.85f;
            float hx = (xs + r * 0.35f) / (r * 0.3f), hy = (ys + r * 0.3f) / (r * 0.35f);
            float hi = expf(-(hx * hx + hy * hy)) * 0.7f;
            float shade = 0.85f + 0.15f * (-ys / r);
            float rr = clamp01(0.62f * shade + hi), gg = clamp01(0.84f * shade + hi), bb = clamp01(0.98f * shade + hi);
            ctx.drop_buf[y * DROP_SPRITE + x] =
                ((uint32_t)to_opa(a) << 24) | ((uint32_t)to_opa(rr) << 16) | ((uint32_t)to_opa(gg) << 8) | to_opa(bb);
        }
    }
    lv_image_dsc_t& d = ctx.drop_dsc;
    d.header.magic = LV_IMAGE_HEADER_MAGIC;
    d.header.cf = LV_COLOR_FORMAT_ARGB8888;
    d.header.w = DROP_SPRITE;
    d.header.h = DROP_SPRITE;
    d.header.stride = DROP_SPRITE * 4;
    d.data_size = DROP_SPRITE * DROP_SPRITE * 4;
    d.data = (const uint8_t*)ctx.drop_buf;
    return true;
}

static void build_luts()
{
    for (int k = 0; k < EDGE_LUT_LEN; k++) {
        float d = (float)k / EDGE_LUT_STEPS;
        float soft = d >= 3 ? 1.0f : d / 3.0f;
        float w = (d - 5) / 4;
        float wet = expf(-w * w);
        tab->edge_soft[k] = (uint16_t)(soft * 256 + 0.5f);
        tab->edge_wet_a[k] = to_opa(wet * 0.4f * soft);
        tab->edge_wet_c[k] = to_opa(wet * 0.35f);
    }
    for (int o = -SHEEN_HALF; o <= SHEEN_HALF; o++) {
        float s = (float)o / 40.0f;
        tab->sheen_lut[o + SHEEN_HALF] = to_opa(expf(-s * s) * 0.35f);
    }
    for (int i = 0; i < U_LUT; i++) {
        float u = (i + 0.5f) / U_LUT * 2 - 1;
        float hu = (u + 0.42f) / 0.16f;
        tab->u_inside[i] = smoothstep(1.0f, 0.75f, fabsf(u));
        tab->u_shade[i] = 0.80f + 0.18f * cosf(u * 1.4f) - 0.12f * smoothstep(0.4f, 1.0f, u);
        tab->u_hi[i] = expf(-hu * hu);
    }
}

// Irregular clusters: one long icicle with a couple of shorter ones, plus stubs.
static void build_icicles()
{
    ctx.nicicles = 0;
    for (int c = 0; c < 5; c++) {
        float cx = frand(60, ICE_W - 60);
        int n = 1 + (int)frand(0, 2.999f);
        for (int k = 0; k < n && ctx.nicicles < MAX_ICICLES; k++) {
            Icicle& ic = ctx.icicles[ctx.nicicles++];
            bool main_one = k == 0;
            ic.ln = main_one ? frand(70, 115) : frand(18, 50);
            ic.hw = main_one ? frand(9, 13) : frand(5, 8);
            ic.x = cx + (main_one ? 0 : (frand(0, 1) < 0.5f ? -1 : 1) * frand(16, 30));
            ic.bend = frand(-6, 6);
            ic.ph = frand(0, 6.28f);
            ic.ndrips = ic.ln > 40 ? 1 + (int)frand(0, 1.999f) : 0;
            for (int d = 0; d < ic.ndrips; d++) ic.drips[d] = frand(1.0f, 2.0f);
            if (ic.ndrips == 2 && ic.drips[1] < ic.drips[0]) std::swap(ic.drips[0], ic.drips[1]);
        }
    }
    for (int s = 0; s < 6 && ctx.nicicles < MAX_ICICLES; s++) {
        Icicle& ic = ctx.icicles[ctx.nicicles++];
        ic.x = frand(30, ICE_W - 30);
        ic.hw = frand(4, 6);
        ic.ln = frand(6, 14);
        ic.bend = 0;
        ic.ph = frand(0, 6.28f);
        ic.ndrips = 0;
    }
    ctx.icicle_c0 = ICE_W;
    ctx.icicle_c1 = 0;
    for (int i = 0; i < ctx.nicicles; i++) {
        const Icicle& ic = ctx.icicles[i];
        int half = (int)(ic.hw + 12) + 1;
        ctx.icicle_c0 = std::min(ctx.icicle_c0, std::max(0, (int)ic.x - half));
        ctx.icicle_c1 = std::min(ICE_W, std::max(ctx.icicle_c1, (int)ic.x + half + 1));
    }
}

// ------------------------------------------------------------ compositing ----

// 8-bit "over" of colour (r,g,b) at alpha a (0..255) onto an RGB565 pixel.
static inline uint16_t blend565(uint16_t d, uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    if (a == 0) return d;
    if (a == 255) return rgb565(r, g, b);
    a += a >> 7;  // 0..256
    const uint32_t ia = 256 - a;
    uint32_t dr = (d >> 11) & 0x1F, dg = (d >> 5) & 0x3F, db = d & 0x1F;
    dr = (dr << 3) | (dr >> 2);
    dg = (dg << 2) | (dg >> 4);
    db = (db << 3) | (db >> 2);
    return rgb565((r * a + dr * ia) >> 8, (g * a + dg * ia) >> 8, (b * a + db * ia) >> 8);
}

// Blend one icicle onto the scene, clipped to ice-canvas rect [r0,r1) x [c0,c1).
static void draw_icicle(const Icicle& ic, float L, float hw, int r0, int r1, int c0, int c1)
{
    if (L < 2) return;
    const int wid = (int)(hw * 2 + 24);
    const float half = wid / 2.0f;
    const int x0 = (int)(ic.x - half);
    if (x0 + wid <= c0 || x0 >= c1) return;
    const int ys0 = std::max(0, r0 - ICICLE_TOP), ys1 = std::min((int)L + 1, r1 - ICICLE_TOP);
    for (int ys = ys0; ys < ys1; ys++) {
        const int row = ICICLE_TOP + ys;
        const float fy = (float)ys;
        const float yn = clamp01(fy / L);
        const float w = std::max(0.3f, hw * powf(1 - yn, 0.95f) * (1 + 0.08f * sinf(fy / 6.5f + ic.ph)) +
                                           1.2f * powf(1 - yn, 0.3f) + 5 * expf(-fy / 9));
        const float cx = ic.bend * yn * yn;
        const float rings = 0.04f * sinf(fy / 3.2f + ic.ph);
        const float top_fade = smoothstep(0, 14, fy);
        const float hi_k = 0.55f - 0.3f * yn, a_k = 0.62f - 0.28f * yn;
        uint16_t* dst = ctx.bg_buf + (ICE_Y + row) * ART_W + ICE_X;
        const int xa = std::max(c0, x0), xb = std::min(c1, x0 + wid);
        for (int col = xa; col < xb; col++) {
            float u = ((float)(col - x0) - half - cx) / w;
            if (u <= -1.0f || u >= 1.0f) continue;
            int i = (int)((u + 1) * (U_LUT / 2));
            float hi = tab->u_hi[i] * hi_k;
            float a = tab->u_inside[i] * (a_k + 0.25f * hi) * top_fade;
            float sh = tab->u_shade[i] + rings;
            dst[col] = blend565(dst[col], to_opa(0.66f * sh + hi), to_opa(0.85f * sh + hi), to_opa(0.97f * sh + hi),
                                to_opa(a));
        }
    }
}

// Rebuild the displayed scene in art rect [x0,x1) x [y0,y1) in one pass:
// background + glow, logo, then the ice block and icicles from the current
// ice state. Everything is blended straight into the opaque RGB565 scene, so
// LVGL only ever copies it and no intermediate layer round-trips PSRAM.
static void compose_rows(int x0, int y0, int x1, int y1)
{
    const lv_image_header_t& lh = ctx.art[ART_LOGO].header;
    const int lx0 = ctx.art_x[ART_LOGO], ly0 = ctx.art_y[ART_LOGO];
    const int lw = lh.w, lhh = lh.h;
    const uint16_t* logo_rgb = (const uint16_t*)ctx.art_buf[ART_LOGO];
    const uint8_t* logo_a = ctx.art_buf[ART_LOGO] + lw * lhh * 2;
    const uint32_t lopa = (uint32_t)ctx.logo_opa;
    const uint32_t glvl = (uint32_t)ctx.glow_level;
    const bool ice = !ctx.ice_hidden;
    const bool melting = ctx.ice_melting, sheen = ctx.ice_sheen;

    for (int y = y0; y < y1; y++) {
        uint16_t* dst = ctx.bg_buf + y * ART_W;
        const uint8_t* c = tab->bg_rgb[y];
        if (glvl && y >= ctx.glow_y_min) {
            const uint8_t* gi = ctx.glow_i + (y - GLOW_Y0) * ART_W;
            for (int x = x0; x < x1; x++) {
                uint32_t s = (gi[x] * glvl) >> 8;
                dst[x] = rgb565_dither(std::min<uint32_t>(255, c[0] + s),
                                       std::min<uint32_t>(255, c[1] + ((s * 115) >> 8)),
                                       std::min<uint32_t>(255, c[2] + ((s * 20) >> 8)), x, y);
            }
        } else {
            uint16_t px[4];
            for (int i = 0; i < 4; i++) px[i] = rgb565_dither(c[0], c[1], c[2], i, y);
            for (int x = x0; x < x1; x++) dst[x] = px[x & 3];
        }

        if (lopa && y >= ly0 && y < ly0 + lhh) {
            const int xa = std::max(x0, lx0), xb = std::min(x1, lx0 + lw);
            const uint16_t* lrow = logo_rgb + (y - ly0) * lw - lx0;
            const uint8_t* arow = logo_a + (y - ly0) * lw - lx0;
            for (int x = xa; x < xb; x++) {
                uint32_t a = arow[x];
                if (!a) continue;
                if (lopa != 255) a = (a * (lopa + 1)) >> 8;
                uint16_t lc = lrow[x];
                uint32_t r = (lc >> 11) & 0x1F, g = (lc >> 5) & 0x3F, b = lc & 0x1F;
                dst[x] = blend565(dst[x], (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), a);
            }
        }

        // Ice block: base texture with the melt edge (soft cut + wet
        // highlight) and the frost sheen.
        if (ice && y >= ICE_Y && y < ICE_Y + ICE_H) {
            const int r = y - ICE_Y;
            const int xa = std::max(x0, ICE_X), xb = std::min(x1, ICE_X + ICE_W);
            const uint32_t* src = ctx.ice_base + r * ICE_W - ICE_X;
            const float* top = tab->top - ICE_X;
            const int sheen_off = (r >> 1) - ctx.ice_sx - ICE_X;
            for (int x = xa; x < xb; x++) {
                uint32_t p = src[x];
                uint32_t a = p >> 24;
                if (!a) continue;
                float d = (float)r - top[x];
                if (d <= 0) continue;
                uint32_t add = 0;
                if (d < 20) {
                    int k = (int)(d * EDGE_LUT_STEPS);
                    a = (a * tab->edge_soft[k]) >> 8;
                    if (melting) {
                        a = std::min<uint32_t>(255, a + tab->edge_wet_a[k]);
                        add = tab->edge_wet_c[k];
                    }
                }
                if (sheen) {
                    int o = x + sheen_off;
                    if (o > -SHEEN_HALF && o < SHEEN_HALF) add += tab->sheen_lut[o + SHEEN_HALF];
                }
                uint32_t rr = (p >> 16) & 0xFF, gg = (p >> 8) & 0xFF, bb = p & 0xFF;
                if (add) {
                    rr = std::min<uint32_t>(255, rr + add);
                    gg = std::min<uint32_t>(255, gg + add);
                    bb = std::min<uint32_t>(255, bb + add);
                }
                dst[x] = blend565(dst[x], rr, gg, bb, a);
            }
        }
    }

    // Icicles hang over the block; "over" is associative, so blending them
    // onto the scene equals blending them onto the ice first.
    if (ice && y1 > ICE_Y + ICICLE_TOP && y0 < ICE_Y + CANVAS_H) {
        const int r0 = y0 - ICE_Y, r1 = y1 - ICE_Y;
        const int c0 = std::max(0, x0 - ICE_X), c1 = std::min(ICE_W, x1 - ICE_X);
        for (int i = 0; i < ctx.nicicles; i++) {
            const Icicle& ic = ctx.icicles[i];
            draw_icicle(ic, ic.ln * (1 - ctx.shrink), ic.hw * (1 - 0.5f * ctx.shrink), r0, r1, c0, c1);
        }
    }
}

// Second core: large composites are split, the worker (pinned to the other
// core) doing the bottom rows while the caller does the top ones.
static void compose_worker(void*)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (ctx.worker_quit) break;
        compose_rows(ctx.job[0], ctx.job[1], ctx.job[2], ctx.job[3]);
        xSemaphoreGive(ctx.job_done);
    }
    xSemaphoreGive(ctx.job_done);
    vTaskSuspend(nullptr);  // deleted by cleanup_animation() (stack is in PSRAM)
}

static void compose(int x0, int y0, int x1, int y1)
{
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, ART_W);
    y1 = std::min(y1, ART_H);
    if (x1 <= x0 || y1 <= y0) return;
    if (ctx.worker && (x1 - x0) * (y1 - y0) >= 20000) {
        const int ym = (y0 + y1) / 2;
        ctx.job[0] = x0;
        ctx.job[1] = ym;
        ctx.job[2] = x1;
        ctx.job[3] = y1;
        xTaskNotifyGive(ctx.worker);
        compose_rows(x0, y0, x1, ym);
        xSemaphoreTake(ctx.job_done, portMAX_DELAY);
    } else {
        compose_rows(x0, y0, x1, y1);
    }
    lv_area_t a = {(int32_t)(ctx.ox + x0), (int32_t)(ctx.oy + y0), (int32_t)(ctx.ox + x1 - 1),
                   (int32_t)(ctx.oy + y1 - 1)};
    lv_obj_invalidate_area(ctx.bg, &a);
}

// Redraw the glow rows for glow level 0..255. The glow spans ~350k px, so it
// is redrawn at most every GLOW_PERIOD seconds; the stepping is invisible on
// such a soft gradient.
static void render_glow(float t, int level)
{
    if (level == ctx.glow_level || t - ctx.glow_t < GLOW_PERIOD) return;
    ctx.glow_level = level;
    ctx.glow_t = t;
    compose(0, ctx.glow_y_min, ART_W, ART_H);
}

// The logo only changes opacity during the final fade.
static void set_logo_opa(lv_opa_t opa)
{
    if (ctx.logo_opa == opa) return;
    ctx.logo_opa = opa;
    const lv_image_header_t& h = ctx.art[ART_LOGO].header;
    compose(ctx.art_x[ART_LOGO], ctx.art_y[ART_LOGO], ctx.art_x[ART_LOGO] + h.w, ctx.art_y[ART_LOGO] + h.h);
}

// Recomposite ice-canvas rect [r0,r1) x [c0,c1) (relative to the block).
static void redraw_ice(int r0, int r1, int c0, int c1)
{
    r0 = std::max(r0, 0);
    r1 = std::min(r1, CANVAS_H);
    c0 = std::max(c0, 0);
    c1 = std::min(c1, ICE_W);
    if (r1 <= r0 || c1 <= c0) return;
    compose(ICE_X + c0, ICE_Y + r0, ICE_X + c1, ICE_Y + r1);
}

static void update_ice(float t, float melt, float shrink)
{
    if (melt >= 1.0f) {
        if (!ctx.ice_hidden) {
            ctx.ice_hidden = true;
            compose(ICE_X, ICE_Y, ICE_X + ICE_W, ICE_Y + CANVAS_H);
        }
        return;
    }
    const bool melting = melt > 0;
    const bool sheen = t < SHEEN_END;
    const float sx = t * (ART_W + 400) - 200;
    ctx.ice_melting = melting;
    ctx.ice_sheen = sheen;
    ctx.ice_sx = (int)sx;
    ctx.shrink = shrink;
    const float wob = std::min(1.0f, melt * 4);
    float tmin = 1e9f, tmax = -1e9f;
    for (int c = 0; c < ICE_W; c++) {
        tab->top[c] = melt * (ICE_H + 40) - 30 + tab->wobble[c] * wob;
        tmin = std::min(tmin, tab->top[c]);
        tmax = std::max(tmax, tab->top[c]);
    }

    // Frost sheen: a diagonal glint sweeping across before the melt starts.
    if (sheen || !std::isnan(ctx.sheen_prev)) {
        float lo_x = sheen ? sx : ctx.sheen_prev, hi_x = lo_x;
        if (!std::isnan(ctx.sheen_prev)) {
            lo_x = std::min(lo_x, ctx.sheen_prev);
            hi_x = std::max(hi_x, ctx.sheen_prev);
        }
        // Band columns for row r: (pos - r/2 - SHEEN_HALF, pos - r/2 + SHEEN_HALF).
        for (int ra = 0; ra < ICE_H; ra += SHEEN_STRIP) {
            int rb = std::min(ICE_H, ra + SHEEN_STRIP);
            int c0 = (int)lo_x - rb / 2 - SHEEN_HALF - 1;
            int c1 = (int)hi_x - ra / 2 + SHEEN_HALF + 2;
            redraw_ice(ra, rb, c0, c1);
        }
        ctx.sheen_prev = sheen ? sx : NAN;
    }

    // Melt band: from last frame's band down to just below the new edge.
    if (melting) {
        int r0 = (int)tmin - 2, r1 = (int)tmax + 21;
        if (ctx.melt_r1 > ctx.melt_r0) {
            r0 = std::min(r0, ctx.melt_r0);
            r1 = std::max(r1, ctx.melt_r1);
        }
        redraw_ice(r0, r1, 0, ICE_W);
        ctx.melt_r0 = (int)tmin - 2;
        ctx.melt_r1 = (int)tmax + 21;
    }

    // Icicles retreating into the block.
    if (shrink != ctx.last_shrink) {
        redraw_ice(ICICLE_TOP, CANVAS_H, ctx.icicle_c0, ctx.icicle_c1);
        ctx.last_shrink = shrink;
    }
}

// ----------------------------------------------------------------- drops ----

static void place_drop(int slot, float x, float y, float r, float stretch, float alpha)
{
    lv_obj_t* o = ctx.drops[slot];
    if (!o) return;
    if (alpha <= 0.01f || y > ART_H + 20) {
        if (!lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_scale_x(o, (uint32_t)(256 * r / DROP_R));
    lv_image_set_scale_y(o, (uint32_t)(256 * r * stretch / DROP_R));
    lv_obj_set_pos(o, ctx.ox + (int)(x - DROP_SPRITE / 2.0f), ctx.oy + (int)(y - DROP_SPRITE / 2.0f));
    lv_obj_set_style_image_opa(o, to_opa(alpha), 0);
}

static void update_drops(float t, float shrink, float fade)
{
    int slot = 0;
    for (int i = 0; i < ctx.nicicles; i++) {
        const Icicle& ic = ctx.icicles[i];
        const float L = ic.ln * (1 - shrink);
        const float tip_x = ICE_X + ic.x + ic.bend;
        for (int d = 0; d < ic.ndrips && slot < MAX_DROPS; d++, slot++) {
            float age = t - ic.drips[d];
            if (age < 0) {
                place_drop(slot, 0, 0, 1, 1, 0);
            } else if (age < 0.45f) {
                float r = 1.5f + 3.5f * ease_out(age / 0.45f);
                float tip_y = ICE_Y + ICICLE_TOP + L;
                place_drop(slot, tip_x, tip_y + r * 0.8f, r, 1.0f + 0.3f * age / 0.45f, 1 - fade);
            } else {
                float fa = age - 0.45f;
                float L0 = ic.ln * (1 - ease_in_out(phase(ic.drips[d] + 0.45f, 1.6f, 2.35f)));
                float y = ICE_Y + ICICLE_TOP + L0 + 4 + 1400 * fa * fa;
                place_drop(slot, tip_x, y, 4.5f, 1.4f + std::min(0.8f, fa * 3),
                           std::max(0.0f, 1 - fa / 0.7f) * (1 - fade));
            }
        }
    }
}

// ----------------------------------------------------------------- frame ----

static void render_frame(float t)
{
    const float fade = phase(t, 3.0f, 3.4f);
    const float glow = ease_out(phase(t, 0.6f, 1.6f)) * (1 - 0.4f * phase(t, 2.6f, 3.1f)) * (1 - fade);
    const float crystals = 1 - phase(t, 0.8f, 1.8f);
    const float melt = ease_in_out(phase(t, 1.0f, 2.4f));
    const float tag = ease_out(phase(t, 0.7f, 1.4f)) * (1 - fade);
    const float shrink = ease_in_out(phase(t, 1.6f, 2.35f));

    render_glow(t, to_opa(glow));
    set_logo_opa(to_opa(1 - fade));
    set_image_opa(ctx.tag, &ctx.tag_opa, to_opa(tag));

    if (crystals > 0) {
        for (auto& c : ctx.crystals) {
            float y = fmodf(c.y + t * 25, (float)ART_H);
            float x = c.x + 6 * sinf(t * 1.3f + c.ph);
            // Crystals float behind the ice block, which is now part of the scene.
            bool behind_ice = x > ICE_X && x < ICE_X + ICE_W && y > ICE_Y && y < ICE_Y + ICE_H;
            if (behind_ice) {
                if (!lv_obj_has_flag(c.obj, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(c.obj, LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            lv_obj_remove_flag(c.obj, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(c.obj, ctx.ox + (int)(x - c.r), ctx.oy + (int)(y - c.r));
            lv_obj_set_style_bg_opa(c.obj, to_opa(0.7f * (0.6f + 0.4f * sinf(t * 3 + c.ph)) * crystals), 0);
        }
    } else if (!ctx.crystals_hidden) {
        for (auto& c : ctx.crystals) lv_obj_add_flag(c.obj, LV_OBJ_FLAG_HIDDEN);
        ctx.crystals_hidden = true;
    }

    update_ice(t, melt, shrink);
    update_drops(t, shrink, fade);
}

// ------------------------------------------------------------------- API ----

static void free_buffers()
{
    heap_caps_free(tab);
    tab = nullptr;
    for (int i = 0; i < ART_COUNT; i++) {
        heap_caps_free(ctx.art_buf[i]);
        ctx.art_buf[i] = nullptr;
    }
    heap_caps_free(ctx.ice_base);
    heap_caps_free(ctx.bg_buf);
    heap_caps_free(ctx.glow_i);
    heap_caps_free(ctx.drop_buf);
    ctx.ice_base = nullptr;
    ctx.bg_buf = nullptr;
    ctx.glow_i = nullptr;
    ctx.drop_buf = nullptr;
}

static lv_obj_t* make_image(lv_obj_t* parent, const lv_image_dsc_t* dsc, int x, int y)
{
    lv_obj_t* img = lv_image_create(parent);
    lv_image_set_src(img, dsc);
    lv_obj_set_pos(img, ctx.ox + x, ctx.oy + y);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    return img;
}

static lv_obj_t* make_canvas(lv_obj_t* parent, void* buf, int w, int h, lv_color_format_t cf, int x, int y)
{
    lv_obj_t* cv = lv_canvas_create(parent);
    lv_canvas_set_buffer(cv, buf, w, h, cf);
    lv_obj_set_pos(cv, ctx.ox + x, ctx.oy + y);
    lv_obj_remove_flag(cv, LV_OBJ_FLAG_CLICKABLE);
    return cv;
}

// Stops the animation. The scene canvas (and its buffer) stays on screen until
// startup_anim_release(), so the main UI can be built underneath it and the
// swap happens in a single refresh, with no undithered in-between frame.
static void cleanup_animation()
{
    if (ctx.tag) lv_obj_delete(ctx.tag);
    ctx.tag = nullptr;
    for (auto& d : ctx.drops) {
        if (d) lv_obj_delete(d);
        d = nullptr;
    }
    for (auto& c : ctx.crystals) {
        if (c.obj) lv_obj_delete(c.obj);
        c.obj = nullptr;
    }
    if (ctx.worker) {
        ctx.worker_quit = true;
        xTaskNotifyGive(ctx.worker);
        xSemaphoreTake(ctx.job_done, portMAX_DELAY);
        vTaskDeleteWithCaps(ctx.worker);
        ctx.worker = nullptr;
    }
    if (ctx.job_done) {
        vSemaphoreDelete(ctx.job_done);
        ctx.job_done = nullptr;
    }
    uint16_t* scene = ctx.bg_buf;
    ctx.bg_buf = nullptr;
    free_buffers();
    ctx.bg_buf = scene;
    ctx.running = false;
}

bool startup_anim_init(void (*on_complete)(void))
{
    if (ctx.running) return false;
    ctx.on_complete = on_complete;
    int64_t t0 = esp_timer_get_time();

    lv_obj_t* scr = lv_screen_active();
    ctx.ox = (lv_obj_get_width(scr) - ART_W) / 2;
    ctx.oy = (lv_obj_get_height(scr) - ART_H) / 2;
    lv_obj_set_style_bg_color(scr, lv_color_make(BG_TOP_R, BG_TOP_G, BG_TOP_B), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_rng = 0x2545F491u;
    tab = (AnimTables*)heap_caps_calloc(1, sizeof(AnimTables), MALLOC_CAP_SPIRAM);
    if (!tab || !load_art() || !build_ice_base() || !build_background() || !build_drop()) {
        ESP_LOGE(TAG, "startup animation unavailable, skipping");
        free_buffers();
        if (on_complete) on_complete();
        return false;
    }
    build_luts();
    build_icicles();
    for (int c = 0; c < ICE_W; c++) {
        tab->wobble[c] = 12 * sinf(c / 61.0f) + 7 * sinf(c / 23.0f + 1.3f);
        tab->top[c] = -30;
    }

    ctx.bg = make_canvas(scr, ctx.bg_buf, ART_W, ART_H, LV_COLOR_FORMAT_RGB565, 0, 0);

    for (auto& c : ctx.crystals) {
        c.x = frand(0, ART_W);
        c.y = frand(0, ART_H);
        c.r = frand(1.5f, 3.5f);
        c.ph = frand(0, 6.28f);
        c.obj = lv_obj_create(scr);
        lv_obj_remove_style_all(c.obj);
        lv_obj_set_size(c.obj, (int)(c.r * 2 + 0.5f), (int)(c.r * 2 + 0.5f));
        lv_obj_set_style_radius(c.obj, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(c.obj, lv_color_hex(0xd9f2ff), 0);
        lv_obj_remove_flag(c.obj, LV_OBJ_FLAG_CLICKABLE);
    }
    ctx.crystals_hidden = false;

    ctx.tag = make_image(scr, &ctx.art[ART_TAG], ctx.art_x[ART_TAG], ctx.art_y[ART_TAG]);
    ctx.tag_opa = -1;
    ctx.logo_opa = 255;

    // Initial frame: the whole block, icicles at full length.
    ctx.melt_r0 = ctx.melt_r1 = 0;
    ctx.sheen_prev = NAN;
    ctx.last_shrink = 0.0f;
    ctx.ice_hidden = false;
    ctx.ice_melting = ctx.ice_sheen = false;
    ctx.ice_sx = 0;
    ctx.shrink = 0.0f;
    ctx.job_done = xSemaphoreCreateBinary();
    ctx.worker_quit = false;
    if (!ctx.job_done ||
        xTaskCreatePinnedToCoreWithCaps(compose_worker, "anim_compose", 4096, nullptr, uxTaskPriorityGet(nullptr),
                                        &ctx.worker, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        ctx.worker = nullptr;
        ESP_LOGW(TAG, "compose worker unavailable, single-core compose");
    }
    compose(0, 0, ART_W, ART_H);

    for (auto& d : ctx.drops) {
        d = make_image(scr, &ctx.drop_dsc, 0, 0);
        lv_image_set_pivot(d, DROP_SPRITE / 2, DROP_SPRITE / 2);
        lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
    }

    render_frame(0);
    ctx.running = true;
    ctx.start_us = 0;
    ctx.last_frame_us = 0;
    ESP_LOGI(TAG, "ready in %lld ms", (long long)((esp_timer_get_time() - t0) / 1000));
    return true;
}

bool startup_anim_update(void)
{
    if (!ctx.running) return false;
    int64_t now = esp_timer_get_time();
    if (now - ctx.last_frame_us < 33000) return true;  // match LVGL's 33 ms refresh period
    ctx.last_frame_us = now;
    if (!ctx.start_us) ctx.start_us = now;
    float t = (float)(now - ctx.start_us) / 1e6f;
    if (t >= T_TOTAL) {
        cleanup_animation();
        if (ctx.on_complete) ctx.on_complete();
        return false;
    }
    render_frame(t);
    return true;
}

bool startup_anim_is_running(void)
{
    return ctx.running;
}

void startup_anim_stop(void)
{
    if (ctx.running) cleanup_animation();
    startup_anim_release();
}

void startup_anim_release(void)
{
    if (ctx.running) return;
    if (ctx.bg) lv_obj_delete(ctx.bg);
    ctx.bg = nullptr;
    heap_caps_free(ctx.bg_buf);
    ctx.bg_buf = nullptr;
}
