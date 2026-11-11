/*
 * Abed's Adventure — an original side-scrolling platformer by Abed Mihilia.
 *
 * The Rust Bugs stole the gears of Abed's great machine. Run, jump and stomp
 * through three worlds, collect the gears and reach the portal at the end of
 * each world. Power-up mushrooms grow you (teal), give a double jump (purple)
 * or an extra life (gold).
 *
 * Drawn in software on a 400x240 RGB565 surface (scaled x2 by the launcher).
 * Controls: D-pad / stick move, A jump, B or Y run, START pause.
 * Touch: on-screen arrows, A (jump), B (run), and the X at the top right exits.
 */
#include "papp/papp.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

PAPP_MANIFEST(.id = "abed.adventure", .name = "Abed Adventure", .version = PAPP_APP_VERSION, .main_stack = 32768);

#define W     400
#define H     240
#define T     16
#define ROWS  15
#define MAXW  240
#define KEY   0xF81F

/* ══ pixels ═══════════════════════════════════════════════════════════════ */
static uint16_t *px;
static int stride;

static inline uint16_t rgb(int r, int g, int b)
{
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return PAPP_RGB565(r, g, b);
}

/* a = 0..32 */
static inline uint16_t blend(uint16_t d, uint16_t s, int a)
{
    uint32_t dd = (d | ((uint32_t)d << 16)) & 0x07E0F81F;
    uint32_t ss = (s | ((uint32_t)s << 16)) & 0x07E0F81F;
    uint32_t r = ((((ss - dd) * a) >> 5) + dd) & 0x07E0F81F;
    return (uint16_t)(r | (r >> 16));
}

static inline void pset(int x, int y, uint16_t c)
{
    if ((unsigned)x < W && (unsigned)y < H) px[y * stride + x] = c;
}

static inline void pmix(int x, int y, uint16_t c, int a)
{
    if ((unsigned)x < W && (unsigned)y < H) {
        uint16_t *p = &px[y * stride + x];
        *p = a >= 32 ? c : blend(*p, c, a);
    }
}

static void fill_rect(int x, int y, int w, int h, uint16_t c, int a)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > W ? W : x + w, y1 = y + h > H ? H : y + h;
    for (int j = y0; j < y1; j++) {
        uint16_t *row = &px[j * stride];
        for (int i = x0; i < x1; i++) row[i] = a >= 32 ? c : blend(row[i], c, a);
    }
}

static void disc(float cx, float cy, float r, uint16_t c, int a)
{
    int x0 = (int)floorf(cx - r), x1 = (int)ceilf(cx + r), y0 = (int)floorf(cy - r), y1 = (int)ceilf(cy + r);
    float r2 = r * r;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            if (dx * dx + dy * dy <= r2) pmix(x, y, c, a);
        }
}

static void ring(float cx, float cy, float rx, float ry, float t, uint16_t c, int a)
{
    int x0 = (int)(cx - rx - t) - 1, x1 = (int)(cx + rx + t) + 1;
    int y0 = (int)(cy - ry - t) - 1, y1 = (int)(cy + ry + t) + 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float dx = (x + 0.5f - cx) / rx, dy = (y + 0.5f - cy) / ry;
            float d = sqrtf(dx * dx + dy * dy);
            if (fabsf(d - 1.0f) * (rx < ry ? rx : ry) < t) pmix(x, y, c, a);
        }
}

/* ══ random ═══════════════════════════════════════════════════════════════ */
static uint32_t seed = 0x2468ACE;
static float frand(void) { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; }
static float frange(float a, float b) { return a + (b - a) * frand(); }
static uint32_t hash2(int x, int y) { uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u; h = (h ^ (h >> 13)) * 1274126177u; return h ^ (h >> 16); }


/* tiny formatter (no stdio): out = pre + v (zero-padded to width) + post */
static void sfmt(char *out, const char *pre, int v, int width, const char *post)
{
    char num[12]; int n = 0;
    unsigned u = v < 0 ? (unsigned)-v : (unsigned)v;
    do { num[n++] = (char)('0' + u % 10); u /= 10; } while (u && n < 11);
    while (n < width && n < 11) num[n++] = '0';
    int k = 0;
    while (*pre) out[k++] = *pre++;
    if (v < 0) out[k++] = '-';
    while (n) out[k++] = num[--n];
    while (post && *post) out[k++] = *post++;
    out[k] = 0;
}

/* ══ font (5x7) ═══════════════════════════════════════════════════════════ */
static const char FONT_CHARS[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:-!'.x/*<>";
static const uint8_t FONT[][7] = {
    {0, 0, 0, 0, 0, 0, 0},
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    {0x0E, 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11}, {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C},
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C},
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11},
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},
    {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},
    {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}, {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00},
    {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}, {0x0C, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}, {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11},
    {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00}, {0x04, 0x15, 0x0E, 0x1B, 0x0E, 0x15, 0x04},
    {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02}, {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08},
};

static void draw_char(int x, int y, char ch, uint16_t c, int sc)
{
    const char *p = strchr(FONT_CHARS, ch);
    if (!p || !ch) return;
    const uint8_t *g = FONT[p - FONT_CHARS];
    for (int r = 0; r < 7; r++)
        for (int k = 0; k < 5; k++)
            if (g[r] & (0x10 >> k)) fill_rect(x + k * sc, y + r * sc, sc, sc, c, 32);
}

static int text_w(const char *s, int sc) { return (int)strlen(s) * 6 * sc - sc; }

static void text(int x, int y, const char *s, uint16_t c, int sc)
{
    for (int i = 0; s[i]; i++) {
        draw_char(x + i * 6 * sc + sc, y + sc, s[i], rgb(10, 10, 20), sc);   /* shadow */
        draw_char(x + i * 6 * sc, y, s[i], c, sc);
    }
}

static void text_c(int y, const char *s, uint16_t c, int sc) { text((W - text_w(s, sc)) / 2, y, s, c, sc); }

/* big title text with a thick outline */
static void text_outline(int x, int y, const char *s, uint16_t c, uint16_t o, int sc)
{
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            for (int i = 0; s[i]; i++) draw_char(x + i * 6 * sc + dx * 2, y + dy * 2 + 2, s[i], o, sc);
    for (int i = 0; s[i]; i++) draw_char(x + i * 6 * sc, y, s[i], c, sc);
}

/* ══ sprites ══════════════════════════════════════════════════════════════ */
typedef struct { int w, h; uint16_t *p; } spr_t;

static uint16_t pool[96 * 1024];
static int pool_used;

static uint16_t *pool_get(int n)
{
    if (pool_used + n > (int)(sizeof(pool) / 2)) return NULL;
    uint16_t *p = &pool[pool_used];
    pool_used += n;
    return p;
}

static uint16_t pal(char ch, uint16_t X, uint16_t Y)
{
    switch (ch) {
    case 'K': return rgb(22, 20, 30);
    case 'S': return rgb(238, 190, 146);
    case 's': return rgb(200, 146, 106);
    case 'H': return rgb(64, 42, 30);
    case 'G': return rgb(30, 30, 44);
    case 'W': return rgb(255, 255, 255);
    case 'w': return rgb(200, 210, 225);
    case 'O': return rgb(255, 142, 44);
    case 'o': return rgb(206, 92, 24);
    case 'B': return rgb(56, 96, 206);
    case 'b': return rgb(34, 58, 140);
    case 'N': return rgb(112, 72, 42);
    case 'M': return rgb(176, 112, 78);
    case 'm': return rgb(122, 72, 50);
    case 'R': return rgb(255, 70, 50);
    case 'Z': return rgb(255, 214, 60);
    case 'X': return X;
    case 'Y': return Y;
    default:  return KEY;
    }
}

static spr_t make_spr(const char *const *rows, int h, uint16_t X, uint16_t Y, bool flip)
{
    spr_t s = { 0, h, NULL };
    for (int r = 0; r < h; r++) { int l = (int)strlen(rows[r]); if (l > s.w) s.w = l; }
    s.p = pool_get(s.w * h);
    if (!s.p) { s.w = s.h = 0; return s; }
    for (int r = 0; r < h; r++) {
        int l = (int)strlen(rows[r]);
        for (int c = 0; c < s.w; c++) {
            int sc = flip ? s.w - 1 - c : c;
            s.p[r * s.w + c] = sc < l ? pal(rows[r][sc], X, Y) : KEY;
        }
    }
    return s;
}

static void blit(const spr_t *s, int x, int y)
{
    for (int r = 0; r < s->h; r++) {
        int yy = y + r;
        if ((unsigned)yy >= H) continue;
        const uint16_t *src = &s->p[r * s->w];
        uint16_t *dst = &px[yy * stride];
        for (int c = 0; c < s->w; c++) {
            int xx = x + c;
            if ((unsigned)xx < W && src[c] != KEY) dst[xx] = src[c];
        }
    }
}

/* squashed (h scaled) blit for stomped enemies */
static void blit_squash(const spr_t *s, int x, int y, int newh)
{
    for (int r = 0; r < newh; r++) {
        int sr = r * s->h / newh, yy = y + r;
        if ((unsigned)yy >= H) continue;
        for (int c = 0; c < s->w; c++) {
            uint16_t v = s->p[sr * s->w + c];
            if (v != KEY) pset(x + c, yy, v);
        }
    }
}

static void blit_flipv(const spr_t *s, int x, int y)
{
    for (int r = 0; r < s->h; r++)
        for (int c = 0; c < s->w; c++) {
            uint16_t v = s->p[(s->h - 1 - r) * s->w + c];
            if (v != KEY) pset(x + c, y + r, v);
        }
}

