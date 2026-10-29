// MGS4CRT.asi — MGS4 on screens narrower than 16:9 (the NEC CRT at 1280x1024): the 3D
// fills the tube and the 16:9 HUD keeps its proper shape, anchored to the tube's edges.
//
// Background (MGS4 Master Collection, exe PE timestamp 0x6A8CFC47, with Sunny Side Up):
//   * The 3D camera takes its aspect from the render buffer (2560x2048 on the CRT), so the
//     3D is already a correct, native 5:4 view; only the final 16:9 letterbox blit hides it.
//     FillScreen replaces that letterbox rectangle with the whole screen.
//   * 2D is laid out for 16:9 and so comes out 1.42x too tall on 5:4. Every 2D sprite is a
//     DrawArrays call whose vertex positions are int16 (1/16 pixel, scale in shadow reg 9)
//     and whose transform is shadow reg 12 (x row), 13 (y row), 15 (position: tx, ty, 1, 0)
//     in NDC; a flush inside DrawArrays uploads the changed registers through
//     SetVertexConstant. AnchorHUD hooks DrawArrays, reads each sprite's vertices to get its
//     true on-screen box, and uploads a y transform that gives it the 16:9 shape:
//       - HUD groups (the sprites a renderer view draws, identified by their first sprite)
//         are fitted as a whole toward the top or bottom edge, learned in gameplay so
//         nothing jumps when a menu opens; groups in the middle scale around their first
//         sprite, and labels on enemies each keep their own position;
//       - "absolute" sprites (origin at the screen centre or top-left corner: menu panels,
//         the selection frame, description text, overlays) are fitted one by one;
//       - such sprites touching the 16:9 top or bottom edge stretch to the tube's edge,
//         so panels and overlays never leave a seam; full-screen ones are left alone.
//   * Clip rectangles (sub-viewports such as the iPod screen) are moved like the HUD, and
//     what is drawn inside them is left alone.
//
// Only active while the render buffer is narrower than 16:9, so 16:9+ screens are untouched.
// Config: MGS4CRT.ini next to this file. Log: MGS4CRT.log.

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#define IMAGE_BASE      0x140000000ULL
#define VA(a)           (g_base + ((a) - IMAGE_BASE))
#define A_SETVIEWPORT   0x14068b630ULL   // SetViewport(u16 x, u16 y, u16 w, u16 h)
#define A_SETCONST      0x1406652b0ULL   // SetVertexConstant(u32 reg, const float v[4])
#define A_CONST_INDEX   0x14068ba10ULL   // called by SetVertexConstant (relocated in trampoline)
#define A_DRAWARRAYS    0x1400caa30ULL   // DrawArrays(u8 mode, u32 first, u32 count)
#define A_CAMERA        0x1400b9bb0ULL   // SetupCamera(ctx, view, float scale = 1/tan(hfov/2), ...)
#define A_RENDER_CTX    0x141dddc50ULL   // the main 3D camera's render context
#define A_CUT_CALL      0x140653b94ULL   // in the cutscene camera owner (0x140652e00): call A_CUT_SOURCE
#define A_CUT_SOURCE    0x1400b9b80ULL   // small wrapper around SetupCamera (ten callers)
#define A_CUT_SOURCE_RET 0x1400b9ba0ULL  // its SetupCamera call returns here
#define A_CUT_REBUILD_RET 0x1400eb0ebULL // SetupCamera call that rebuilds the last camera returns here
#define A_MOTION_BLUR   0x140a89ab0ULL   // motion blur setup (returning 0 turns it off)
#define A_SHADOW_PARSE  0x1401428d0ULL   // ParseShadowSettings(source, std::vector<ShadowEntry> *)
#define A_SAMPLER_CALL  0x14078cc59ULL   // call [rax+0B8h]: ID3D11Device::CreateSamplerState(desc)
#define A_CUR_VIEW      0x163c9ac58ULL   // u16 current renderer view id
#define A_BUFSIZE       0x141b00000ULL   // int render.bufferSize {w, h}
#define A_SCREEN        0x143bd1158ULL   // int output size {w, h}
#define A_OUTVIEWPORT   0x143bd1168ULL   // int final blit rect {w, h, x, y} (the letterbox)
#define A_SHADOW        0x141e23b20ULL   // float4 per shadow register, as set by the game
#define A_ATTR0_DESC    0x141e23aa0ULL   // u64 packed vertex attribute 0 (position), as set by the game
#define A_ATTR0_REC     0x163c74620ULL   // applied attribute 0; +0x14 = buffer id (bound immediately)
#define A_BUFTAB        0x163c74990ULL   // vertex buffers, 32 bytes each: u32 size, u32, u8 *cpu copy
#define PE_TIMESTAMP    0x6A8CFC47u
#define REG_SCALE       9                // shadow registers
#define REG_XROW        12
#define REG_YROW        13
#define REG_POS         15
#define EDGE            0.98f            // a box reaching this far touches the 16:9 top/bottom edge
#define TRUST_FRAMES    30               // gameplay frames before a group's fit is kept in menus
#define HUD_MIN_VIEW    40               // the gameplay HUD comes after the 3D and post views

typedef uint64_t (*SetViewportFn)(uint64_t x, uint64_t y, uint64_t w, uint64_t h);
typedef uint64_t (*SetConstFn)(uint64_t reg, const float *v);
typedef uint64_t (*DrawArraysFn)(uint64_t mode, uint64_t first, uint64_t count);
typedef uint64_t (*CameraFn)(void *ctx, void *view, float scale, float a4, float a5, float a6, uint64_t a7, uint64_t a8);

static uint8_t       *g_base;
static SetViewportFn  g_orig_vp;
static SetConstFn     g_orig_const;
static DrawArraysFn   g_orig_draw;
static CameraFn       g_orig_cam;
typedef void *(*ShadowParseFn)(void *source, void *entries);
static ShadowParseFn  g_orig_shadow;
static int            g_aniso = 16, g_blur_crt = 1, g_blur_main = 0;   // [Graphics]
static int            g_shadow_crt = 4096, g_shadow_main = 2048;
static int            g_blur_screen = -1;      // screen kind the motion blur patch was set for
static int            g_enabled = 1, g_fill = 1, g_anchor = 1, g_menu_stretch, g_save_stretch, g_free_mouse = 1;
static float          g_threshold = 0.5f;
static unsigned       g_prev_view;
static char           g_dir[MAX_PATH - 16];
static FILE          *g_log;

static void logf_(const char *fmt, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}

// Narrow screen? Returns the 16:9-height / buffer-height ratio (e.g. 0.703 on 5:4), else 0.
static float narrow_ratio(int *bw_out, int *bh_out) {
    const int32_t *buf = (const int32_t *)VA(A_BUFSIZE);
    int bw = buf[0], bh = buf[1];
    if (bw_out) *bw_out = bw;
    if (bh_out) *bh_out = bh;
    if (bw <= 0 || bh <= 0 || (int64_t)bw * 9 >= (int64_t)bh * 16 - 16) return 0.0f;
    return (float)(bw * 9 / 16) / (float)bh;
}

// Narrow screen, also before the game has sized its buffer (it reads its shadow settings
// at startup, while the buffer size still holds the exe's 1920x1080 default): then the
// screen it is about to size itself to, Wine's primary display.
static int narrow_screen(void) {
    int bw, bh;
    if (narrow_ratio(&bw, &bh) != 0.0f) return 1;
    if (bw > 0 && bh > 0 && !(bw == 1920 && bh == 1080)) return 0;
    int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    return w > 0 && h > 0 && (int64_t)w * 9 < (int64_t)h * 16 - 16;
}

// ---------------------------------------------------------------- fitting rules
//
// A fit maps a 16:9 layout y to the tube: y' = a*y + b. Scaling by k = 16:9 height / tube
// height gives the proper shape; the pivot says which line stays put (+1 top edge, -1
// bottom edge, 0 centre, or a group's own origin).

