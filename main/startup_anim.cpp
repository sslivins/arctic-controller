/*
 * Arctic Heat Pump Controller - Startup Animation
 *
 * The Arctic mark sits frozen in a block of ice; the heat pump "comes on" (a
 * warm glow rises from below and HEAT PUMPS lights up), the icicles start to
 * drip and the ice melts away from the top down, revealing the crisp logo.
 *
 * The static layers (logo, frozen logo, tagline, ice texture) are pre-rendered
 * by tools/boot_art/gen_boot_art.py and embedded LZ4-compressed
 * (main/boot_art/boot_art.bin). Everything that moves is animated here: the
 * melt edge, sheen and icicles are drawn into a canvas each frame, the rest is
 * plain LVGL objects whose opacity/position change. The timeline mirrors the
 * approved preview and is driven by wall-clock time, so a slow frame simply
 * skips ahead instead of stretching the animation.
 */
#include "startup_anim.h"

#include <lvgl.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

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
#define GLOW_Y0 724                         // first screen row the heat glow reaches

#define COLOR_BG_TOP 0x0a101e
#define COLOR_BG_BOT 0x1b3050

#define NUM_CRYSTALS 22
#define MAX_ICICLES 16
#define MAX_DROPS 24
#define DROP_SPRITE 24
#define DROP_R 10.0f

static constexpr float T_TOTAL = 3.4f;

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
    int64_t start_us = 0;
    int64_t last_frame_us = 0;
    int ox = 0, oy = 0;  // offset of the 720x1280 art on the actual screen

    lv_image_dsc_t art[ART_COUNT] = {};
    int16_t art_x[ART_COUNT] = {}, art_y[ART_COUNT] = {};
    uint8_t* art_buf[ART_COUNT] = {};

    uint32_t* ice_base = nullptr;  // ARGB8888, ICE_W x ICE_H
    uint32_t* canvas_buf = nullptr;
    uint16_t* glow_buf = nullptr;
    uint32_t* drop_buf = nullptr;
    lv_image_dsc_t glow_dsc = {}, drop_dsc = {};

    lv_obj_t* glow = nullptr;
    lv_obj_t* frozen = nullptr;
    lv_obj_t* logo = nullptr;
    lv_obj_t* tag = nullptr;
    lv_obj_t* canvas = nullptr;
    lv_obj_t* fade = nullptr;
    lv_obj_t* drops[MAX_DROPS] = {};
    Crystal crystals[NUM_CRYSTALS] = {};

    Icicle icicles[MAX_ICICLES] = {};
    int nicicles = 0;
    float top[ICE_W] = {};
    float wobble[ICE_W] = {};
    int dirty_r0 = 0, dirty_r1 = 0;  // canvas rows touched by the melt edge last frame
    bool sheen_was_active = true;
    float last_shrink = -1.0f;
    bool ice_hidden = false;
} ctx;

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