/* The hero: Abed the inventor — cap, round glasses, orange shirt, jeans. Facing right. */
static const char *HERO_TOP[13] = {
    "...KKKKKK...",
    "..KXXXXXXK..",
    ".KXXXXXXXXK.",
    ".KYYYYYYYYYK",
    ".KHSSGWSGWK.",
    ".KHSSGGSGGK.",
    ".KHSSSSSSSK.",
    "..KHSSSsSK..",
    "..KOOOOOOK..",
    ".KOOOOOOOOK.",
    "KSOoOOOOoOSK",
    ".KOOOOOOOOK.",
    "..KBBBBBBK..",
};
static const char *HERO_LEGS[4][3] = {
    { "..KBBKKBBK..", "..KBK..KBK..", ".KNNK..KNNK." },   /* stand  */
    { ".KBBK..KBBK.", ".KBK....KBK.", "KNNK....KNNK" },   /* walk a */
    { "...KBBBBK...", "...KBKKBK...", "..KNNKKNNK.." },   /* walk b */
    { ".KBBKKBBBK..", "KBBK...KBBK.", "KNNK....KNNK" },   /* jump   */
};

/* Rust Bug: a rusty little beetle robot */
static const char *BEETLE[2][16] = {
    { "", "......K..K......", ".....K....K.....", "....KKKKKKKK....", "...KMMMMMMMMK...",
      "..KMMmMMMMmMMK..", "..KMRRMMMMRRMK..", ".KMMRRMMMMRRMMK.", ".KMMMMMMMMMMMMK.", ".KmmmmmmmmmmmmK.",
      "KMMMMMMMMMMMMMMK", "KmMMMMMMMMMMMMmK", ".KKKKKKKKKKKKKK.", "..KK..KK..KK....", ".KK..KK..KK.....", "" },
    { "", "......K..K......", ".....K....K.....", "....KKKKKKKK....", "...KMMMMMMMMK...",
      "..KMMmMMMMmMMK..", "..KMRRMMMMRRMK..", ".KMMRRMMMMRRMMK.", ".KMMMMMMMMMMMMK.", ".KmmmmmmmmmmmmK.",
      "KMMMMMMMMMMMMMMK", "KmMMMMMMMMMMMMmK", ".KKKKKKKKKKKKKK.", "....KK..KK..KK..", ".....KK..KK..KK.", "" },
};

/* Buzz drone: a flying rust bug with a propeller (propeller drawn in code) */
static const char *DRONE[10] = {
    "", "......KK........", "..KKKKKKKKKK....", ".KXXXXXXXXXXK...", "KXXWWXXXXWWXXK..",
    "KXXWKXXXXWKXXK..", "KXXXXXXXXXXXXK..", ".KYYYYYYYYYYK...", "..KKKKKKKKKK....", "...K......K.....",
};

/* Power-up mushroom: X = cap, Y = stripes */
static const char *SHROOM[14] = {
    "", "", ".....KKKKKK.....", "...KKXXXXXXKK...", "..KXYYXXXXYYXK..", ".KXXXXYYYYXXXXK.",
    ".KXYXXXXXXXXYXK.", "KXXYYXXXXXXYYXXK", "KKKKKKKKKKKKKKKK", "...KWWWWWWWWK...",
    "...KWwWWWWwWK...", "...KWWWWWWWWK...", "...KwWWWWWWwK...", "....KKKKKKKK....",
};

static spr_t hero_spr[2][2][4][2];          /* [power set][big][frame][dir] */
static spr_t beetle_spr[2][2], drone_spr[2];
static spr_t shroom_spr[3];
static spr_t gear_spr[8];

static void make_gear_frames(void)
{
    for (int f = 0; f < 8; f++) {
        spr_t s = { 12, 12, pool_get(144) };
        float sx = fabsf(cosf(f * 3.14159f / 8.0f));
        if (sx < 0.18f) sx = 0.18f;
        for (int y = 0; y < 12; y++)
            for (int x = 0; x < 12; x++) {
                float dx = (x + 0.5f - 6) / sx, dy = y + 0.5f - 6;
                float d = sqrtf(dx * dx + dy * dy), a = atan2f(dy, dx);
                float rim = 4.2f + (cosf(a * 8) > 0.2f ? 1.6f : 0);
                uint16_t c = KEY;
                if (d < rim) c = d < 1.6f ? rgb(120, 80, 10) : (dy < -1 ? rgb(255, 236, 120) : rgb(244, 190, 40));
                if (d >= rim - 0.8f && d < rim) c = rgb(170, 110, 20);
                s.p[y * 12 + x] = c;
            }
        gear_spr[f] = s;
    }
}

static void make_sprites(void)
{
    uint16_t cx[2] = { rgb(45, 224, 194), rgb(176, 96, 255) };
    uint16_t cy[2] = { rgb(18, 140, 120), rgb(110, 50, 190) };
    for (int set = 0; set < 2; set++)
        for (int f = 0; f < 4; f++) {
            const char *r16[16], *r24[24];
            for (int r = 0; r < 13; r++) r16[r] = HERO_TOP[r];
            for (int r = 0; r < 3; r++) r16[13 + r] = HERO_LEGS[f][r];
            for (int r = 0; r < 24; r++) r24[r] = r < 8 ? r16[r] : r < 18 ? r16[8 + (r - 8) / 2] : r16[13 + (r - 18) / 2];
            for (int d = 0; d < 2; d++) {
                hero_spr[set][0][f][d] = make_spr(r16, 16, cx[set], cy[set], d);
                hero_spr[set][1][f][d] = make_spr(r24, 24, cx[set], cy[set], d);
            }
        }
    for (int f = 0; f < 2; f++)
        for (int d = 0; d < 2; d++) beetle_spr[f][d] = make_spr(BEETLE[f], 16, 0, 0, d);
    for (int d = 0; d < 2; d++) drone_spr[d] = make_spr(DRONE, 10, rgb(214, 104, 48), rgb(140, 60, 30), d);
    shroom_spr[0] = make_spr(SHROOM, 14, rgb(40, 200, 170), rgb(255, 230, 90), false);    /* grow   */
    shroom_spr[1] = make_spr(SHROOM, 14, rgb(150, 80, 240), rgb(120, 230, 255), false);   /* double */
    shroom_spr[2] = make_spr(SHROOM, 14, rgb(250, 190, 30), rgb(255, 255, 255), false);   /* life   */
    make_gear_frames();
}

/* ══ worlds & tiles ═══════════════════════════════════════════════════════ */
typedef struct {
    const char *name;
    uint8_t sky0[3], sky1[3], mount[3], hill[3], grass[3], dirt[3];
    bool cave;
} theme_t;

static const theme_t THEMES[3] = {
    { "GREEN VALLEY",   { 84, 160, 250 }, { 186, 228, 255 }, { 126, 150, 206 }, { 74, 170, 96 },  { 86, 200, 72 },  { 150, 100, 62 }, false },
    { "CRYSTAL CAVES",  { 16, 12, 40 },   { 58, 30, 92 },    { 56, 40, 104 },   { 40, 28, 78 },   { 150, 90, 230 }, { 72, 60, 96 },   true },
    { "SUNSET FACTORY", { 255, 118, 64 }, { 255, 212, 126 }, { 150, 72, 92 },   { 92, 52, 72 },   { 232, 172, 64 }, { 112, 72, 62 },  false },
};

enum { TI_GTOP, TI_GROUND, TI_STONE, TI_CRATE, TI_USED, TI_METAL, TI_PLANK, TI_SPIKE, TI_N };
static uint16_t tiles[TI_N][T * T];
static uint16_t sky_row[H];
static const theme_t *th;

static uint16_t c3(const uint8_t *c, int d) { return rgb(c[0] + d, c[1] + d, c[2] + d); }