typedef struct { float x0, x1, y0, y1; } Box;

static void fit_map(float k, float pivot, const Box *b, float *a, float *c) {
    float A = k, C = pivot * (1.0f - k);
    if (b && b->y1 - b->y0 > 1e-3f) {                     // b: only for sprites fitted alone
        // Panels and overlays that reach the top/bottom edge of the 16:9 layout keep
        // reaching the tube's edge; their other side follows the fit.
        int top = b->y1 >= EDGE, bot = b->y0 <= -EDGE;
        if (top || bot) {
            float y0 = bot ? b->y0 : A * b->y0 + C, y1 = top ? b->y1 : A * b->y1 + C;
            A = (y1 - y0) / (b->y1 - b->y0); C = y0 - A * b->y0;
        }
    }
    *a = A; *c = C;
}

static int g_menu_count, g_menu;          // carousel groups this frame / item or weapon menu open
static int g_hud_count, g_hud;            // gameplay HUD elements this frame / HUD on screen
static int g_hud_hold;                    // frames HUD mode is kept after the HUD was last seen
static int g_load_count, g_load;          // Load screen (title menu) this frame / on screen
static int g_save_count, g_save;          // Save screens (pause menu) this frame / on screen

// Full-screen menus are stretched when FullScreenMenus = Stretch, except the Load screen,
// which always keeps the stock layout in the middle, and the Save screens unless
// SaveScreen = Stretch.
static int save_framed(void) { return g_save && !g_load && !g_save_stretch; }
static int menu_stretched(void) { return g_menu_stretch && !g_load && !save_framed(); }

static int near2(const float *t, float x, float y) { return fabsf(t[0] - x) < 0.003f && fabsf(t[1] - y) < 0.003f; }

// Beside the centre area: the item/weapon carousel columns and their panels.
static int box_side(const Box *b) { return b->x1 < -0.5f || b->x0 > 0.5f; }

// +1: sits in the top part of the layout (its middle beyond the threshold, all of it in
// the upper half), -1: likewise in the bottom part, 0: middle.
static int box_edge(const Box *b) {
    float mid = 0.5f * (b->y0 + b->y1);
    if (mid >  g_threshold && b->y0 > 0.0f) return  1;
    if (mid < -g_threshold && b->y1 < 0.0f) return -1;
    return 0;
}

// Fit for a sprite (or clip rectangle) placed on its own. 0 = leave alone.
// Panoramic stretch (FullScreenMenus = Stretch, PanoramicStretch > 0): menus still fill the tube,
// but through a curve instead of a straight stretch, so the middle of the screen keeps
// closer to its true shape and the stretch goes into the top and bottom edges. Every
// sprite is placed by where its top and bottom land on the curve, so panels and the texts
// in them stay together.
static int g_pano;                        // strength in percent, 0 = plain stretch

static float pano_g(float k, float y) {   // 16:9 layout y -> tube y; g(+-1) = +-1
    float s = 1.0f - (float)g_pano / 100.0f * (1.0f - k);    // slope in the middle
    float e = 3.0f - 2.0f * s;                               // slope at the edges
    if (y > 1.0f)  return  1.0f + e * (y - 1.0f);
    if (y < -1.0f) return -1.0f + e * (y + 1.0f);
    return s * y + (1.0f - s) * y * y * y;
}

static void pano_map(float k, float y0, float y1, float *a, float *c) {
    if (y1 - y0 > 1e-3f) { *a = (pano_g(k, y1) - pano_g(k, y0)) / (y1 - y0); }
    else {
        float m = 0.5f * (y0 + y1), s = 1.0f - (float)g_pano / 100.0f * (1.0f - k);
        *a = fabsf(m) > 1.0f ? 3.0f - 2.0f * s : s + 3.0f * (1.0f - s) * m * m;
    }
    *c = pano_g(k, y0) - *a * y0;
}

static int single_fit(const Box *b, int hud, float *pivot) {
    int top = b->y1 >= EDGE, bot = b->y0 <= -EDGE;
    if (top && bot) return 0;                              // full height: overlay, fade
    if (!hud) { *pivot = 0.0f; return !menu_stretched(); }   // full-screen menus
    int e = box_edge(b);
    if (g_menu && box_side(b))            *pivot = -1.0f;  // carousel column: bottom edge
    else if (e)                           *pivot = (float)e;
    else if (top)                         *pivot =  1.0f;
    else if (bot)                         *pivot = -1.0f;
    else                                  *pivot =  0.0f;  // middle: stock layout
    return 1;
}

// First-sprite origins of the gameplay HUD: life bar, radar, item and weapon boxes. While
// one of them is on screen, the HUD is anchored to the tube's edges; screens without them
// (Load, pause menu, codec, title) keep the game's own 16:9 layout in the middle.
static int is_hud_origin(const float *t) {
    static const float o[4][2] = { { -0.8813f, 0.8833f }, { 0.7328f, 0.6222f },
                                   { -0.7188f, -0.7694f }, { 0.7188f, -0.7694f } };
    for (int i = 0; i < 4; i++)
        if (fabsf(t[0] - o[i][0]) < 0.003f && fabsf(t[1] - o[i][1]) < 0.003f) return 1;
    return 0;
}

// Exact x origins of the item/weapon carousel columns: icons, item labels, weapon labels.
static int is_carousel_x(float tx) {
    float a = fabsf(tx);
    return fabsf(a - 0.7188f) < 0.004f || fabsf(a - 0.8672f) < 0.004f || fabsf(a - 0.5672f) < 0.004f;
}

// Drawn at 2560x1440 scale: Solid Eye labels on enemies and other markers placed at a
// projected 3D position. The 3D already has the tube's shape, so each keeps exactly its
// own position, wherever it is on screen and whatever else shares its layer.
static int is_marker(const float *rx, const float *ry) {
    return fabsf(rx[0] - 0.00078f) < 0.00002f && fabsf(ry[1] + 0.00139f) < 0.00002f;
}

// ---------------------------------------------------------------- group memory
//
// A group is identified by its first sprite's position and scale, which are fixed for a HUD
// element. Gameplay teaches the table how each group is fitted; while a menu is open,
// groups that were steady in gameplay keep that fit and only the menu's own groups get the
// menu rules.

enum { FIT_NONE = 0, FIT_PIVOT, FIT_KEEP, FIT_SINGLE, FIT_SCREEN, FIT_FRAME };
typedef struct {
    int32_t k[4]; uint8_t used, fit, has_ext; float pivot; Box ext;
    unsigned seen, gp_last, gp_run;
} Ident;
#define NIDENT 1024
static Ident    g_ident[NIDENT];
static unsigned g_frame_no;

static Ident *ident_get(const float *pos, const float *rx, const float *ry) {
    int32_t k[4] = { (int32_t)lroundf(pos[0] * 2000.f), (int32_t)lroundf(pos[1] * 2000.f),
                     (int32_t)lroundf(rx[0] * 1e6f),    (int32_t)lroundf(ry[1] * 1e6f) };
    uint32_t h = ((uint32_t)k[0] * 73856093u) ^ ((uint32_t)k[1] * 19349663u) ^
                 ((uint32_t)k[2] * 83492791u) ^ ((uint32_t)k[3] * 2654435761u);
    Ident *oldest = NULL;
    for (int i = 0; i < 8; i++) {                       // small open-addressed bucket, LRU
        Ident *e = &g_ident[(h + i) % NIDENT];
        if (e->used && !memcmp(e->k, k, sizeof k)) return e;
        if (!e->used) { oldest = e; break; }
        if (!oldest || e->seen < oldest->seen) oldest = e;
    }
    memset(oldest, 0, sizeof *oldest);
    memcpy(oldest->k, k, sizeof k); oldest->used = 1;
    return oldest;
}

// Box of everything each view drew, this frame and the last one.
#define MAXV 1024
static Box      g_vext[MAXV], g_vext_prev[MAXV];
static uint8_t  g_vhas[MAXV], g_vhas_prev[MAXV];