static void* psram_alloc(size_t n)
{
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// Straight-alpha "over" of (r,g,b,a) in 0..1 onto an ARGB8888 pixel.
static inline void blend_over(uint32_t* px, float r, float g, float b, float a)
{
    if (a <= 0.002f) return;
    uint32_t d = *px;
    float da = (float)(d >> 24) / 255.0f;
    float oa = a + da * (1 - a);
    float k = da * (1 - a);
    float orr = (r * a + (float)((d >> 16) & 0xFF) / 255.0f * k) / oa;
    float og = (g * a + (float)((d >> 8) & 0xFF) / 255.0f * k) / oa;
    float ob = (b * a + (float)(d & 0xFF) / 255.0f * k) / oa;
    *px = ((uint32_t)to_opa(oa) << 24) | ((uint32_t)to_opa(orr) << 16) | ((uint32_t)to_opa(og) << 8) | to_opa(ob);
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

// Expand the ice layer to ARGB8888 so per-frame edits are cheap.
static bool build_ice_base()
{
    ctx.ice_base = (uint32_t*)psram_alloc(ICE_W * ICE_H * 4);
    if (!ctx.ice_base) return false;
    const uint16_t* c565 = (const uint16_t*)ctx.art_buf[ART_ICE];
    const uint8_t* a8 = ctx.art_buf[ART_ICE] + ICE_W * ICE_H * 2;
    for (int i = 0; i < ICE_W * ICE_H; i++) {
        uint16_t c = c565[i];
        uint32_t r = ((c >> 11) & 0x1F) * 255 / 31;
        uint32_t g = ((c >> 5) & 0x3F) * 255 / 63;
        uint32_t b = (c & 0x1F) * 255 / 31;
        ctx.ice_base[i] = ((uint32_t)a8[i] << 24) | (r << 16) | (g << 8) | b;
    }
    return true;
}

// Warm radial glow from below the screen, drawn additively.
static bool build_glow()
{
    const int gh = ART_H - GLOW_Y0;
    ctx.glow_buf = (uint16_t*)psram_alloc(ART_W * gh * 2);
    if (!ctx.glow_buf) return false;
    for (int y = 0; y < gh; y++) {
        float dy = ((float)(y + GLOW_Y0) - ART_H * 1.05f) / 620.0f;
        for (int x = 0; x < ART_W; x++) {
            float dx = ((float)x - ART_W / 2.0f) / 520.0f;
            float r = sqrtf(dx * dx + dy * dy);
            float g = r < 1 ? (1 - r) * (1 - r) * 0.55f : 0;
            uint16_t rr = (uint16_t)(clamp01(g * 1.00f) * 31 + 0.5f);
            uint16_t gg = (uint16_t)(clamp01(g * 0.45f) * 63 + 0.5f);
            uint16_t bb = (uint16_t)(clamp01(g * 0.08f) * 31 + 0.5f);
            ctx.glow_buf[y * ART_W + x] = (rr << 11) | (gg << 5) | bb;
        }
    }
    lv_image_dsc_t& d = ctx.glow_dsc;
    d.header.magic = LV_IMAGE_HEADER_MAGIC;
    d.header.cf = LV_COLOR_FORMAT_RGB565;
    d.header.w = ART_W;
    d.header.h = gh;
    d.header.stride = ART_W * 2;
    d.data_size = ART_W * gh * 2;
    d.data = (const uint8_t*)ctx.glow_buf;
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
}

// ------------------------------------------------------------ ice canvas ----

// Rewrite canvas rows [r0, r1) of the block from the base texture, applying the
// melt edge (soft cut + wet highlight) and the frost sheen.
static void render_ice_rows(int r0, int r1, float t, float melt)
{
    const bool sheen_on = t < 1.1f;
    const float sx = t * (ART_W + 400) - 200;
    r0 = std::max(r0, 0);
    r1 = std::min(r1, CANVAS_H);
    for (int r = r0; r < r1; r++) {
        uint32_t* dst = ctx.canvas_buf + r * ICE_W;
        if (r >= ICE_H) {
            memset(dst, 0, ICE_W * 4);
            continue;
        }
        const uint32_t* src = ctx.ice_base + r * ICE_W;
        for (int c = 0; c < ICE_W; c++) {
            uint32_t p = src[c];
            uint32_t a0 = p >> 24;
            float d = (float)r - ctx.top[c];
            if (a0 == 0 || d <= 0) {
                dst[c] = 0;
                continue;
            }
            float soft = d >= 3 ? 1.0f : d / 3.0f;
            float add = 0, wet = 0;
            if (melt > 0) {
                float w = (d - 5) / 4;
                if (w > -3 && w < 3) wet = expf(-w * w);
                add += wet * 0.35f;
            }
            if (sheen_on) {
                float s = ((float)c + (float)r * 0.5f - sx) / 40;
                if (s > -3 && s < 3) add += expf(-s * s) * 0.35f;
            }
            uint32_t a = (uint32_t)std::min(255.0f, a0 * soft + wet * 0.4f * soft * 255.0f);
            if (add > 0) {
                uint32_t k = (uint32_t)(add * 255);
                uint32_t rr = std::min<uint32_t>(255, ((p >> 16) & 0xFF) + k);
                uint32_t gg = std::min<uint32_t>(255, ((p >> 8) & 0xFF) + k);
                uint32_t bb = std::min<uint32_t>(255, (p & 0xFF) + k);
                dst[c] = (a << 24) | (rr << 16) | (gg << 8) | bb;
            } else {
                dst[c] = (a << 24) | (p & 0xFFFFFF);
            }
        }
    }
}

// Draw one icicle into the canvas, clipped to rows [r0, r1).
static void draw_icicle(const Icicle& ic, float L, float hw, float bend, int r0, int r1)
{
    if (L < 2) return;
    const int hgt = (int)(L + 4), wid = (int)(hw * 2 + 24);
    const int x0 = (int)(ic.x - wid / 2.0f);
    for (int ys = 0; ys < hgt; ys++) {
        int row = ICICLE_TOP + ys;
        if (row < r0 || row >= r1 || row >= CANVAS_H) continue;
        float fy = (float)ys;
        if (fy > L) break;
        float yn = clamp01(fy / L);
        float w = hw * powf(1 - yn, 0.95f) * (1 + 0.08f * sinf(fy / 6.5f + ic.ph)) + 1.2f * powf(1 - yn, 0.3f) +
                  5 * expf(-fy / 9);
        float cx = bend * yn * yn;
        float rings = 0.04f * sinf(fy / 3.2f + ic.ph);
        float top_fade = smoothstep(0, 14, fy);
        uint32_t* dst = ctx.canvas_buf + row * ICE_W;
        for (int xi = 0; xi < wid; xi++) {
            int col = x0 + xi;
            if (col < 0 || col >= ICE_W) continue;
            float xs = (float)xi - wid / 2.0f;
            float u = (xs - cx) / std::max(w, 0.3f);
            float au = fabsf(u);
            if (au >= 1.0f) continue;
            float inside = smoothstep(1.0f, 0.75f, au);
            float shade = 0.80f + 0.18f * cosf(u * 1.4f) - 0.12f * smoothstep(0.4f, 1.0f, u);
            float hu = (u + 0.42f) / 0.16f;
            float hi = expf(-hu * hu) * (0.55f - 0.3f * yn);
            float a = inside * (0.62f - 0.28f * yn + 0.25f * hi) * top_fade;
            blend_over(&dst[col], clamp01(0.66f * (shade + rings) + hi), clamp01(0.85f * (shade + rings) + hi),
                       clamp01(0.97f * (shade + rings) + hi), a);
        }
    }
}

static void place_drop(int slot, float x, float y, float r, float stretch, float alpha)
{
    lv_obj_t* o = ctx.drops[slot];
    if (!o) return;
    if (alpha <= 0.01f || y > ART_H + 20) {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_scale_x(o, (uint32_t)(256 * r / DROP_R));
    lv_image_set_scale_y(o, (uint32_t)(256 * r * stretch / DROP_R));
    lv_obj_set_pos(o, ctx.ox + (int)(x - DROP_SPRITE / 2.0f), ctx.oy + (int)(y - DROP_SPRITE / 2.0f));
    lv_obj_set_style_image_opa(o, to_opa(alpha), 0);
}

// ----------------------------------------------------------------- frame ----

static void render_frame(float t)
{
    const float glow = ease_out(phase(t, 0.6f, 1.6f)) * (1 - 0.4f * phase(t, 2.6f, 3.1f));
    const float crystals = 1 - phase(t, 0.8f, 1.8f);
    const float melt = ease_in_out(phase(t, 1.0f, 2.4f));
    const float crisp = phase(t, 1.2f, 2.5f);
    const float tag = ease_out(phase(t, 0.7f, 1.4f));
    const float shrink = ease_in_out(phase(t, 1.6f, 2.35f));
    const float fade = phase(t, 3.0f, 3.4f);

    lv_obj_set_style_image_opa(ctx.glow, to_opa(glow), 0);
    lv_obj_set_style_image_opa(ctx.frozen, to_opa(1 - crisp), 0);
    lv_obj_set_style_image_opa(ctx.logo, to_opa(crisp), 0);
    lv_obj_set_style_image_opa(ctx.tag, to_opa(tag), 0);
    lv_obj_set_style_bg_opa(ctx.fade, to_opa(fade), 0);

    for (auto& c : ctx.crystals) {
        if (!c.obj) continue;
        if (crystals <= 0) {
            lv_obj_add_flag(c.obj, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        float y = fmodf(c.y + t * 25, (float)ART_H);
        float x = c.x + 6 * sinf(t * 1.3f + c.ph);
        lv_obj_set_pos(c.obj, ctx.ox + (int)(x - c.r), ctx.oy + (int)(y - c.r));
        lv_obj_set_style_bg_opa(c.obj, to_opa(0.7f * (0.6f + 0.4f * sinf(t * 3 + c.ph)) * crystals), 0);
    }

    // Ice block + icicles.
    if (melt >= 1.0f) {
        if (!ctx.ice_hidden) {
            lv_obj_add_flag(ctx.canvas, LV_OBJ_FLAG_HIDDEN);
            ctx.ice_hidden = true;
        }
    } else {
        lv_obj_set_style_image_opa(ctx.canvas, to_opa(1 - 0.45f * melt), 0);
        float wob = std::min(1.0f, melt * 4);
        float tmin = 1e9f, tmax = -1e9f;
        for (int c = 0; c < ICE_W; c++) {
            ctx.top[c] = melt * (ICE_H + 40) - 30 + ctx.wobble[c] * wob;
            tmin = std::min(tmin, ctx.top[c]);
            tmax = std::max(tmax, ctx.top[c]);
        }
        int r0, r1;
        const bool sheen_on = t < 1.1f;
        if (sheen_on || ctx.sheen_was_active) {
            r0 = 0;
            r1 = ICE_H;  // the sheen sweeps the whole block
        } else if (melt > 0) {
            r0 = std::min(ctx.dirty_r0, (int)tmin - 2);
            r1 = std::max(ctx.dirty_r1, (int)tmax + 20);
        } else {
            r0 = r1 = 0;
        }
        ctx.sheen_was_active = sheen_on;
        if (melt > 0) {
            ctx.dirty_r0 = (int)tmin - 2;
            ctx.dirty_r1 = (int)tmax + 20;
        }
        // The icicle zone must be redrawn when the icicles change or the rows under them were rewritten.
        bool icicles_changed = shrink != ctx.last_shrink;
        if (icicles_changed || r1 > ICICLE_TOP) {
            r0 = std::min(r0 == r1 ? ICICLE_TOP : r0, ICICLE_TOP);
            r1 = CANVAS_H;
        }
        ctx.last_shrink = shrink;
        if (r1 > r0) {
            render_ice_rows(r0, r1, t, melt);
            for (int i = 0; i < ctx.nicicles; i++) {
                const Icicle& ic = ctx.icicles[i];
                draw_icicle(ic, ic.ln * (1 - shrink), ic.hw * (1 - 0.5f * shrink), ic.bend, r0, r1);
            }
            lv_area_t a;
            lv_obj_get_coords(ctx.canvas, &a);
            int y0 = a.y1;
            a.y1 = y0 + std::max(r0, 0);
            a.y2 = y0 + std::min(r1, CANVAS_H) - 1;
            lv_obj_invalidate_area(ctx.canvas, &a);
        }
    }

    // Drips: a bead swells at an icicle tip, then falls and fades.
    int slot = 0;
    for (int i = 0; i < ctx.nicicles; i++) {
        const Icicle& ic = ctx.icicles[i];
        float L = ic.ln * (1 - shrink);
        float tip_x = ICE_X + ic.x + ic.bend * (1 - shrink);
        float tip_y = ICE_Y + ICICLE_TOP + L;
        for (int d = 0; d < ic.ndrips && slot < MAX_DROPS; d++, slot++) {
            float age = t - ic.drips[d];
            if (age < 0) {
                place_drop(slot, 0, 0, 1, 1, 0);
            } else if (age < 0.45f) {
                float r = 1.5f + 3.5f * ease_out(age / 0.45f);
                place_drop(slot, tip_x, tip_y + r * 0.8f, r, 1.0f + 0.3f * age / 0.45f, 1.0f);
            } else {
                float fa = age - 0.45f;
                float L0 = ic.ln * (1 - ease_in_out(phase(ic.drips[d] + 0.45f, 1.6f, 2.35f)));
                float y = ICE_Y + ICICLE_TOP + L0 + 4 + 1400 * fa * fa;
                place_drop(slot, tip_x, y, 4.5f, 1.4f + std::min(0.8f, fa * 3), std::max(0.0f, 1 - fa / 0.7f));
            }
        }
    }
}

// ------------------------------------------------------------------- API ----

static void free_buffers()
{
    for (int i = 0; i < ART_COUNT; i++) {
        heap_caps_free(ctx.art_buf[i]);
        ctx.art_buf[i] = nullptr;
    }
    heap_caps_free(ctx.ice_base);
    heap_caps_free(ctx.canvas_buf);
    heap_caps_free(ctx.glow_buf);
    heap_caps_free(ctx.drop_buf);
    ctx.ice_base = nullptr;
    ctx.canvas_buf = nullptr;
    ctx.glow_buf = nullptr;
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

static void cleanup_animation()
{
    lv_obj_t* scr = lv_screen_active();
    lv_obj_t* objs[] = {ctx.glow, ctx.frozen, ctx.logo, ctx.tag, ctx.canvas, ctx.fade};
    for (lv_obj_t* o : objs) {
        if (o) lv_obj_delete(o);
    }
    ctx.glow = ctx.frozen = ctx.logo = ctx.tag = ctx.canvas = ctx.fade = nullptr;
    for (auto& d : ctx.drops) {
        if (d) lv_obj_delete(d);
        d = nullptr;
    }
    for (auto& c : ctx.crystals) {
        if (c.obj) lv_obj_delete(c.obj);
        c.obj = nullptr;
    }
    (void)scr;
    free_buffers();
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
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG_TOP), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(COLOR_BG_BOT), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_rng = 0x2545F491u;
    ctx.canvas_buf = (uint32_t*)psram_alloc(ICE_W * CANVAS_H * 4);
    if (!ctx.canvas_buf || !load_art() || !build_ice_base() || !build_glow() || !build_drop()) {
        ESP_LOGE(TAG, "startup animation unavailable, skipping");
        free_buffers();
        if (on_complete) on_complete();
        return false;
    }
    build_icicles();
    for (int c = 0; c < ICE_W; c++) {
        ctx.wobble[c] = 12 * sinf(c / 61.0f) + 7 * sinf(c / 23.0f + 1.3f);
    }

    ctx.glow = make_image(scr, &ctx.glow_dsc, 0, GLOW_Y0);
    lv_obj_set_style_blend_mode(ctx.glow, LV_BLEND_MODE_ADDITIVE, 0);
    lv_obj_set_style_image_opa(ctx.glow, LV_OPA_TRANSP, 0);

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

    ctx.frozen = make_image(scr, &ctx.art[ART_FROZEN], ctx.art_x[ART_FROZEN], ctx.art_y[ART_FROZEN]);
    ctx.logo = make_image(scr, &ctx.art[ART_LOGO], ctx.art_x[ART_LOGO], ctx.art_y[ART_LOGO]);
    ctx.tag = make_image(scr, &ctx.art[ART_TAG], ctx.art_x[ART_TAG], ctx.art_y[ART_TAG]);

    ctx.canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(ctx.canvas, ctx.canvas_buf, ICE_W, CANVAS_H, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_set_pos(ctx.canvas, ctx.ox + ICE_X, ctx.oy + ICE_Y);
    lv_obj_remove_flag(ctx.canvas, LV_OBJ_FLAG_CLICKABLE);
    ctx.sheen_was_active = true;
    ctx.last_shrink = -1.0f;
    ctx.ice_hidden = false;
    ctx.dirty_r0 = ctx.dirty_r1 = 0;

    for (auto& d : ctx.drops) {
        d = make_image(scr, &ctx.drop_dsc, 0, 0);
        lv_image_set_pivot(d, DROP_SPRITE / 2, DROP_SPRITE / 2);
        lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
    }

    // Final hand-off: fade to the background before the main UI takes over.
    ctx.fade = lv_obj_create(scr);
    lv_obj_remove_style_all(ctx.fade);
    lv_obj_set_size(ctx.fade, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ctx.fade, lv_color_hex(COLOR_BG_TOP), 0);
    lv_obj_set_style_bg_grad_color(ctx.fade, lv_color_hex(COLOR_BG_BOT), 0);
    lv_obj_set_style_bg_grad_dir(ctx.fade, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(ctx.fade, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(ctx.fade, LV_OBJ_FLAG_CLICKABLE);

    render_frame(0);
    ctx.running = true;
    ctx.start_us = esp_timer_get_time();
    ctx.last_frame_us = 0;
    ESP_LOGI(TAG, "ready in %lld ms", (long long)((ctx.start_us - t0) / 1000));
    return true;
}

bool startup_anim_update(void)
{
    if (!ctx.running) return false;
    int64_t now = esp_timer_get_time();
    if (now - ctx.last_frame_us < 33000) return true;  // match LVGL's 33 ms refresh period
    ctx.last_frame_us = now;
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
}