static void make_tiles(int world)
{
    th = &THEMES[world];
    for (int y = 0; y < H; y++) {
        float t = (float)y / H;
        sky_row[y] = rgb(th->sky0[0] + (int)((th->sky1[0] - th->sky0[0]) * t),
                         th->sky0[1] + (int)((th->sky1[1] - th->sky0[1]) * t),
                         th->sky0[2] + (int)((th->sky1[2] - th->sky0[2]) * t));
    }
    for (int y = 0; y < T; y++)
        for (int x = 0; x < T; x++) {
            uint32_t h = hash2(x * 7 + world, y * 13);
            int n = (int)(h % 21) - 10;
            /* dirt */
            uint16_t dirt = c3(th->dirt, n);
            if (h % 17 == 0) dirt = c3(th->dirt, -35);
            if (h % 23 == 0) dirt = c3(th->dirt, 30);
            tiles[TI_GROUND][y * T + x] = dirt;
            /* grass on top */
            int blade = 4 + (int)(hash2(x, 99 + world) % 3);
            uint16_t g = y < blade ? c3(th->grass, y == 0 ? 40 : n / 2) : y == blade ? c3(th->grass, -60) : dirt;
            tiles[TI_GTOP][y * T + x] = g;
            /* stone bricks */
            bool mortar = (y % 8 == 7) || ((x + (y / 8) * 8) % 16 == 15);
            tiles[TI_STONE][y * T + x] = mortar ? rgb(70, 66, 80) : rgb(150 + n, 146 + n, 160 + n);
            if (y % 8 == 0 && !mortar) tiles[TI_STONE][y * T + x] = rgb(190, 186, 200);
            /* metal plate with rivets */
            bool edge = x == 0 || y == 0 || x == 15 || y == 15;
            bool rivet = (x == 3 || x == 12) && (y == 3 || y == 12);
            uint16_t m = rgb(120 + y * 3, 130 + y * 3, 150 + y * 2);
            tiles[TI_METAL][y * T + x] = edge ? rgb(50, 56, 72) : rivet ? rgb(230, 236, 245) : m;
            /* crate with a gear mark */
            bool frame = x <= 1 || y <= 1 || x >= 14 || y >= 14;
            float dx = x - 7.5f, dy = y - 7.5f, d = sqrtf(dx * dx + dy * dy), a = atan2f(dy, dx);
            bool gear = d < 4.5f + (cosf(a * 6) > 0.3f ? 1.3f : 0) && d > 1.6f;
            uint16_t wood = rgb(196 + n, 130 + n, 64 + n / 2);
            if ((y % 4) == 0) wood = rgb(170, 108, 50);
            tiles[TI_CRATE][y * T + x] = frame ? rgb(120, 70, 30) : gear ? rgb(255, 214, 60) : wood;
            tiles[TI_USED][y * T + x] = frame ? rgb(70, 54, 46) : rgb(118 + n / 2, 96 + n / 2, 84 + n / 2);
            /* plank (one-way) */
            uint16_t pl = KEY;
            if (y < 5) pl = y == 0 ? rgb(236, 180, 110) : y == 4 ? rgb(110, 64, 30) : rgb(196, 134, 70);
            if (y >= 5 && y < 9 && (x == 2 || x == 13)) pl = rgb(110, 64, 30);
            tiles[TI_PLANK][y * T + x] = pl;
            /* spikes */
            int sx = x % 8;
            int top = 16 - (sx < 4 ? sx * 2 + 2 : (7 - sx) * 2 + 2) - 4;
            uint16_t sp = KEY;
            if (y >= top + 4) sp = sx < 4 ? rgb(230, 234, 244) : rgb(150, 156, 176);
            if (y >= 14) sp = rgb(90, 90, 110);
            tiles[TI_SPIKE][y * T + x] = sp;
        }
}

/* ── the map ── */
static char map[ROWS][MAXW + 1];
static int LW;

static inline char tile(int tx, int ty)
{
    if (tx < 0 || tx >= LW) return 'B';
    if (ty < 0 || ty >= ROWS) return ' ';
    return map[ty][tx];
}

static inline bool is_solid(char c)
{
    return c == '#' || c == 'S' || c == 'C' || c == 'M' || c == 'J' || c == 'L' || c == 'B' || c == 'U';
}

/* builder */
static void lv_begin(int w) { LW = w; for (int r = 0; r < ROWS; r++) { memset(map[r], ' ', MAXW); map[r][MAXW] = 0; } }
static void ground(int x0, int x1, int top) { for (int x = x0; x <= x1 && x < LW; x++) for (int y = top; y < ROWS; y++) map[y][x] = '#'; }
static void put(int x, int y, const char *s) { for (int i = 0; s[i]; i++) if (s[i] != ' ' && x + i < LW) map[y][x + i] = s[i]; }
static void column(int x, int top, int bottom) { for (int y = top; y <= bottom; y++) map[y][x] = 'B'; }
static void stairs_up(int x, int n) { for (int i = 0; i < n; i++) column(x + i, 12 - i, 12); }

static void build_world(int w)
{
    if (w == 0) {                       /* GREEN VALLEY */
        lv_begin(190);
        ground(0, 33, 13); ground(37, 73, 13); ground(79, 140, 13); ground(144, 189, 13);
        put(2, 12, "P");
        put(9, 9, "CSMSC"); put(11, 5, "C");
        put(18, 12, "e"); put(20, 11, "oooo");
        stairs_up(26, 3); put(26, 8, "ooo");
        put(40, 10, "====="); put(40, 9, "ooooo");
        put(46, 12, "e"); put(51, 12, "e");
        column(55, 11, 12); put(58, 12, "e"); column(62, 10, 12);
        put(67, 9, "SJS");
        put(75, 10, "==="); put(75, 9, "ooo");
        put(84, 12, "k");
        put(90, 9, "SSSLSSSS"); put(92, 5, "oooo");
        put(88, 12, "e"); put(96, 12, "e"); put(100, 7, "f");
        stairs_up(105, 5); put(112, 12, "e");
        put(120, 12, "e"); put(124, 12, "e"); put(128, 12, "e");
        put(122, 9, "CMC");
        put(132, 12, "^^^"); put(132, 9, "ooo");
        put(146, 10, "===="); put(146, 9, "oooo"); put(150, 7, "f");
        stairs_up(158, 8); column(166, 5, 12); column(167, 5, 12);
        put(175, 11, "X");
    } else if (w == 1) {                /* CRYSTAL CAVES */
        lv_begin(200);
        for (int x = 8; x < 192; x++) if (x % 37 < 30) map[2][x] = 'B';
        ground(0, 20, 13); ground(24, 45, 13); ground(50, 80, 13); ground(85, 120, 13);
        ground(125, 160, 13); ground(164, 199, 13);
        put(2, 12, "P");
        put(12, 9, "SMS"); put(16, 12, "e");
        put(28, 12, "e"); put(30, 9, "C C C"); put(36, 12, "e");
        put(42, 7, "f"); put(46, 10, "====");
        put(55, 12, "e"); put(56, 9, "===="); put(56, 8, "oooo");
        put(60, 12, "^^"); put(65, 12, "e"); put(70, 12, "^^^"); put(70, 9, "ooo"); put(75, 12, "e");
        put(82, 10, "=="); put(85, 6, "f");
        put(88, 12, "k");
        stairs_up(92, 3); ground(95, 105, 10); put(99, 9, "e"); put(100, 6, "J"); put(96, 9, "o  o  o");
        put(95, 8, "f");
        put(110, 12, "^^^^"); put(110, 9, "===="); put(110, 8, "oooo");
        put(122, 10, "==");
        put(128, 9, "SSCSS"); put(130, 5, "L");
        put(135, 12, "e"); put(140, 12, "e"); put(145, 12, "e"); put(148, 7, "f");
        column(150, 10, 12); column(155, 9, 12); put(152, 12, "e");
        put(168, 12, "e"); put(166, 9, "CMC");
        stairs_up(170, 6); column(176, 7, 12); column(177, 7, 12);
        put(184, 11, "X");
    } else {                            /* SUNSET FACTORY */
        lv_begin(210);
        ground(0, 25, 13); ground(30, 60, 13); ground(66, 100, 13); ground(105, 150, 13); ground(155, 209, 13);
        put(2, 12, "P");
        put(8, 9, "CMC"); put(14, 12, "e"); put(20, 12, "e");
        put(27, 10, "==");
        column(35, 10, 12); column(40, 9, 12); column(45, 8, 12); put(42, 5, "f"); put(38, 12, "e");
        put(50, 9, "SJS"); put(52, 12, "e"); put(56, 12, "e");
        put(62, 9, "==="); put(62, 8, "ooo");
        put(70, 12, "k");
        put(75, 12, "^^^^^"); put(74, 9, "======="); put(75, 8, "ooooo");
        put(85, 12, "e"); put(88, 6, "f"); put(90, 12, "e"); put(95, 12, "e");
        put(92, 9, "SCSLS");
        put(102, 10, "==");
        stairs_up(108, 4); ground(112, 125, 9); put(118, 8, "e"); put(114, 6, "oooooo"); put(121, 5, "M");
        put(130, 12, "^^"); put(136, 12, "^^"); put(133, 7, "f");
        put(140, 9, "CSC"); put(145, 12, "e");
        put(152, 10, "==");
        put(160, 12, "e"); put(163, 12, "e"); put(166, 12, "e"); put(170, 6, "f"); put(162, 9, "SMS");
        stairs_up(178, 8); column(186, 5, 12); column(187, 5, 12);
        put(195, 11, "X");
    }
}

/* ══ game state ═══════════════════════════════════════════════════════════ */
enum { ST_TITLE, ST_INTRO, ST_PLAY, ST_DYING, ST_CLEAR, ST_OVER, ST_WIN };
static int state, state_t;
static int world, lives, gears, score, best, maxworld, timer_s, timer_f;
static int checkpoint_x = -1;
static float camx;
static int tick;
static bool paused;

typedef struct {
    float x, y, vx, vy;
    bool big, dbl, left, ground, used_dbl;
    int coyote, jbuf, invuln, combo;
    float animt;
} hero_t;
static hero_t hero;
static float hero_h(void) { return hero.big ? 23 : 15; }
#define HERO_W 10

#define NENEMY 48
typedef struct { bool on, active; int type, dead; float x, y, vx, vy, bx, by, t; bool flip; } enemy_t;
static enemy_t en[NENEMY];

#define NITEM 8
typedef struct { bool on; int type, emerge; float x, y, vx, vy; } item_t;
static item_t items[NITEM];

#define NPART 96
typedef struct { float x, y, vx, vy; int life; uint16_t c; int kind; } part_t;
static part_t parts[NPART];

#define NPOP 12
typedef struct { float x, y; int life; char txt[8]; } pop_t;
static pop_t pops[NPOP];

static int bump_tx = -1, bump_ty, bump_t;