static unsigned g_group_view = 0xffffffff;       // view the current group belongs to
static int      g_group_fit;
static float    g_group_pivot;
static float    g_group_anchor;                  // first sprite's y: middle groups scale around it
static int      g_group_marker;                  // a layer of markers on enemies
static Ident   *g_cur_id;                        // identity of the group being drawn...
static Box      g_cur_ext;                       // ...and the box of its sprites so far
static int      g_cur_has;

static void close_group(void) {          // remember where the finished group's sprites sat
    if (g_cur_id && g_cur_has) { g_cur_id->ext = g_cur_ext; g_cur_id->has_ext = 1; }
    g_cur_id = NULL; g_cur_has = 0;
}

static void start_group(unsigned view, const float *t, const float *rx, const float *ry, const Box *box) {
    close_group();
    g_group_view = view;
    // The gameplay HUD is drawn late in the frame, after the 3D and post-processing
    // views (from about view 54). Loading screens still draw it early and cover it, and
    // pause screens draw their own layers early too: those never count as HUD.
    int hud_view = view >= HUD_MIN_VIEW;
    // Enemy markers count too: the game hides the life bar, radar and item boxes for a
    // moment now and then while Solid Eye labels stay up.
    if ((is_hud_origin(t) || is_marker(rx, ry)) && hud_view) g_hud_count++;
    if (is_carousel_x(t[0]) && t[1] <= 0.3f && hud_view) g_menu_count++;
    if (near2(t, -0.8172f, 0.8889f) && fabsf(rx[0] - 0.00156f) < 0.00002f) g_load_count++;  // "LOAD" header
    if (near2(t, -0.8516f, 0.8639f)) g_save_count++;                                    // Save window

    Ident *id = ident_get(t, rx, ry);
    Box ext = id->has_ext ? id->ext : box ? *box : (Box){ t[0], t[0], t[1], t[1] };
    g_cur_id = id; id->seen = g_frame_no;
    g_group_anchor = t[1];
    g_group_marker = 0;

    int absolute = (fabsf(t[0]) < 2e-3f && fabsf(t[1]) < 2e-3f) || (t[0] <= -0.999f && t[1] >= 0.999f);
    if (!g_hud || !hud_view) {
        // No gameplay HUD: a full-screen menu. Center: everything keeps the stock layout
        // around the centre and only full-screen layers (backgrounds, fades) fill the tube.
        // Stretch: the whole screen is stretched to the tube, as the game draws it
        // (not the Load/Save slot list, see menu_stretched).
        g_group_fit = menu_stretched() ? FIT_NONE : FIT_SCREEN; g_group_pivot = 0.0f;
        // Save screens: the Mk. II frame, dimming and backgrounds (screen-positioned
        // layers spanning the whole height) fill the tube; the Save window stays in the
        // middle. Screen-positioned layers all start at the same corner, so they are told
        // apart by view, not by identity.
        if (save_framed() && absolute && view < MAXV && g_vhas_prev[view] &&
            g_vext_prev[view].y0 <= -EDGE && g_vext_prev[view].y1 >= EDGE) g_group_fit = FIT_FRAME;
    } else if (is_marker(rx, ry)) {
        g_group_fit = FIT_KEEP; g_group_marker = 1;
    } else if (absolute) {
        // Origin at the screen centre or corner: the sprites carry their own screen
        // positions (menu panels, selection frame, texts, overlays). Each is fitted alone.
        g_group_fit = FIT_SINGLE;
    } else if (!g_menu) {
        // Gameplay: learn. Groups entirely high/low go to that edge; everything else
        // (labels on enemies, centre text) keeps its own origin so nothing can jump.
        int e = box_edge(&ext);
        if (e) { g_group_fit = FIT_PIVOT; g_group_pivot = (float)e; }
        else     g_group_fit = FIT_KEEP;
        id->fit = (uint8_t)g_group_fit; id->pivot = g_group_pivot;
        if (id->gp_last != g_frame_no) {
            id->gp_run = id->gp_last + 1 == g_frame_no ? id->gp_run + 1 : 1;
            id->gp_last = g_frame_no;
        }
    } else if (id->fit && id->gp_run >= TRUST_FRAMES) {
        // Menu open, but this group was steady in gameplay: keep its gameplay fit.
        g_group_fit = id->fit; g_group_pivot = id->pivot;
    } else {
        // The menu's own groups: the carousel (side columns, bottom row) goes with the
        // bottom edge so an opening menu grows out of its box; the rest keeps the stock
        // layout around the centre.
        int e = box_edge(&ext);
        g_group_fit = FIT_PIVOT;
        g_group_pivot = box_side(&ext) ? -1.0f : (float)e;
    }
}

// ---------------------------------------------------------------- sprite boxes

static const uint8_t *g_rd_lo, *g_rd_hi;          // last memory region found readable

static int readable(const uint8_t *p, size_t n) {
    if (p >= g_rd_lo && p + n <= g_rd_hi) return 1;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT ||
        (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(mi.Protect & 0xEE)) return 0;
    g_rd_lo = (const uint8_t *)mi.BaseAddress; g_rd_hi = g_rd_lo + mi.RegionSize;
    return p >= g_rd_lo && p + n <= g_rd_hi;
}

// The on-screen box (16:9 layout NDC) of the sprite about to be drawn, from its vertices.
static int sprite_box(uint32_t first, uint32_t count, const float *sh, Box *b) {
    uint64_t desc = *(const uint64_t *)VA(A_ATTR0_DESC);
    unsigned stride = (desc >> 16) & 0xff, size = (desc >> 24) & 0xf, type = (desc >> 28) & 0xf;
    uint32_t off = (uint32_t)(desc >> 33);
    if (size != 2 || type != 5 || stride < 4 || count == 0 || count > 65536) return 0;  // int16 x, y
    uint32_t buf = *(const uint32_t *)(VA(A_ATTR0_REC) + 0x14);
    if (buf >= 1024) return 0;
    const uint8_t *ent = VA(A_BUFTAB) + (size_t)buf * 32;
    uint32_t bsize = *(const uint32_t *)ent;
    const uint8_t *cpu = *(const uint8_t *const *)(ent + 8);
    uint64_t lo = (uint64_t)off + (uint64_t)first * stride, len = (uint64_t)(count - 1) * stride + 4;
    if (!cpu || lo + len > bsize || !readable(cpu + lo, (size_t)len)) return 0;
    float sc = sh[REG_SCALE * 4];
    if (!(sc > 0.0f && sc < 16.0f)) return 0;

    const float *rx = sh + REG_XROW * 4, *ry = sh + REG_YROW * 4, *t = sh + REG_POS * 4;
    const uint8_t *p = cpu + lo;
    b->x0 = b->y0 = 1e9f; b->x1 = b->y1 = -1e9f;
    for (uint32_t i = 0; i < count; i++, p += stride) {
        float X = ((const int16_t *)p)[0] * sc, Y = ((const int16_t *)p)[1] * sc;
        float x = t[0] + rx[0] * X + rx[1] * Y, y = t[1] + ry[0] * X + ry[1] * Y;
        if (x < b->x0) b->x0 = x;
        if (x > b->x1) b->x1 = x;
        if (y < b->y0) b->y0 = y;
        if (y > b->y1) b->y1 = y;
    }
    return 1;
}

// ---------------------------------------------------------------- per-draw transform

enum { VP_FULL = 0, VP_CLIP, VP_OFF };
static int      g_vp_state = VP_FULL;      // last viewport: whole buffer, clip on screen, offscreen
static int      g_2d_frame;                // a HUD sprite has been drawn this frame
static float    g_xf_rx[4], g_xf_ry[4], g_xf_t[4];
static int      g_xf_on;                   // inside a draw whose constants are replaced

static int small_2d_row(const float *r) {
    return r[2] == 0.0f && r[3] == 0.0f && r[0] > -0.05f && r[0] < 0.05f && r[1] > -0.05f && r[1] < 0.05f;
}

// Decide the transform of the sprite about to be drawn. 0 = draw it as the game set it.
static int prepare_draw(uint32_t first, uint32_t count) {
    float k = narrow_ratio(NULL, NULL);
    if (k == 0.0f || g_vp_state != VP_FULL) return 0;    // clip rects are moved instead
    const float *sh = (const float *)VA(A_SHADOW);
    const float *rx = sh + REG_XROW * 4, *ry = sh + REG_YROW * 4, *t = sh + REG_POS * 4;
    if (t[2] != 1.0f || t[3] != 0.0f || !small_2d_row(rx) || !small_2d_row(ry) ||
        (rx[1] == 0.0f && ry[1] == 0.0f)) return 0;

    Box box;
    int have = sprite_box(first, count, sh, &box);
    unsigned view = *(const uint16_t *)VA(A_CUR_VIEW);
    g_2d_frame = 1;
    if (view != g_group_view) start_group(view, t, rx, ry, have ? &box : NULL);
    if (have && view < MAXV) {
        Box *v = &g_vext[view];
        if (!g_vhas[view]) { *v = box; g_vhas[view] = 1; }
        else {
            if (box.x0 < v->x0) v->x0 = box.x0;
            if (box.x1 > v->x1) v->x1 = box.x1;
            if (box.y0 < v->y0) v->y0 = box.y0;
            if (box.y1 > v->y1) v->y1 = box.y1;
        }
    }
    if (have) {
        if (!g_cur_has) { g_cur_ext = box; g_cur_has = 1; }
        else {
            if (box.x0 < g_cur_ext.x0) g_cur_ext.x0 = box.x0;
            if (box.x1 > g_cur_ext.x1) g_cur_ext.x1 = box.x1;
            if (box.y0 < g_cur_ext.y0) g_cur_ext.y0 = box.y0;
            if (box.y1 > g_cur_ext.y1) g_cur_ext.y1 = box.y1;
        }
    }

    if (g_group_fit == FIT_NONE) {
        // Stretched menu: as the game draws it, or through the panoramic curve.
        if (!g_pano || !have || !menu_stretched() || (box.y0 <= -EDGE && box.y1 >= EDGE)) return 0;
        float a, c;
        pano_map(k, box.y0, box.y1, &a, &c);
        memcpy(g_xf_rx, rx, 16); memcpy(g_xf_ry, ry, 16); memcpy(g_xf_t, t, 16);
        g_xf_ry[0] *= a; g_xf_ry[1] *= a;
        g_xf_t[1] = a * t[1] + c;
        return 1;
    }
    if (g_group_fit == FIT_FRAME) {
        // Screen-positioned pieces are stretched with the frame; anything placed from its
        // own origin (the Save window's outline, the vertical MK. II lettering) keeps the
        // stock layout around the centre like the rest of the window.
        if ((fabsf(t[0]) < 1e-4f && fabsf(t[1]) < 1e-4f) || (t[0] <= -0.999f && t[1] >= 0.999f)) return 0;
    }
    int marker = g_group_fit != FIT_SCREEN && g_group_fit != FIT_FRAME && is_marker(rx, ry);
    int fit = g_group_fit == FIT_FRAME ? FIT_SCREEN : marker ? FIT_KEEP : g_group_fit;
    // Keep: the group scales around its first sprite, so its parts stay together (the
    // aiming reticle, whose centre sways by a hair, stays in one piece); markers on
    // enemies, and everything in a layer of them (the HP bars), each scale around
    // their own position.
    float pivot = fit != FIT_KEEP ? g_group_pivot : marker || g_group_marker ? t[1] : g_group_anchor, a, c;
    int single = fit == FIT_SINGLE;
    if (single && (!have || !single_fit(&box, 1, &pivot))) return 0;
    if (g_group_fit == FIT_SCREEN && have && box.y0 <= -EDGE && box.y1 >= EDGE) return 0;
    fit_map(k, pivot, single ? &box : NULL, &a, &c);

    memcpy(g_xf_rx, rx, 16); memcpy(g_xf_ry, ry, 16); memcpy(g_xf_t, t, 16);
    g_xf_ry[0] *= a; g_xf_ry[1] *= a;
    g_xf_t[1] = a * t[1] + c;
    return 1;
}

static uint64_t hook_DrawArrays(uint64_t mode, uint64_t first, uint64_t count) {
    if (!g_enabled || !g_anchor || !prepare_draw((uint32_t)first, (uint32_t)count))
        return g_orig_draw(mode, first, count);
    // Upload the adjusted transform; the draw's own flush re-sends only registers the game
    // changed, and those are replaced too (hook_SetConst). Afterwards the game's values go
    // back, so every other draw sees exactly what the game set.
    g_orig_const(REG_XROW, g_xf_rx); g_orig_const(REG_YROW, g_xf_ry); g_orig_const(REG_POS, g_xf_t);
    g_xf_on = 1;
    uint64_t r = g_orig_draw(mode, first, count);
    g_xf_on = 0;
    const float *sh = (const float *)VA(A_SHADOW);
    g_orig_const(REG_XROW, sh + REG_XROW * 4); g_orig_const(REG_YROW, sh + REG_YROW * 4);
    g_orig_const(REG_POS, sh + REG_POS * 4);
    return r;
}

static uint64_t hook_SetConst(uint64_t reg, const float *v) {
    if (g_xf_on) {
        switch (reg & 0xffffffff) {
        case REG_XROW: return g_orig_const(reg, g_xf_rx);
        case REG_YROW: return g_orig_const(reg, g_xf_ry);
        case REG_POS:  return g_orig_const(reg, g_xf_t);
        }
    }
    return g_orig_const(reg, v);
}

// ---------------------------------------------------------------- field of view
//
// Every 3D camera setup passes its horizontal projection scale (1/tan(hfov/2); 1.8 is the
// game's usual 58 degrees) to one function. The FOV hotkeys widen or narrow the view there
// by a percentage, like a zoom, so every camera (normal, aiming, cutscenes) changes by the
// same amount: each 1% is about half a degree of the usual 58. The value is saved per
// screen (FovCRT for screens narrower than 16:9, FovMain otherwise).

static int      g_fov_crt = 100, g_fov_main = 100, g_fov_step = 1;   // percent
static int      g_vk_narrower = VK_F10, g_vk_wider = VK_F11;
static int      g_key_was;                               // last frame's key state bits
static int      g_key_held;                              // frames the key has been held
static int      g_fov_dirty;                             // changed since last saved

static int fov_percent(void) { return narrow_ratio(NULL, NULL) != 0.0f ? g_fov_crt : g_fov_main; }

// The game sets the camera up several times a frame, and later setups pass on the value the
// earlier ones stored (ours): a value we produced is passed through, not scaled again.
static float g_cam_out;

// Real-time cutscenes (CutsceneFov... = 0 keeps their framing, per screen), recognised the way Sunny Side Up
// does it: the cutscene camera owner builds its camera through A_CUT_SOURCE, and our stub
// around that one call records the thread doing it; right after, the game rebuilds that same
// camera (same context, same value) from A_CUT_REBUILD_RET.
static int            g_cut_fov_crt = 1, g_cut_fov_main = 1;   // [FOV] CutsceneFovCRT / CutsceneFovMain
static volatile DWORD *g_cut_tid;            // in the stub: thread inside the owner's call, else 0
static void          *g_cut_ctx;
static float          g_cut_scale;
static DWORD          g_cut_tick;
static int            g_cut_seen;

static int cutscene_camera(void *ctx, float scale, void *ret) {
    if (!g_cut_tid) return 0;
    if (ret == (void *)VA(A_CUT_SOURCE_RET) && *g_cut_tid && *g_cut_tid == GetCurrentThreadId()) {
        g_cut_ctx = ctx; g_cut_scale = scale; g_cut_tick = GetTickCount();
        if (!g_cut_seen) { g_cut_seen = 1; logf_("Cutscene camera seen"); }
        return 1;
    }
    return ret == (void *)VA(A_CUT_REBUILD_RET) && ctx == g_cut_ctx && GetTickCount() - g_cut_tick <= 250 &&
           fabsf(scale - g_cut_scale) <= 1e-5f * scale;
}