/* ══ audio (original tunes, synthesized) ══════════════════════════════════ */
enum { SFX_NONE, SFX_JUMP, SFX_JUMP2, SFX_GEAR, SFX_STOMP, SFX_POWER, SFX_HURT, SFX_BREAK,
       SFX_BUMP, SFX_LIFE, SFX_DIE, SFX_CLEAR, SFX_PAUSE };
static volatile int a_music = -1, a_sfx = 0;
static volatile bool a_run;
static papp_thread_t *a_thread;

static const uint8_t TUNE_MEL[3][32] = {
    { 72, 0, 76, 79, 84, 0, 79, 76, 77, 0, 81, 84, 81, 0, 77, 0, 76, 0, 79, 83, 86, 0, 83, 79, 81, 79, 77, 76, 74, 0, 72, 0 },
    { 69, 0, 0, 72, 76, 0, 74, 0, 72, 0, 71, 0, 69, 0, 0, 0, 65, 0, 0, 69, 72, 0, 71, 0, 69, 0, 68, 0, 69, 0, 0, 0 },
    { 74, 74, 0, 77, 0, 79, 0, 81, 79, 0, 77, 0, 74, 0, 72, 0, 74, 74, 0, 77, 0, 79, 81, 84, 83, 81, 79, 77, 76, 0, 74, 0 },
};
static const uint8_t TUNE_BASS[3][32] = {
    { 48, 0, 55, 0, 48, 0, 55, 0, 53, 0, 60, 0, 53, 0, 60, 0, 52, 0, 59, 0, 55, 0, 62, 0, 53, 0, 55, 0, 48, 0, 43, 0 },
    { 45, 0, 52, 0, 45, 0, 52, 0, 43, 0, 50, 0, 43, 0, 50, 0, 41, 0, 48, 0, 41, 0, 48, 0, 40, 0, 47, 0, 45, 0, 40, 0 },
    { 38, 50, 38, 50, 38, 50, 38, 50, 36, 48, 36, 48, 36, 48, 36, 48, 41, 53, 41, 53, 43, 55, 43, 55, 45, 57, 45, 57, 38, 50, 38, 50 },
};
static const float TUNE_STEP[3] = { 0.17f, 0.22f, 0.15f };

static float mtof(int m) { return 440.0f * powf(2.0f, (m - 69) / 12.0f); }

static void sfx(int id) { a_sfx = id; }

static int audio_thread(void *arg)
{
    (void)arg;
    const int R = 22050;
    papp_audio_cfg_t ac = { .rate = R, .channels = 1, .format = PAPP_AUDIO_S16, .latency_ms = 70 };
    papp_audio_stream_t *as = papp_audio_open(&ac);
    if (!as) return 0;
    int16_t buf[256];
    float ph_m = 0, ph_b = 0, ph_s = 0, f_m = 0, f_b = 0, env_m = 0, env_b = 0;
    int cur = -2, step = 0, spos = 1 << 30, sps = R / 6;
    int sid = 0; float st = 0;
    uint32_t noise = 0x1234;
    while (a_run) {
        int want = a_music;
        if (want != cur) { cur = want; step = -1; spos = 1 << 30; env_m = env_b = 0; if (cur >= 0) sps = (int)(R * TUNE_STEP[cur]); }
        int req = a_sfx;
        if (req) { a_sfx = 0; sid = req; st = 0; }
        for (int i = 0; i < 256; i++) {
            float s = 0;
            if (cur >= 0) {
                if (++spos >= sps) {
                    spos = 0; step = (step + 1) & 31;
                    int m = TUNE_MEL[cur][step], b = TUNE_BASS[cur][step];
                    if (m) { f_m = mtof(m); env_m = 1; }
                    if (b) { f_b = mtof(b); env_b = 1; }
                }
                ph_m += f_m / R; if (ph_m >= 1) ph_m -= 1;
                ph_b += f_b / R; if (ph_b >= 1) ph_b -= 1;
                s += (ph_m < 0.25f ? 1.0f : -1.0f) * 0.075f * env_m;
                s += (4.0f * fabsf(ph_b - 0.5f) - 1.0f) * 0.16f * env_b;
                env_m *= 0.99975f; if (env_m < 0.22f && env_m > 0) env_m *= 0.9999f;
                env_b *= 0.99985f;
            }
            if (sid) {
                float f = 0, dur = 0.2f, vol = 0.16f; int wave = 0;   /* 0 square, 1 noise */
                static const uint8_t pw[] = { 60, 64, 67, 72, 76, 79, 84 }, lf[] = { 72, 76, 79, 84, 88 };
                static const uint8_t cl[] = { 60, 64, 67, 72, 67, 72, 76, 79, 84 }, dd[] = { 67, 66, 65, 64, 62, 60, 57, 55 };
                switch (sid) {
                case SFX_JUMP:  dur = 0.15f; f = 320 + 640 * st / dur; break;
                case SFX_JUMP2: dur = 0.17f; f = 520 + 900 * st / dur; break;
                case SFX_GEAR:  dur = 0.15f; f = st < 0.05f ? 1568 : 2093; vol = 0.11f; break;
                case SFX_STOMP: dur = 0.13f; f = 320 - 220 * st / dur; wave = 2; break;
                case SFX_POWER: dur = 0.49f; f = mtof(pw[(int)(st / 0.07f) % 7]); break;
                case SFX_HURT:  dur = 0.35f; f = 620 - 470 * st / dur; break;
                case SFX_BREAK: dur = 0.22f; wave = 1; vol = 0.2f; break;
                case SFX_BUMP:  dur = 0.07f; f = 130; break;
                case SFX_LIFE:  dur = 0.5f; f = mtof(lf[(int)(st / 0.1f) % 5]); break;
                case SFX_DIE:   dur = 1.2f; f = mtof(dd[(int)(st / 0.15f) % 8]); break;
                case SFX_CLEAR: dur = 1.35f; f = mtof(cl[(int)(st / 0.15f) % 9]); break;
                case SFX_PAUSE: dur = 0.12f; f = st < 0.06f ? 988 : 1319; break;
                }
                float e = 1.0f - st / dur;
                if (e <= 0) sid = 0;
                else {
                    ph_s += f / R; if (ph_s >= 1) ph_s -= 1;
                    noise ^= noise << 13; noise ^= noise >> 17; noise ^= noise << 5;
                    float w = wave == 1 ? ((noise & 0xFFFF) / 32768.0f - 1.0f)
                            : wave == 2 ? ((ph_s < 0.5f ? 1.0f : -1.0f) * 0.6f + ((noise & 0xFF) / 128.0f - 1.0f) * 0.4f)
                            : (ph_s < 0.5f ? 1.0f : -1.0f);
                    s += w * vol * sqrtf(e);
                }
                st += 1.0f / R;
            }
            if (s > 0.95f) s = 0.95f;
            if (s < -0.95f) s = -0.95f;
            buf[i] = (int16_t)(s * 32767);
        }
        papp_audio_write(as, buf, 256, 200);
    }
    papp_audio_close(as);
    return 0;
}

/* ══ save ═════════════════════════════════════════════════════════════════ */
#define SAVE_DIR  "/sd/data/abed.adventure"
#define SAVE_FILE SAVE_DIR "/save.txt"

static int parse_int(const char **p)
{
    int v = 0;
    while (**p && (**p < '0' || **p > '9')) (*p)++;
    while (**p >= '0' && **p <= '9') { v = v * 10 + (**p - '0'); (*p)++; }
    return v;
}

static void load_save(void)
{
    best = 0; maxworld = 0;
    FILE *f = fopen(SAVE_FILE, "rb");
    if (!f) return;
    char buf[64] = { 0 };
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char *p = buf;
    best = parse_int(&p);
    maxworld = parse_int(&p);
    if (maxworld < 0 || maxworld > 2) maxworld = 0;
}

static void save_game(void)
{
    if (score > best) best = score;
    mkdir("/sd/data", 0777);
    mkdir(SAVE_DIR, 0777);
    FILE *f = fopen(SAVE_FILE, "wb");
    if (!f) return;
    char buf[40];
    sfmt(buf, "", best, 0, " ");
    char b2[12];
    sfmt(b2, "", maxworld, 0, "\n");
    strcat(buf, b2);
    fwrite(buf, 1, strlen(buf), f);
    fclose(f);
}

/* ══ effects ══════════════════════════════════════════════════════════════ */
static void spawn_part(float x, float y, float vx, float vy, int life, uint16_t c, int kind)
{
    for (int i = 0; i < NPART; i++)
        if (parts[i].life <= 0) { parts[i] = (part_t){ x, y, vx, vy, life, c, kind }; return; }
}

static void burst(float x, float y, int n, uint16_t c)
{
    for (int i = 0; i < n; i++) {
        float a = frange(0, 6.283f), sp = frange(0.6f, 2.4f);
        spawn_part(x, y, cosf(a) * sp, sinf(a) * sp - 1, (int)frange(18, 34), c, 0);
    }
}

static void popup(float x, float y, const char *txt)
{
    for (int i = 0; i < NPOP; i++)
        if (pops[i].life <= 0) {
            pops[i].x = x; pops[i].y = y; pops[i].life = 50;
            strncpy(pops[i].txt, txt, 7); pops[i].txt[7] = 0;
            return;
        }
}

static void add_score(int n, float x, float y)
{
    score += n;
    char b[8];
    sfmt(b, "", n, 0, NULL);
    popup(x, y, b);
}

static void add_gear(float x, float y)
{
    gears++;
    score += 100;
    sfx(SFX_GEAR);
    spawn_part(x, y, 0, -2.2f, 24, 0, 1);   /* kind 1 = a flying gear */
    if (gears >= 50) { gears -= 50; lives++; sfx(SFX_LIFE); popup(x, y - 10, "1UP"); }
}