static uint64_t hook_Camera(void *ctx, void *view, float scale, float a4, float a5, float a6, uint64_t a7, uint64_t a8) {
    int pct = fov_percent(), cut_fov = narrow_ratio(NULL, NULL) != 0.0f ? g_cut_fov_crt : g_cut_fov_main;
    if (pct != 100 && ctx == (void *)VA(A_RENDER_CTX) && scale > 0.05f && scale < 50.0f &&
        fabsf(scale - g_cam_out) > 1e-6f * scale &&
        (cut_fov || !cutscene_camera(ctx, scale, __builtin_return_address(0)))) {
        scale *= 100.0f / (float)pct;                // tan(hfov/2) grows with the percentage
        g_cam_out = scale;
    }
    return g_orig_cam(ctx, view, scale, a4, a5, a6, a7, a8);
}

static int game_focused(void) {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static int parse_vk(const char *s, int def) {
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9') {
        int n = atoi(s + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    return s[0] ? def : 0;                           // blank: no key
}

static void save_config(void);

// Once a frame: FOV hotkeys (only while the game window has focus). A tap is one step;
// holding a key repeats after a moment, about 20 steps a second. Saved on release.
static void poll_hotkeys(void) {
    int now = 0;
    if (game_focused()) {
        if (g_vk_narrower && (GetAsyncKeyState(g_vk_narrower) & 0x8000)) now |= 1;
        if (g_vk_wider && (GetAsyncKeyState(g_vk_wider) & 0x8000)) now |= 2;
    }
    int pressed = now & ~g_key_was;
    g_key_held = now && now == g_key_was ? g_key_held + 1 : 0;
    g_key_was = now;
    int *fov = narrow_ratio(NULL, NULL) != 0.0f ? &g_fov_crt : &g_fov_main;
    int step = 0;
    if (now == 3) {
        if (pressed && *fov != 100) { *fov = 100; g_fov_dirty = 1; }   // both keys: stock
    } else if (pressed || (now && g_key_held > 20 && g_key_held % 3 == 0)) {
        step = now & 1 ? -g_fov_step : now & 2 ? g_fov_step : 0;
    }
    if (step) {
        int v = *fov + step;
        if (v < 50) v = 50;
        if (v > 200) v = 200;
        if (v != *fov) { *fov = v; g_fov_dirty = 1; }
    }
    if (!now && g_fov_dirty) {
        g_fov_dirty = 0;
        logf_("Field of view %d%% (%s)", *fov, fov == &g_fov_crt ? "FovCRT" : "FovMain");
        save_config();
    }
}

// ---------------------------------------------------------------- viewport: fill + clip rects

static void check_config(void);
static void apply_motion_blur(void);

static uint64_t hook_SetViewport(uint64_t x, uint64_t y, uint64_t w, uint64_t h) {
    // Track frames: view ids climb through the frame and restart low on the next one.
    unsigned view = *(const uint16_t *)VA(A_CUR_VIEW);
    if (view + 4 < g_prev_view) {             // new frame
        close_group(); g_group_view = 0xffffffff;
        // The item/weapon menus always show the gameplay HUD (life bar); the pause menu's
        // Items screen lines its icons up on the same columns but has no HUD.
        // HUD mode is held for about a second after the HUD was last drawn, so the game
        // hiding it briefly never flips the layout. Pause, loading and Load/Save screens
        // draw in early views and switch at once regardless (see HUD_MIN_VIEW).
        if (g_hud_count > 0) g_hud_hold = 60; else if (g_hud_hold > 0) g_hud_hold--;
        g_hud = g_hud_hold > 0; g_hud_count = 0;
        g_menu = g_hud && g_menu_count >= 8; g_menu_count = 0; g_frame_no++;
        g_load = g_load_count > 0; g_load_count = 0;
        g_save = g_save_count > 0; g_save_count = 0;
        memcpy(g_vext_prev, g_vext, sizeof g_vext); memcpy(g_vhas_prev, g_vhas, sizeof g_vhas);
        memset(g_vhas, 0, sizeof g_vhas);

        if (g_frame_no % 60 == 0) check_config();
        if ((narrow_ratio(NULL, NULL) != 0.0f) != g_blur_screen) apply_motion_blur();
        poll_hotkeys();
        g_2d_frame = 0;
    }
    g_prev_view = view;
    if (!g_enabled) return g_orig_vp(x, y, w, h);

    int bw, bh;
    float k = narrow_ratio(&bw, &bh);
    int X = (int)(x & 0xffff), Y = (int)(y & 0xffff), W = (int)(w & 0xffff), H = (int)(h & 0xffff);
    int full = X == 0 && Y == 0 && W == bw && H == bh;
    int clip = !full && (X || Y) && X + W <= bw && Y + H <= bh && g_2d_frame;
    g_vp_state = full ? VP_FULL : clip ? VP_CLIP : VP_OFF;
    if (k == 0.0f) return g_orig_vp(x, y, w, h);

    if (X == 0 && Y == 0 && g_fill) {
        // The 3D is rendered at the buffer's own (narrow) aspect: show all of it
        // instead of the game's 16:9 letterbox rectangle.
        const int32_t *scr = (const int32_t *)VA(A_SCREEN);
        int32_t *vp = (int32_t *)VA(A_OUTVIEWPORT);
        if (scr[0] > 0 && scr[1] > 0 && (vp[0] != scr[0] || vp[1] != scr[1] || vp[2] || vp[3])) {
            vp[0] = scr[0]; vp[1] = scr[1]; vp[2] = 0; vp[3] = 0;
        }
    }
    if (clip && g_anchor) {
        // A clip rectangle for part of the HUD (the iPod screen): the game places it for the
        // 16:9 layout, so move and shape it like the HUD around it. Its contents then need
        // no adjustment of their own.
        float yt = 1.0f - 2.0f * Y / bh, yb = 1.0f - 2.0f * (Y + H) / bh, pivot, a, c;
        Box b = { 2.0f * X / bw - 1.0f, 2.0f * (X + W) / bw - 1.0f, yb, yt };
        int hud = g_hud && view >= HUD_MIN_VIEW;
        int pano = !hud && g_pano && menu_stretched() && !(yb <= -EDGE && yt >= EDGE);
        if (pano || single_fit(&b, hud, &pivot)) {
            if (pano) pano_map(k, yb, yt, &a, &c);
            else      fit_map(k, pivot, &b, &a, &c);
            float nt = a * yt + c, nb = a * yb + c;
            long ny = lroundf((1.0f - nt) * bh / 2), nh = lroundf((nt - nb) * bh / 2);
            if (ny >= 0 && nh > 0 && ny + nh <= bh) { y = (uint64_t)ny; h = (uint64_t)nh; }
        }
    }
    return g_orig_vp(x, y, w, h);
}

// ---------------------------------------------------------------- config

static char     g_ini[MAX_PATH];
static FILETIME g_ini_time;

static void load_config(void) {
    char *ini = g_ini, s[64];
    snprintf(g_ini, sizeof g_ini, "%s\\MGS4CRT.ini", g_dir);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExA(ini, GetFileExInfoStandard, &fa)) g_ini_time = fa.ftLastWriteTime;
    g_enabled = GetPrivateProfileIntA("HUD", "Enabled", 1, ini);
    g_fill    = GetPrivateProfileIntA("HUD", "FillScreen", 1, ini);
    g_anchor  = GetPrivateProfileIntA("HUD", "AnchorHUD", 1, ini);
    GetPrivateProfileStringA("HUD", "AnchorThreshold", "0.5", s, sizeof s, ini);
    g_threshold = (float)atof(s);
    GetPrivateProfileStringA("HUD", "FullScreenMenus", "Center", s, sizeof s, ini);
    g_menu_stretch = _stricmp(s, "Stretch") == 0;
    g_pano = g_menu_stretch ? GetPrivateProfileIntA("HUD", "PanoramicStretch", 0, ini) : 0;
    if (g_pano < 0) g_pano = 0;
    if (g_pano > 100) g_pano = 100;
    g_free_mouse = GetPrivateProfileIntA("HUD", "FreeMouse", 1, ini);
    g_fov_crt  = GetPrivateProfileIntA("FOV", "FovCRT", 100, ini);
    g_fov_main = GetPrivateProfileIntA("FOV", "FovMain", 100, ini);
    if (g_fov_crt < 50 || g_fov_crt > 200) g_fov_crt = 100;
    if (g_fov_main < 50 || g_fov_main > 200) g_fov_main = 100;
    g_fov_step = GetPrivateProfileIntA("FOV", "FovStep", 1, ini);
    if (g_fov_step < 1) g_fov_step = 1;
    g_cut_fov_crt  = GetPrivateProfileIntA("FOV", "CutsceneFovCRT", 1, ini) != 0;
    g_cut_fov_main = GetPrivateProfileIntA("FOV", "CutsceneFovMain", 1, ini) != 0;
    GetPrivateProfileStringA("FOV", "NarrowerHotkey", "F10", s, sizeof s, ini);
    g_vk_narrower = parse_vk(s, VK_F10);
    GetPrivateProfileStringA("FOV", "WiderHotkey", "F11", s, sizeof s, ini);
    g_vk_wider = parse_vk(s, VK_F11);
    g_aniso      = GetPrivateProfileIntA("Graphics", "AnisotropicFiltering", 16, ini);
    g_blur_crt   = GetPrivateProfileIntA("Graphics", "MotionBlurCRT", 1, ini);
    g_blur_main  = GetPrivateProfileIntA("Graphics", "MotionBlurMain", 0, ini);
    g_shadow_crt  = GetPrivateProfileIntA("Graphics", "ShadowResolutionCRT", 4096, ini);
    g_shadow_main = GetPrivateProfileIntA("Graphics", "ShadowResolutionMain", 2048, ini);
    if (g_shadow_crt > 4096) g_shadow_crt = 4096;
    if (g_shadow_main > 4096) g_shadow_main = 4096;
    GetPrivateProfileStringA("HUD", "SaveScreen", "Center", s, sizeof s, ini);
    g_save_stretch = _stricmp(s, "Stretch") == 0;
    logf_("Enabled = %d  FillScreen = %d  AnchorHUD = %d  AnchorThreshold = %.2f  FullScreenMenus = %s  SaveScreen = %s",
          g_enabled, g_fill, g_anchor, g_threshold, g_menu_stretch ? "Stretch" : "Center",
          g_save_stretch ? "Stretch" : "Center");
    if (g_pano) logf_("PanoramicStretch = %d%%", g_pano);
    logf_("FOV: CRT %d%%, Main %d%%, step %d%%; cutscenes CRT %s, Main %s", g_fov_crt, g_fov_main, g_fov_step,
          g_cut_fov_crt ? "included" : "own framing", g_cut_fov_main ? "included" : "own framing");
    logf_("Graphics: AnisotropicFiltering %d, MotionBlur CRT %d Main %d, Shadows CRT %d Main %d",
          g_aniso, g_blur_crt, g_blur_main, g_shadow_crt, g_shadow_main);
}

// Store the FOV offsets (the hotkeys change them).
static void save_config(void) {
    char v[16];
    snprintf(v, sizeof v, "%d", g_fov_crt);  WritePrivateProfileStringA("FOV", "FovCRT", v, g_ini);
    snprintf(v, sizeof v, "%d", g_fov_main); WritePrivateProfileStringA("FOV", "FovMain", v, g_ini);
    WIN32_FILE_ATTRIBUTE_DATA fa;                    // our own write is not an edit to reload
    if (GetFileAttributesExA(g_ini, GetFileExInfoStandard, &fa)) g_ini_time = fa.ftLastWriteTime;
}

// Pick up ini edits while the game runs (checked about once a second).
static void check_config(void) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(g_ini, GetFileExInfoStandard, &fa) ||
        CompareFileTime(&fa.ftLastWriteTime, &g_ini_time) == 0) return;
    logf_("MGS4CRT.ini changed, reloading");
    load_config();
    apply_motion_blur();
}

static uint8_t *emit_jmp_abs(uint8_t *p, uint64_t to);

// ---------------------------------------------------------------- graphics
//
// Ported from MGSPatriotFix by ShizCalev/Afevis (MIT License, Copyright (c) 2026 Afevis):
// Disable Motion Blur (by cipherxof), Custom Shadow Resolution (by Emoose) and the
// anisotropic filtering override, rewritten here for this exe without its hooking library.

// The shadow settings the game parses are a vector of 24-byte entries; field 5 is the
// shadow buffer size (the game mixes 512, 1024 and 2048).
typedef struct { int32_t field, index, unknown[2], value, padding; } ShadowEntry;
typedef struct { ShadowEntry *first, *last, *capacity; } ShadowEntryVector;

static void *hook_ShadowParse(void *source, void *entries) {
    void *r = g_orig_shadow(source, entries);
    ShadowEntryVector *v = (ShadowEntryVector *)entries;
    int crt = narrow_screen();
    int g_shadow_res = crt ? g_shadow_crt : g_shadow_main;   // per screen
    const int32_t *buf = (const int32_t *)VA(A_BUFSIZE);
    logf_("Shadow settings read: buffer %dx%d, display %dx%d -> %s", buf[0], buf[1],
          GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), crt ? "CRT" : "Main");
    if (g_shadow_res > 0 && v && v->first && v->last >= v->first && v->last - v->first < 4096) {
        int n = 0, lo = 1 << 30, hi = 0;
        for (ShadowEntry *e = v->first; e != v->last; e++)
            if (e->field == 5) {
                if (e->value < lo) lo = e->value;
                if (e->value > hi) hi = e->value;
                e->value = g_shadow_res; n++;
            }
        if (n) logf_("Shadow settings: %d buffer sizes (were %d-%d) set to %d (%s)", n, lo, hi, g_shadow_res,
                     crt ? "CRT" : "Main");
    }
    return r;
}

static const uint8_t kBlurHead[3] = { 0x40, 0x53, 0x48 };               // push rbx; sub rsp,...
static const uint8_t kBlurOff[3]  = { 0x31, 0xc0, 0xc3 };               // xor eax,eax; ret