/* ══ level start ══════════════════════════════════════════════════════════ */
static void start_level(void)
{
    build_world(world);
    make_tiles(world);
    memset(en, 0, sizeof en);
    memset(items, 0, sizeof items);
    memset(parts, 0, sizeof parts);
    memset(pops, 0, sizeof pops);
    bump_tx = -1;
    int ne = 0;
    float sx = 40, sy = 160;
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < LW; x++) {
            char c = map[y][x];
            if (c == 'P') { sx = x * T + 3; sy = y * T; map[y][x] = ' '; }
            else if ((c == 'e' || c == 'f') && ne < NENEMY) {
                enemy_t *e = &en[ne++];
                e->on = true; e->type = c == 'f';
                e->x = e->bx = x * T + 1; e->y = e->by = y * T + (c == 'e' ? 4 : 2);
                e->vx = c == 'e' ? -0.55f : 0.8f;
                e->t = frange(0, 6.28f);
                map[y][x] = ' ';
            }
        }
    memset(&hero, 0, sizeof hero);
    hero.x = checkpoint_x >= 0 ? checkpoint_x * T + 3 : sx;
    hero.y = (checkpoint_x >= 0 ? 12 * T : sy + T) - hero_h() - 0.01f;
    camx = hero.x - W * 0.4f;
    if (camx < 0) camx = 0;
    if (camx > LW * T - W) camx = LW * T - W;
    timer_s = 300; timer_f = 0;
    a_music = world;
}

static void new_game(int w)
{
    world = w; lives = 3; gears = 0; score = 0; checkpoint_x = -1;
    state = ST_INTRO; state_t = 0;
    a_music = -1;
}

/* ══ physics ══════════════════════════════════════════════════════════════ */
static bool box_solid(float x, float y, float w, float h)
{
    int x0 = (int)floorf(x / T), x1 = (int)floorf((x + w - 0.01f) / T);
    int y0 = (int)floorf(y / T), y1 = (int)floorf((y + h - 0.01f) / T);
    for (int ty = y0; ty <= y1; ty++)
        for (int tx = x0; tx <= x1; tx++)
            if (is_solid(tile(tx, ty))) return true;
    return false;
}

static int head_tx, head_ty;
enum { M_LAND = 1, M_WALL = 2, M_HEAD = 4 };

static int move_box(float *x, float *y, float *vx, float *vy, float w, float h)
{
    int fl = 0;
    *x += *vx;
    if (box_solid(*x, *y, w, h)) {
        if (*vx > 0) *x = floorf((*x + w - 0.01f) / T) * T - w;
        else if (*vx < 0) *x = (floorf(*x / T) + 1) * T;
        fl |= M_WALL;
    }
    float prev_bottom = *y + h;
    *y += *vy;
    if (*vy > 0) {
        int ty = (int)floorf((*y + h - 0.01f) / T);
        bool land = box_solid(*x, *y, w, h);
        if (!land && prev_bottom <= ty * T + 0.5f) {
            int x0 = (int)floorf(*x / T), x1 = (int)floorf((*x + w - 0.01f) / T);
            for (int tx = x0; tx <= x1; tx++) if (tile(tx, ty) == '=') land = true;
        }
        if (land) { *y = ty * T - h; *vy = 0; fl |= M_LAND; }
    } else if (*vy < 0 && box_solid(*x, *y, w, h)) {
        int ty = (int)floorf(*y / T);
        *y = (ty + 1) * T;
        *vy = 0.5f;
        head_ty = ty;
        float cx = *x + w / 2;
        head_tx = (int)floorf(cx / T);
        if (!is_solid(tile(head_tx, ty))) {
            int a = (int)floorf(*x / T), b = (int)floorf((*x + w - 0.01f) / T);
            head_tx = is_solid(tile(a, ty)) ? a : b;
        }
        fl |= M_HEAD;
    }
    return fl;
}

static void spawn_item(int type, int tx, int ty)
{
    for (int i = 0; i < NITEM; i++)
        if (!items[i].on) {
            items[i] = (item_t){ true, type, 32, tx * T + 1, ty * T, 0, 0 };
            return;
        }
}

static void hit_block(int tx, int ty)
{
    char c = tile(tx, ty);
    bump_tx = tx; bump_ty = ty; bump_t = 10;
    if (c == 'C') { map[ty][tx] = 'U'; add_gear(tx * T + 8, ty * T - 8); }
    else if (c == 'M' || c == 'J' || c == 'L') {
        map[ty][tx] = 'U';
        spawn_item(c == 'M' ? 0 : c == 'J' ? 1 : 2, tx, ty);
        sfx(SFX_BUMP);
    } else if (c == 'S' && hero.big) {
        map[ty][tx] = ' ';
        bump_tx = -1;
        for (int k = 0; k < 6; k++)
            spawn_part(tx * T + 8, ty * T + 8, frange(-2, 2), frange(-4.5f, -2), 50, rgb(150, 146, 160), 2);
        score += 50;
        sfx(SFX_BREAK);
    } else sfx(SFX_BUMP);
    /* knock enemies standing on the bumped block */
    for (int i = 0; i < NENEMY; i++) {
        enemy_t *e = &en[i];
        if (!e->on || e->dead) continue;
        if (e->x + 14 > tx * T && e->x < tx * T + T && fabsf(e->y + 12 - ty * T) < 4) {
            e->dead = 2; e->vy = -4; e->vx = 1; add_score(200, e->x, e->y);
        }
    }
}

static void hurt_hero(void)
{
    if (hero.invuln > 0 || state != ST_PLAY) return;
    if (hero.dbl) { hero.dbl = false; hero.invuln = 100; sfx(SFX_HURT); return; }
    if (hero.big) { hero.big = false; hero.y += 8; hero.invuln = 100; sfx(SFX_HURT); return; }
    state = ST_DYING; state_t = 0;
    hero.vy = -6; hero.vx = 0;
    a_music = -1;
    sfx(SFX_DIE);
}

/* ══ update ═══════════════════════════════════════════════════════════════ */
static uint32_t btn, pressed;

static void update_hero(void)
{
    bool L = btn & PAPP_BTN_LEFT, Rr = btn & PAPP_BTN_RIGHT;
    bool run = btn & (PAPP_BTN_B | PAPP_BTN_Y);
    bool jump_held = btn & PAPP_BTN_A, jump_pr = pressed & PAPP_BTN_A;
    float top = run ? 2.7f : 1.7f, acc = hero.ground ? 0.15f : 0.1f;
    if (L && !Rr) {
        hero.vx -= hero.vx > 0 ? 0.3f : acc;
        if (hero.vx < -top) hero.vx += 0.12f;
        if (hero.vx < -top - 0.2f) hero.vx = -top;
        hero.left = true;
    } else if (Rr && !L) {
        hero.vx += hero.vx < 0 ? 0.3f : acc;
        if (hero.vx > top) hero.vx -= 0.12f;
        if (hero.vx > top + 0.2f) hero.vx = top;
        hero.left = false;
    } else {
        hero.vx *= hero.ground ? 0.8f : 0.97f;
        if (fabsf(hero.vx) < 0.05f) hero.vx = 0;
    }
    if (jump_pr) hero.jbuf = 7; else if (hero.jbuf > 0) hero.jbuf--;
    if (hero.jbuf > 0 && (hero.ground || hero.coyote > 0)) {
        hero.vy = -(5.6f + fabsf(hero.vx) * 0.25f);
        hero.jbuf = hero.coyote = 0; hero.ground = false; hero.used_dbl = false;
        sfx(SFX_JUMP);
    } else if (jump_pr && !hero.ground && hero.coyote <= 0 && hero.dbl && !hero.used_dbl) {
        hero.vy = -5.2f; hero.used_dbl = true; hero.jbuf = 0;
        for (int k = 0; k < 10; k++) spawn_part(hero.x + 5, hero.y + hero_h(), frange(-1.5f, 1.5f), frange(0, 1), 20, rgb(200, 150, 255), 0);
        sfx(SFX_JUMP2);
    }
    hero.vy += (hero.vy < 0 && jump_held) ? 0.24f : 0.5f;
    if (hero.vy > 7) hero.vy = 7;

    float h = hero_h();
    int fl = move_box(&hero.x, &hero.y, &hero.vx, &hero.vy, HERO_W, h);
    if (fl & M_WALL) hero.vx = 0;
    if (fl & M_LAND) { hero.ground = true; hero.coyote = 6; hero.combo = 0; }
    else { if (hero.ground) hero.coyote = 6; hero.ground = false; if (hero.coyote > 0) hero.coyote--; }
    if (fl & M_HEAD) hit_block(head_tx, head_ty);
    if (hero.x < camx) { hero.x = camx; if (hero.vx < 0) hero.vx = 0; }
    if (hero.invuln > 0) hero.invuln--;
    hero.animt += fabsf(hero.vx) * 0.12f;

    /* tiles the hero touches: gears, spikes, checkpoint, portal */
    int x0 = (int)floorf(hero.x / T), x1 = (int)floorf((hero.x + HERO_W - 0.01f) / T);
    int y0 = (int)floorf(hero.y / T), y1 = (int)floorf((hero.y + h - 0.01f) / T);
    for (int ty = y0; ty <= y1; ty++)
        for (int tx = x0; tx <= x1; tx++) {
            char c = tile(tx, ty);
            if (c == 'o') { map[ty][tx] = ' '; add_gear(tx * T + 8, ty * T + 8); }
            else if (c == '^' && hero.y + h > ty * T + 7) { hurt_hero(); if (state == ST_PLAY) hero.vy = -4; }
            else if (c == 'k' && checkpoint_x != tx) { checkpoint_x = tx; sfx(SFX_PAUSE); burst(tx * T + 8, ty * T - 10, 14, rgb(90, 255, 140)); }
        }
    for (int ty = 0; ty < ROWS; ty++)
        for (int tx = x0 - 1; tx <= x1; tx++)
            if (tile(tx, ty) == 'X' && hero.x + HERO_W > tx * T + 4 && hero.x < tx * T + 12 && hero.y + h > ty * T) {
                state = ST_CLEAR; state_t = 0; a_music = -1; sfx(SFX_CLEAR);
                hero.vx = hero.vy = 0;
                return;
            }
    if (hero.y > H + 20) {   /* fell into a pit */
        hero.dbl = false; hero.big = false; hero.invuln = 0;
        hurt_hero();
    }
}

static void update_enemies(void)
{
    float h = hero_h();
    for (int i = 0; i < NENEMY; i++) {
        enemy_t *e = &en[i];
        if (!e->on) continue;
        if (!e->active) { if (e->x < camx + W + 24) e->active = true; else continue; }
        e->t += 0.05f;
        if (e->dead == 1) { if (++e->vy > 30) e->on = false; continue; }   /* squashed: vy is the timer */
        if (e->dead == 2) { e->vy += 0.4f; e->x += e->vx; e->y += e->vy; if (e->y > H + 20) e->on = false; continue; }
        float ew = 14, ehgt = e->type ? 10 : 12;
        if (e->type == 0) {
            e->vy += 0.4f; if (e->vy > 6) e->vy = 6;
            float vx = e->vx;
            int fl = move_box(&e->x, &e->y, &e->vx, &e->vy, ew, ehgt);
            if (fl & M_WALL) e->vx = -vx;
            if (fl & M_LAND) {
                float fx = e->vx > 0 ? e->x + ew + 1 : e->x - 1;
                char below = tile((int)floorf(fx / T), (int)floorf((e->y + ehgt + 2) / T));
                if (!is_solid(below) && below != '=') e->vx = -e->vx;
            }
            if (e->y > H + 20) { e->on = false; continue; }
        } else {
            e->x += e->vx;
            if (e->x > e->bx + 48 || e->x < e->bx - 48 || box_solid(e->x, e->y, ew, ehgt)) e->vx = -e->vx;
            e->y = e->by + sinf(e->t * 1.6f) * 14;
        }
        e->flip = e->vx > 0;
        if (e->x < camx - 64) { e->on = false; continue; }
        /* hero contact */
        if (state == ST_PLAY && hero.x + HERO_W > e->x + 1 && hero.x < e->x + ew - 1 && hero.y + h > e->y + 2 && hero.y < e->y + ehgt) {
            if (hero.vy > 0.5f && hero.y + h - hero.vy <= e->y + 7) {
                e->dead = 1; e->vy = 0;
                hero.vy = (btn & PAPP_BTN_A) ? -6.0f : -4.2f;
                int pts = 100 << (hero.combo < 4 ? hero.combo : 4);
                hero.combo++;
                add_score(pts, e->x, e->y - 6);
                burst(e->x + 7, e->y + 6, 8, rgb(255, 230, 120));
                sfx(SFX_STOMP);
            } else hurt_hero();
        }
    }
}

static void update_items(void)
{
    float h = hero_h();
    for (int i = 0; i < NITEM; i++) {
        item_t *it = &items[i];
        if (!it->on) continue;
        if (it->emerge > 0) {
            it->y -= 0.5f;
            if (--it->emerge == 0) it->vx = it->type == 2 ? 1.25f : 0.9f;
            continue;
        }
        it->vy += 0.35f; if (it->vy > 6) it->vy = 6;
        float vx = it->vx;
        int fl = move_box(&it->x, &it->y, &it->vx, &it->vy, 14, 14);
        if (fl & M_WALL) it->vx = -vx;
        if (it->y > H + 20) { it->on = false; continue; }
        if (hero.x + HERO_W > it->x && hero.x < it->x + 14 && hero.y + h > it->y && hero.y < it->y + 14) {
            it->on = false;
            if (it->type == 0) {
                if (!hero.big) { hero.big = true; hero.y -= 8; if (box_solid(hero.x, hero.y, HERO_W, 23)) hero.y += 8; }
                add_score(1000, it->x, it->y); sfx(SFX_POWER);
            } else if (it->type == 1) {
                hero.dbl = true; add_score(1000, it->x, it->y); sfx(SFX_POWER);
            } else {
                lives++; popup(it->x, it->y, "1UP"); sfx(SFX_LIFE);
            }
            burst(it->x + 7, it->y + 7, 14, rgb(255, 255, 200));
        }
    }
}

static void update_fx(void)
{
    for (int i = 0; i < NPART; i++) {
        part_t *p = &parts[i];
        if (p->life <= 0) continue;
        p->x += p->vx; p->y += p->vy;
        p->vy += p->kind == 1 ? 0.12f : 0.18f;
        p->life--;
    }
    for (int i = 0; i < NPOP; i++) if (pops[i].life > 0) { pops[i].life--; pops[i].y -= 0.6f; }
    if (bump_t > 0) bump_t--;
}

static void update_camera(void)
{
    float target = hero.x - W * 0.4f + (hero.left ? -20 : 20);
    camx += (target - camx) * 0.12f;
    if (camx < 0) camx = 0;
    if (camx > LW * T - W) camx = LW * T - W;
}

static void update(void)
{
    tick++;
    switch (state) {
    case ST_TITLE:
        camx += 0.6f;
        if (pressed & (PAPP_BTN_LEFT)) { if (world > 0) world--; }
        if (pressed & (PAPP_BTN_RIGHT)) { if (world < maxworld) world++; }
        if (pressed & (PAPP_BTN_A | PAPP_BTN_START)) new_game(world);
        break;
    case ST_INTRO:
        if (state_t == 0) start_level();
        if (++state_t > 130) { state = ST_PLAY; state_t = 0; a_music = world; }
        break;
    case ST_PLAY:
        if (pressed & PAPP_BTN_START) { paused = !paused; sfx(SFX_PAUSE); a_music = paused ? -1 : world; }
        if (paused) break;
        update_hero();
        if (state != ST_PLAY) break;
        update_enemies();
        update_items();
        update_fx();
        update_camera();
        if (++timer_f >= 60) { timer_f = 0; if (--timer_s <= 0) { hero.big = hero.dbl = false; hero.invuln = 0; hurt_hero(); } }
        break;
    case ST_DYING:
        state_t++;
        if (state_t > 30) { hero.vy += 0.35f; hero.y += hero.vy; }
        update_fx();
        if (state_t > 150) {
            lives--;
            if (lives < 0) { state = ST_OVER; state_t = 0; save_game(); }
            else { state = ST_INTRO; state_t = 0; }
        }
        break;
    case ST_CLEAR:
        state_t++;
        update_fx();
        if (timer_s > 0 && state_t > 40 && (state_t & 1)) { timer_s--; score += 10; }
        if (state_t % 9 == 0) burst(frange(camx + 40, camx + W - 40) - camx + camx, frange(30, 120), 16,
                                    rgb((int)frange(120, 255), (int)frange(120, 255), (int)frange(120, 255)));
        if (state_t > 240 && timer_s <= 0) {
            checkpoint_x = -1;
            if (world == 2) { state = ST_WIN; state_t = 0; memset(en, 0, sizeof en); memset(items, 0, sizeof items); if (maxworld < 2) maxworld = 2; save_game(); a_music = 0; }
            else { world++; if (world > maxworld) maxworld = world; save_game(); state = ST_INTRO; state_t = 0; }
        }
        break;
    case ST_OVER:
        if (++state_t > 200 || (state_t > 60 && (pressed & (PAPP_BTN_A | PAPP_BTN_START)))) {
            state = ST_TITLE; state_t = 0; world = 0; a_music = 0; make_tiles(0);
        }
        break;
    case ST_WIN:
        state_t++;
        update_fx();
        if (state_t % 7 == 0) burst(frange(40, W - 40) + camx, frange(30, 140), 18,
                                    rgb((int)frange(120, 255), (int)frange(120, 255), (int)frange(120, 255)));
        if (state_t > 120 && (pressed & (PAPP_BTN_A | PAPP_BTN_START))) { state = ST_TITLE; state_t = 0; world = 0; make_tiles(0); }
        break;
    }
}