// Motion blur is set per screen (on the CRT it smooths 60 fps motion on a display that
// flashes each frame; on the LCD at high frame rates it only smears) and switches live:
// patch or restore the function's start.
static void apply_motion_blur(void) {
    uint8_t *f = VA(A_MOTION_BLUR);
    int crt = narrow_ratio(NULL, NULL) != 0.0f;
    int off = !(crt ? g_blur_crt : g_blur_main);
    g_blur_screen = crt;
    const uint8_t *want = off ? kBlurOff : kBlurHead;
    if (memcmp(f, kBlurHead, 3) && memcmp(f, kBlurOff, 3)) return;   // someone else's code
    if (!memcmp(f, want, 3)) return;
    DWORD old;
    if (!VirtualProtect(f, 3, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(f, want, 3);
    VirtualProtect(f, 3, old, &old);
    FlushInstructionCache(GetCurrentProcess(), f, 3);
    logf_("Motion blur %s (%s)", off ? "off" : "on", crt ? "CRT" : "Main");
}

// Executable memory within 2 GB of the exe, for a 5-byte jump.
static uint8_t *alloc_near(uint8_t *target, size_t n) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t g = si.dwAllocationGranularity, t = (uintptr_t)target & ~(g - 1);
    for (uintptr_t d = g; d < 0x70000000; d += g) {
        void *p = VirtualAlloc((void *)(t - d), n, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (p) return p;
    }
    return NULL;
}

// Samplers the game asks 8x anisotropic filtering for get AnisotropicFiltering instead:
// the 6-byte call becomes a jump to a stub that edits D3D11_SAMPLER_DESC.MaxAnisotropy
// (offset 20, in rdx) and then makes the original call.
static int patch_anisotropy(void) {
    static const uint8_t kCall[9] = { 0xff, 0x90, 0xb8, 0x00, 0x00, 0x00, 0x4c, 0x8b, 0x4e };
    uint8_t *site = VA(A_SAMPLER_CALL);
    if (g_aniso <= 0 || g_aniso == 8 || g_aniso > 16 || memcmp(site, kCall, sizeof kCall)) return 0;
    uint8_t *st = alloc_near(site, 64), *p = st;
    if (!st) return 0;
    const uint8_t cmp8[] = { 0x83, 0x7a, 0x14, 0x08, 0x75, 0x07 };      // cmp dword [rdx+14h],8; jne +7
    memcpy(p, cmp8, sizeof cmp8); p += sizeof cmp8;
    p[0] = 0xc7; p[1] = 0x42; p[2] = 0x14; *(int32_t *)(p + 3) = g_aniso; p += 7;   // mov dword [rdx+14h],N
    memcpy(p, kCall, 6); p += 6;                                          // call [rax+0B8h]
    emit_jmp_abs(p, (uint64_t)(uintptr_t)(site + 6));
    uint8_t patch[6] = { 0xe9, 0, 0, 0, 0, 0x90 };
    *(int32_t *)(patch + 1) = (int32_t)(st - (site + 5));
    DWORD old;
    if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(site, patch, 6);
    VirtualProtect(site, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    return 1;
}

// The cutscene camera owner's call to A_CUT_SOURCE goes through a stub that notes which
// thread is inside it (TEB thread id, gs:[48h]) for cutscene_camera. A_CUT_SOURCE takes its
// arguments in registers only, so the extra frame changes nothing for it.
static int patch_cutscene_call(void) {
    static const uint8_t kSourceHead[8] = { 0x48, 0x83, 0xec, 0x38, 0xf3, 0x0f, 0x10, 0x05 };
    uint8_t *site = VA(A_CUT_CALL), *src = VA(A_CUT_SOURCE);
    if (site[0] != 0xe8 || site + 5 + *(int32_t *)(site + 1) != src ||
        memcmp(src, kSourceHead, sizeof kSourceHead) ||
        VA(A_CUT_REBUILD_RET)[-5] != 0xe8 ||
        VA(A_CUT_REBUILD_RET) + *(int32_t *)(VA(A_CUT_REBUILD_RET) - 4) != VA(A_CAMERA)) return 0;
    uint8_t *st = alloc_near(site, 64), *p = st;
    if (!st) return 0;
    volatile DWORD *tid = (volatile DWORD *)(st + 48);
    static const uint8_t kTid[8] = { 0x65, 0x8b, 0x04, 0x25, 0x48, 0x00, 0x00, 0x00 };   // mov eax, gs:[48h]
    memcpy(p, kTid, 8); p += 8;
    p[0] = 0x89; p[1] = 0x05; *(int32_t *)(p + 2) = (int32_t)((uint8_t *)tid - (p + 6)); p += 6;   // mov [tid], eax
    p[0] = 0x48; p[1] = 0x83; p[2] = 0xec; p[3] = 0x28; p += 4;                                  // sub rsp, 28h
    p[0] = 0x48; p[1] = 0xb8; *(uint64_t *)(p + 2) = (uint64_t)(uintptr_t)src; p += 10;          // mov rax, source
    p[0] = 0xff; p[1] = 0xd0; p += 2;                                                            // call rax
    p[0] = 0x48; p[1] = 0x83; p[2] = 0xc4; p[3] = 0x28; p += 4;                                  // add rsp, 28h
    p[0] = 0xc7; p[1] = 0x05; *(int32_t *)(p + 2) = (int32_t)((uint8_t *)tid - (p + 10));        // mov dword [tid], 0
    *(int32_t *)(p + 6) = 0; p += 10;
    *p = 0xc3;                                                                                   // ret
    int64_t rel = st - (site + 5);
    if (rel != (int32_t)rel) return 0;
    DWORD old;
    if (!VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return 0;
    g_cut_tid = tid;
    *(int32_t *)(site + 1) = (int32_t)rel;
    VirtualProtect(site + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return 1;
}

// ---------------------------------------------------------------- free mouse
//
// The game and Wine's DirectInput re-centre the cursor on the game window (SetCursorPos)
// and clip it there (ClipCursor). Under XWayland that holds the real desktop cursor, so the
// mouse can never leave the game and focus snaps back to it. FreeMouse makes SetCursorPos
// do nothing and turns every ClipCursor into a release, for all callers: Wine's user32
// exports start with an 8-byte hot-patch pad (lea rsp,[rsp+0]) that is overwritten. The
// game's own import table is patched too, as a fallback. The game reads the mouse as raw
// input.

static BOOL WINAPI stub_SetCursorPos(int x, int y) { (void)x; (void)y; return TRUE; }
static BOOL WINAPI stub_ClipCursor(const RECT *r) { (void)r; return TRUE; }

static int patch_import(const char *dll, const char *fn, void *repl) {
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(g_base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((const char *)(g_base + d->Name), dll) != 0) continue;
        IMAGE_THUNK_DATA64 *iat = (IMAGE_THUNK_DATA64 *)(g_base + d->FirstThunk);
        IMAGE_THUNK_DATA64 *names = d->OriginalFirstThunk ? (IMAGE_THUNK_DATA64 *)(g_base + d->OriginalFirstThunk) : iat;
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(g_base + names->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, fn) != 0) continue;
            DWORD old;
            if (!VirtualProtect(&iat->u1.Function, 8, PAGE_READWRITE, &old)) return 0;
            iat->u1.Function = (ULONGLONG)(uintptr_t)repl;
            VirtualProtect(&iat->u1.Function, 8, old, &old);
            return 1;
        }
    }
    return 0;
}

// Overwrite the hot-patch pad at the start of a user32 export.
static int patch_pad(const char *fn, const uint8_t repl[8]) {
    static const uint8_t pad[8] = { 0x48, 0x8d, 0xa4, 0x24, 0x00, 0x00, 0x00, 0x00 };
    uint8_t *f = (uint8_t *)GetProcAddress(GetModuleHandleA("user32.dll"), fn);
    DWORD old;
    if (!f || memcmp(f, pad, 8) != 0 || !VirtualProtect(f, 8, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(f, repl, 8);
    VirtualProtect(f, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), f, 8);
    return 1;
}

static void free_mouse(void) {
    if (!g_free_mouse) return;
    static const uint8_t release[8] = { 0x31, 0xc9, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };  // xor ecx,ecx
    static const uint8_t noop[8]    = { 0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3, 0x90, 0x90 };  // mov eax,1; ret
    int c = patch_pad("ClipCursor", release), w = patch_pad("SetCursorPos", noop);
    logf_("FreeMouse: user32 ClipCursor %s, SetCursorPos %s", c ? "releases" : "NOT PATCHED", w ? "off" : "NOT PATCHED");
    int a = patch_import("user32.dll", "SetCursorPos", (void *)stub_SetCursorPos);
    int b = patch_import("user32.dll", "ClipCursor", (void *)stub_ClipCursor);
    logf_("FreeMouse: import table SetCursorPos %s, ClipCursor %s", a ? "off" : "not found", b ? "off" : "not found");
}

// ---------------------------------------------------------------- hooking

static void *alloc_exec(size_t n) {
    return VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}

static uint8_t *emit_jmp_abs(uint8_t *p, uint64_t to) {   // jmp [rip+0]; dq to
    p[0] = 0xFF; p[1] = 0x25; *(uint32_t *)(p + 2) = 0; *(uint64_t *)(p + 6) = to;
    return p + 14;
}

static int write_detour(uint8_t *target, size_t stolen, void *hook) {
    uint8_t patch[32];
    memset(patch, 0x90, sizeof patch);
    emit_jmp_abs(patch, (uint64_t)(uintptr_t)hook);
    DWORD old;
    if (!VirtualProtect(target, stolen, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(target, patch, stolen);
    VirtualProtect(target, stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, stolen);
    return 1;
}

// SetViewport starts with three 5-byte "mov [rsp+n], reg": copy them verbatim.
static const uint8_t kVpHead[15] = {
    0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18 };

// SetVertexConstant: push rbx; sub rsp,20h; mov rbx,rdx; call rel32; movups xmm0,[rbx]
static const uint8_t kConstHead[17] = {
    0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xda, 0xe8, 0x52, 0x67, 0x02, 0x00, 0x0f, 0x10, 0x03 };

// DrawArrays: mov [rsp+8],rbx; mov [rsp+10h],rsi; push rdi; sub rsp,20h
static const uint8_t kDrawHead[15] = {
    0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20 };

// ParseShadowSettings: mov [rsp+8],rbx; mov [rsp+10h],rsi; mov [rsp+18h],rdi
static const uint8_t kShadowHead[15] = {
    0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7c, 0x24, 0x18 };

// SetupCamera: mov rax,rsp; push rbx; push rsi; push rdi; sub rsp,100h; movaps [rax-28h],xmm6
static const uint8_t kCamHead[17] = {
    0x48, 0x8b, 0xc4, 0x53, 0x56, 0x57, 0x48, 0x81, 0xec, 0x00, 0x01, 0x00, 0x00, 0x0f, 0x29, 0x70, 0xd8 };

static int heads_match(void) {
    return memcmp(VA(A_SETVIEWPORT), kVpHead, sizeof kVpHead) == 0 &&
           memcmp(VA(A_SETCONST), kConstHead, sizeof kConstHead) == 0 &&
           memcmp(VA(A_DRAWARRAYS), kDrawHead, sizeof kDrawHead) == 0;
}

static int install_hooks(void) {
    if (!heads_match()) return 0;
    uint8_t *tr = alloc_exec(320);
    if (!tr) { logf_("ERROR: VirtualAlloc failed"); return -1; }

    // Trampoline 1: SetViewport's first 15 bytes, then back.
    uint8_t *t1 = tr;
    memcpy(t1, kVpHead, 15);
    emit_jmp_abs(t1 + 15, (uint64_t)(uintptr_t)(VA(A_SETVIEWPORT) + 15));
    g_orig_vp = (SetViewportFn)t1;

    // Trampoline 2: SetVertexConstant's head with the rel32 call made absolute.
    uint8_t *t2 = tr + 64, *p = t2;
    memcpy(p, kConstHead, 9); p += 9;                                   // push rbx; sub rsp; mov rbx,rdx
    p[0] = 0x48; p[1] = 0xB8; *(uint64_t *)(p + 2) = (uint64_t)(uintptr_t)VA(A_CONST_INDEX); p += 10; // mov rax, imm64
    p[0] = 0xFF; p[1] = 0xD0; p += 2;                                   // call rax
    memcpy(p, kConstHead + 14, 3); p += 3;                              // movups xmm0,[rbx]
    emit_jmp_abs(p, (uint64_t)(uintptr_t)(VA(A_SETCONST) + 17));
    g_orig_const = (SetConstFn)t2;

    // Trampoline 3: DrawArrays' first 15 bytes, then back.
    uint8_t *t3 = tr + 128;
    memcpy(t3, kDrawHead, 15);
    emit_jmp_abs(t3 + 15, (uint64_t)(uintptr_t)(VA(A_DRAWARRAYS) + 15));
    g_orig_draw = (DrawArraysFn)t3;

    // Trampoline 4: SetupCamera's first 17 bytes, then back (only if nothing else hooked it).
    int cam = memcmp(VA(A_CAMERA), kCamHead, sizeof kCamHead) == 0;
    if (cam) {
        uint8_t *t4 = tr + 192;
        memcpy(t4, kCamHead, 17);
        emit_jmp_abs(t4 + 17, (uint64_t)(uintptr_t)(VA(A_CAMERA) + 17));
        g_orig_cam = (CameraFn)t4;
    }

    if (!write_detour(VA(A_SETVIEWPORT), 15, hook_SetViewport) ||
        !write_detour(VA(A_SETCONST), 17, hook_SetConst) ||
        !write_detour(VA(A_DRAWARRAYS), 15, hook_DrawArrays) ||
        (cam && !write_detour(VA(A_CAMERA), 17, hook_Camera))) {
        logf_("ERROR: VirtualProtect failed");
        return -1;
    }
    if (!cam) logf_("FOV hotkeys off: the camera setup code is not the expected one");
    // Only when this screen keeps cutscenes at their own framing: otherwise the game's
    // cutscene code runs exactly as it ships.
    else if ((narrow_screen() ? g_cut_fov_crt : g_cut_fov_main) != 0)
        logf_("Cutscenes follow the field of view on this screen: cutscene camera not hooked");
    else if (patch_cutscene_call()) logf_("Cutscene camera call hooked (cutscenes keep their own framing)");
    else logf_("Cutscene camera call not recognised: cutscenes follow the field of view");

    // Graphics (independent of the screen shape).
    if ((g_shadow_crt > 0 || g_shadow_main > 0) && memcmp(VA(A_SHADOW_PARSE), kShadowHead, sizeof kShadowHead) == 0) {
        uint8_t *t5 = tr + 256;
        memcpy(t5, kShadowHead, 15);
        emit_jmp_abs(t5 + 15, (uint64_t)(uintptr_t)(VA(A_SHADOW_PARSE) + 15));
        g_orig_shadow = (ShadowParseFn)t5;
        if (write_detour(VA(A_SHADOW_PARSE), 15, hook_ShadowParse)) logf_("Shadow resolution hooked");
    }
    apply_motion_blur();
    if (patch_anisotropy()) logf_("Anisotropic filtering %dx for the game's 8x samplers", g_aniso);
    return 1;
}

static DWORD WINAPI wait_and_install(LPVOID arg) {
    (void)arg;
    // Steam's DRM may still be decrypting the code; wait for the expected bytes.
    for (int i = 0; i < 1200; i++) {                 // up to 60 s
        int r = install_hooks();
        if (r == 1) { logf_("Hooks installed after %.2fs", i * 0.05); return 0; }
        if (r < 0) return 0;
        Sleep(50);
    }
    logf_("ERROR: code bytes never matched; this game version is not supported. Nothing patched.");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);

    GetModuleFileNameA(inst, g_dir, sizeof g_dir);
    char *slash = strrchr(g_dir, '\\'); if (slash) *slash = 0;
    char logp[MAX_PATH]; snprintf(logp, sizeof logp, "%s\\MGS4CRT.log", g_dir);
    g_log = fopen(logp, "w");
    logf_("MGS4CRT 2.30 - native 5:4 fill + edge-anchored HUD on narrow screens");

    // Only run inside mgs4.exe (the loader also sits next to the launcher).
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, sizeof exe);
    const char *name = strrchr(exe, '\\'); name = name ? name + 1 : exe;
    if (_stricmp(name, "mgs4.exe") != 0) { logf_("Not mgs4.exe (%s), idle.", name); return TRUE; }

    g_base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != PE_TIMESTAMP) {
        logf_("Unsupported mgs4.exe (PE timestamp 0x%08lX, want 0x%08X). Nothing patched.",
              (unsigned long)nt->FileHeader.TimeDateStamp, PE_TIMESTAMP);
        return TRUE;
    }
    load_config();
    free_mouse();
    if (install_hooks() == 1) logf_("Hooks installed at load");
    else CreateThread(NULL, 0, wait_and_install, NULL, 0, NULL);
    return TRUE;
}