/* ══ render ═══════════════════════════════════════════════════════════════ */
static void draw_background(float cam)
{
    for (int y = 0; y < H; y++) {
        uint16_t c = sky_row[y];
        uint16_t *row = &px[y * stride];
        for (int x = 0; x < W; x++) row[x] = c;
    }
    if (th->cave) {   /* twinkling crystals in the dark */
        for (int i = 0; i < 40; i++) {
            float x = fmodf(i * 97.0f - cam * 0.15f, W + 20.0f);
            if (x < 0) x += W + 20;
            float y = 20 + (i * 53) % 150;
            int a = (int)(16 + 14 * sinf(tick * 0.05f + i));
            pmix((int)x, (int)y, rgb(200, 170, 255), a);
            if (i % 3 == 0) { pmix((int)x + 1, (int)y, rgb(200, 170, 255), a / 2); pmix((int)x, (int)y + 1, rgb(200, 170, 255), a / 2); }
        }
    } else {          /* drifting clouds */
        for (int i = 0; i < 6; i++) {
            float x = fmodf(i * 140.0f - cam * 0.3f - tick * 0.1f, W + 120.0f);
            if (x < 0) x += W + 120;
            x -= 60;
            float y = 30 + (i * 37) % 60;
            disc(x, y, 12, rgb(255, 255, 255), 22);
            disc(x + 14, y - 5, 15, rgb(255, 255, 255), 22);
            disc(x + 30, y, 11, rgb(255, 255, 255), 22);
        }
    }
    /* far mountains */
    uint16_t mc = c3(th->mount, 0), mrim = c3(th->mount, 26);
    for (int x = 0; x < W; x++) {
        float wx = x + cam * 0.25f;
        int top = (int)(112 + 22 * sinf(wx * 0.011f) + 12 * sinf(wx * 0.031f + 1.0f));
        if (top < 0) top = 0;
        for (int y = top; y < H; y++) px[y * stride + x] = y < top + 2 ? mrim : mc;
    }
    /* near hills */
    uint16_t hc = c3(th->hill, 0), hrim = c3(th->hill, 30);
    for (int x = 0; x < W; x++) {
        float wx = x + cam * 0.55f;
        int top = (int)(158 + 14 * sinf(wx * 0.02f) + 8 * sinf(wx * 0.05f + 2.0f));
        for (int y = top; y < H; y++) px[y * stride + x] = y < top + 2 ? hrim : hc;
    }
}

static void draw_tile_img(const uint16_t *img, int x, int y, bool key)
{
    for (int r = 0; r < T; r++) {
        int yy = y + r;
        if ((unsigned)yy >= H) continue;
        uint16_t *dst = &px[yy * stride];
        const uint16_t *src = &img[r * T];
        for (int c = 0; c < T; c++) {
            int xx = x + c;
            if ((unsigned)xx < W && (!key || src[c] != KEY)) dst[xx] = src[c];
        }
    }
}

static void draw_portal(int x, int y)
{
    float cx = x + 8, cy = y + 16;
    disc(cx, cy, 13, rgb(40, 10, 70), 26);
    for (int k = 0; k < 3; k++) {
        float r = 13 - k * 3.5f;
        ring(cx, cy, r * 0.75f, r * 1.25f, 1.6f, k == 0 ? rgb(90, 255, 230) : k == 1 ? rgb(170, 110, 255) : rgb(255, 120, 220), 28);
    }
    for (int i = 0; i < 8; i++) {
        float a = tick * 0.12f + i * 0.785f, r = 4 + 6 * fabsf(sinf(tick * 0.03f + i));
        pset((int)(cx + cosf(a) * r * 0.75f), (int)(cy + sinf(a) * r * 1.25f), rgb(255, 255, 255));
    }
}

static void draw_tiles(float cam)
{
    int c0 = (int)floorf(cam / T), c1 = c0 + W / T + 1;
    int ox = (int)floorf(cam);
    for (int ty = 0; ty < ROWS; ty++)
        for (int tx = c0; tx <= c1; tx++) {
            char c = tile(tx, ty);
            if (c == ' ' || (c == 'B' && (tx < 0 || tx >= LW))) continue;
            int x = tx * T - ox, y = ty * T;
            if (tx == bump_tx && ty == bump_ty && bump_t > 0) y -= (int)(sinf(bump_t * 0.314f) * 5);
            switch (c) {
            case '#': draw_tile_img(tile(tx, ty - 1) == '#' ? tiles[TI_GROUND] : tiles[TI_GTOP], x, y, false); break;
            case 'S': draw_tile_img(tiles[TI_STONE], x, y, false); break;
            case 'C': case 'M': case 'J': case 'L': {
                draw_tile_img(tiles[TI_CRATE], x, y, false);
                int glow = (int)(10 + 8 * sinf(tick * 0.1f));
                fill_rect(x + 2, y + 2, 12, 12, rgb(255, 240, 160), glow / 3);
                break;
            }
            case 'U': draw_tile_img(tiles[TI_USED], x, y, false); break;
            case 'B': draw_tile_img(tiles[TI_METAL], x, y, false); break;
            case '=': draw_tile_img(tiles[TI_PLANK], x, y, true); break;
            case '^': draw_tile_img(tiles[TI_SPIKE], x, y, true); break;
            case 'o': blit(&gear_spr[(tick / 5 + tx) & 7], x + 2, y + 2 + (int)(sinf(tick * 0.08f + tx) * 1.5f)); break;
            case 'k': {
                bool on = checkpoint_x == tx;
                fill_rect(x + 7, y - 10, 2, 26, rgb(200, 200, 210), 32);
                disc(x + 8, y - 12, 4, on ? rgb(80, 255, 130) : rgb(255, 80, 80), 32);
                disc(x + 8, y - 12, 7, on ? rgb(80, 255, 130) : rgb(255, 80, 80), 8 + (tick / 8 & 1) * 6);
                break;
            }
            case 'X': draw_portal(x, y); break;
            }
        }
}

static void draw_hero(float cam)
{
    if (state == ST_PLAY && hero.invuln > 0 && (hero.invuln / 3) & 1) return;
    int f = 0;
    if (state == ST_DYING) f = 3;
    else if (!hero.ground) f = 3;
    else if (fabsf(hero.vx) > 0.2f) f = 1 + ((int)hero.animt & 1);
    const spr_t *s = &hero_spr[hero.dbl ? 1 : 0][hero.big ? 1 : 0][f][hero.left ? 1 : 0];
    blit(s, (int)floorf(hero.x - cam) - 1, (int)floorf(hero.y) - 1);
}

static void draw_entities(float cam)
{
    for (int i = 0; i < NITEM; i++) {
        item_t *it = &items[i];
        if (!it->on) continue;
        blit(&shroom_spr[it->type], (int)(it->x - cam) - 1, (int)it->y - 2);
    }
    for (int i = 0; i < NENEMY; i++) {
        enemy_t *e = &en[i];
        if (!e->on || !e->active) continue;
        int x = (int)(e->x - cam) - 1, y = (int)e->y - 4;
        if (e->type == 0) {
            const spr_t *s = &beetle_spr[(tick / 8) & 1][e->flip ? 1 : 0];
            if (e->dead == 1) blit_squash(s, x, y + 10, 6);
            else if (e->dead == 2) blit_flipv(s, x, y);
            else blit(s, x, y);
        } else {
            const spr_t *s = &drone_spr[e->flip ? 1 : 0];
            int yy = (int)e->y - 2;
            if (e->dead == 1) { blit_squash(s, x, yy + 6, 4); continue; }
            if (e->dead == 2) { blit_flipv(s, x, yy); continue; }
            blit(s, x, yy);
            int w = 3 + (int)(fabsf(sinf(tick * 0.9f)) * 6);   /* spinning propeller */
            fill_rect(x + 7 - w, yy, w * 2, 1, rgb(230, 230, 240), 32);
        }
    }
    draw_hero(cam);
    for (int i = 0; i < NPART; i++) {
        part_t *p = &parts[i];
        if (p->life <= 0) continue;
        int x = (int)(p->x - cam), y = (int)p->y;
        if (p->kind == 1) blit(&gear_spr[(tick / 2) & 7], x - 6, y - 6);
        else if (p->kind == 2) fill_rect(x - 3, y - 3, 6, 6, p->c, 32);
        else fill_rect(x, y, 2, 2, p->c, p->life > 10 ? 32 : p->life * 3);
    }
    for (int i = 0; i < NPOP; i++)
        if (pops[i].life > 0) text((int)(pops[i].x - cam), (int)pops[i].y, pops[i].txt, rgb(255, 255, 255), 1);
}

static void draw_hud(void)
{
    char b[40];
    fill_rect(0, 0, W, 18, rgb(0, 0, 0), 10);
    sfmt(b, "ABED x", lives < 0 ? 0 : lives, 0, NULL);
    text(6, 5, b, rgb(255, 255, 255), 1);
    sfmt(b, "*x", gears, 2, NULL);
    text(70, 5, b, rgb(255, 220, 80), 1);
    sfmt(b, "", score, 6, NULL);
    text(118, 5, b, rgb(255, 255, 255), 1);
    sfmt(b, "WORLD ", world + 1, 0, NULL);
    text(176, 5, b, rgb(255, 255, 255), 1);
    sfmt(b, "TIME ", timer_s < 0 ? 0 : timer_s, 3, NULL);
    text(236, 5, b, timer_s < 60 ? rgb(255, 120, 100) : rgb(255, 255, 255), 1);
    if (hero.dbl) text(296, 5, "2JUMP", rgb(200, 150, 255), 1);
}

/* on-screen controls and the exit X */
#define BTN_L_X 30
#define BTN_R_X 76
#define BTN_Y   206
#define BTN_A_X 372
#define BTN_B_X 326
static bool touch_ui;

static void draw_round_btn(int cx, int cy, int r, const char *label, bool down)
{
    disc(cx, cy, r, down ? rgb(255, 255, 255) : rgb(10, 20, 30), down ? 14 : 12);
    ring(cx, cy, r, r, 1.2f, rgb(255, 255, 255), 20);
    text(cx - text_w(label, 1) / 2, cy - 3, label, rgb(255, 255, 255), 1);
}

static void draw_controls(uint32_t touch_btn)
{
    draw_round_btn(W - 16, 16, 12, "x", false);   /* exit */
    if (!touch_ui || state != ST_PLAY) return;
    draw_round_btn(BTN_L_X, BTN_Y, 20, "<", touch_btn & PAPP_BTN_LEFT);
    draw_round_btn(BTN_R_X, BTN_Y, 20, ">", touch_btn & PAPP_BTN_RIGHT);
    draw_round_btn(BTN_B_X, BTN_Y, 18, "B", touch_btn & PAPP_BTN_B);
    draw_round_btn(BTN_A_X, BTN_Y, 20, "A", touch_btn & PAPP_BTN_A);
}

static void draw_title(void)
{
    camx = fmodf(camx, 100000.0f);
    draw_background(camx);
    int ox = (int)camx % T;
    for (int x = -1; x <= W / T; x++) {
        draw_tile_img(tiles[TI_GTOP], x * T - ox, 13 * T, false);
        draw_tile_img(tiles[TI_GROUND], x * T - ox, 14 * T, false);
    }
    text_outline(W / 2 - text_w("ABED'S", 4) / 2, 26, "ABED'S", rgb(255, 214, 60), rgb(30, 20, 40), 4);
    text_outline(W / 2 - text_w("ADVENTURE", 3) / 2, 66, "ADVENTURE", rgb(45, 224, 194), rgb(30, 20, 40), 3);
    text_c(98, "COLLECT THE GEARS - FIX THE MACHINE!", rgb(255, 255, 255), 1);
    /* hero walking across */
    int f = 1 + ((tick / 8) & 1);
    blit(&hero_spr[0][1][f][0], W / 2 - 6, 13 * T - 24);
    blit(&beetle_spr[(tick / 8) & 1][0], W / 2 + 60 + (int)(sinf(tick * 0.02f) * 20), 13 * T - 13);
    blit(&gear_spr[(tick / 5) & 7], W / 2 - 60, 13 * T - 40 + (int)(sinf(tick * 0.08f) * 3));
    blit(&shroom_spr[0], W / 2 - 100, 13 * T - 14);
    if ((tick / 30) & 1) text_c(126, "PRESS A OR TAP TO START", rgb(255, 255, 255), 1);
    char b[40];
    sfmt(b, "BEST ", best, 6, NULL);
    text_c(142, b, rgb(255, 230, 140), 1);
    if (maxworld > 0) {
        sfmt(b, "< WORLD ", world + 1, 0, ": "); strcat(b, THEMES[world].name); strcat(b, " >");
        text_c(158, b, rgb(200, 255, 240), 1);
    }
    text(6, H - 10, "BY ABED MIHILIA 2026", rgb(255, 255, 255), 1);
}

static void draw_center_card(const char *l1, const char *l2, uint16_t bg)
{
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) px[y * stride + x] = bg;
    text_c(80, l1, rgb(255, 255, 255), 3);
    if (l2) text_c(120, l2, rgb(255, 220, 120), 2);
}

static void render(uint32_t touch_btn)
{
    char b[48];
    switch (state) {
    case ST_TITLE:
        draw_title();
        break;
    case ST_INTRO:
        sfmt(b, "WORLD ", world + 1, 0, NULL);
        draw_center_card(b, THEMES[world].name, rgb(12, 14, 26));
        blit(&hero_spr[0][0][0][0], W / 2 - 30, 158);
        sfmt(b, "x ", lives, 0, NULL);
        text(W / 2 - 10, 162, b, rgb(255, 255, 255), 2);
        break;
    case ST_OVER:
        draw_center_card("GAME OVER", NULL, rgb(20, 8, 16));
        { char b2[20]; sfmt(b, "SCORE ", score, 6, "   "); sfmt(b2, "BEST ", best, 6, NULL); strcat(b, b2); }
        text_c(130, b, rgb(255, 230, 140), 1);
        break;
    case ST_WIN:
        draw_background(camx);
        draw_entities(camx);
        text_outline(W / 2 - text_w("THE MACHINE WORKS!", 2) / 2, 50, "THE MACHINE WORKS!", rgb(255, 214, 60), rgb(30, 20, 40), 2);
        text_c(86, "THANK YOU, ABED!", rgb(255, 255, 255), 2);
        sfmt(b, "FINAL SCORE ", score, 6, NULL);
        text_c(120, b, rgb(200, 255, 240), 1);
        if (state_t > 120 && ((tick / 30) & 1)) text_c(150, "PRESS A TO PLAY AGAIN", rgb(255, 255, 255), 1);
        break;
    default:
        draw_background(camx);
        draw_tiles(camx);
        draw_entities(camx);
        draw_hud();
        if (state == ST_CLEAR) {
            text_outline(W / 2 - text_w("WORLD CLEAR!", 3) / 2, 70, "WORLD CLEAR!", rgb(255, 214, 60), rgb(30, 20, 40), 3);
        }
        if (paused) {
            fill_rect(0, 0, W, H, rgb(0, 0, 0), 14);
            text_c(100, "PAUSED", rgb(255, 255, 255), 3);
            text_c(132, "START TO RESUME - TAP X TO QUIT", rgb(255, 230, 140), 1);
        }
        break;
    }
    draw_controls(touch_btn);
}

/* ══ input ════════════════════════════════════════════════════════════════ */
static bool in_circle(int x, int y, int cx, int cy, int r) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; }

/* returns the buttons pressed by touch; *exit set when the X is tapped */
static uint32_t read_touch(papp_surface_t *s, bool *exit_tap, bool *any_tap)
{
    static bool was_x, was_any;
    papp_touch_point_t tp[5];
    int n = papp_disp_touch(s, tp, 5);
    uint32_t b = 0;
    bool on_x = false;
    for (int i = 0; i < n; i++) {
        int x = tp[i].x, y = tp[i].y;
        if (x > W - 40 && y < 40) { on_x = true; continue; }
        if (in_circle(x, y, BTN_L_X, BTN_Y, 26) || (x < BTN_L_X + 20 && y > 160)) b |= PAPP_BTN_LEFT;
        else if (in_circle(x, y, BTN_R_X, BTN_Y, 26)) b |= PAPP_BTN_RIGHT;
        else if (in_circle(x, y, BTN_A_X, BTN_Y, 28)) b |= PAPP_BTN_A;
        else if (in_circle(x, y, BTN_B_X, BTN_Y, 24)) b |= PAPP_BTN_B;
    }
    *exit_tap = on_x && !was_x;
    *any_tap = n > 0 && !on_x && !was_any;
    was_x = on_x;
    was_any = n > 0;
    if (n > 0) touch_ui = true;
    return b;
}

/* ══ main ═════════════════════════════════════════════════════════════════ */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    papp_surface_cfg_t cfg = { .width = W, .height = H, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 2 };
    papp_surface_t *s = papp_disp_surface_create(&cfg);
    if (!s) { papp_log("adventure: no surface\n"); return 2; }
    seed ^= (uint32_t)papp_time_us();
    make_sprites();
    load_save();
    make_tiles(0);
    lv_begin(60);
    state = ST_TITLE; world = 0; a_music = 0;

    a_run = true;
    papp_thread_cfg_t tc = { .name = "adv_audio", .stack_size = 8192, .prio = PAPP_PRIO_HIGH, .core = -1 };
    a_thread = papp_thread_create(audio_thread, NULL, &tc);

    papp_gamepad_t pad0;
    touch_ui = !(papp_input_gamepad(0, &pad0) && pad0.connected);

    uint32_t prev = 0;
    int64_t last = papp_time_us();
    double acc = 0;
    while (!papp_exit_requested()) {
        /* input */
        uint32_t b = 0;
        papp_gamepad_t pad;
        for (int p = 0; p < 2; p++)
            if (papp_input_gamepad(p, &pad)) {
                b |= pad.buttons;
                if (pad.lx < -14000) b |= PAPP_BTN_LEFT;
                if (pad.lx > 14000) b |= PAPP_BTN_RIGHT;
                if (pad.ly < -20000) b |= PAPP_BTN_UP;
                if (pad.ly > 20000) b |= PAPP_BTN_DOWN;
            }
        if (b & PAPP_BTN_X) b |= PAPP_BTN_B;
        bool exit_tap, any_tap;
        uint32_t tb = read_touch(s, &exit_tap, &any_tap);
        if (exit_tap) {
            if (state == ST_PLAY && !paused) { paused = true; a_music = -1; sfx(SFX_PAUSE); }
            else break;
        }
        b |= tb;
        if (any_tap && state != ST_PLAY) b |= PAPP_BTN_A;
        uint32_t pr = b & ~prev;
        prev = b;

        /* fixed 60 Hz simulation */
        int64_t now = papp_time_us();
        acc += (now - last) / 1e6;
        last = now;
        if (acc > 0.1) acc = 0.1;
        bool first = true;
        while (acc >= 1.0 / 60) {
            btn = b;
            pressed = first ? pr : 0;
            if (paused && state == ST_PLAY) {
                if (pressed & PAPP_BTN_START) { paused = false; a_music = world; sfx(SFX_PAUSE); }
                else if (pressed & PAPP_BTN_A && touch_ui) { paused = false; a_music = world; }
            } else update();
            first = false;
            acc -= 1.0 / 60;
        }

        px = papp_disp_lock(s, &stride);
        render(tb);
        papp_disp_present(s);
    }

    a_run = false;
    if (a_thread) papp_thread_join(a_thread, 1000, NULL);
    papp_disp_surface_destroy(s);
    return 0;
}
