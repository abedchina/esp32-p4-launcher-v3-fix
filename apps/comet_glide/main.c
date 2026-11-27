/*
 * Comet Glide - an original arcade game by Abed Mihilia.
 *
 * Guide a glowing comet through living nebulae. Catch the light seeds to draw
 * constellations across the sky, chain them fast for a combo, and dodge the
 * burning meteors. Every wave paints a new nebula.
 *
 * Drawn in software on a 400x240 RGB565 surface (scaled x2 by the launcher).
 * Touch: drag anywhere - the comet follows your finger's movement.
 * X (top right): pause, X again: exit. Gamepad: stick / D-pad, A = start.
 */
#include "papp/papp.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

PAPP_MANIFEST(.id = "abed.comet", .name = "Comet Glide", .version = PAPP_APP_VERSION, .main_stack = 32768);

#define W 400
#define H 240

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

static inline void pmix(int x, int y, uint16_t c, int a)
{
    if ((unsigned)x < W && (unsigned)y < H) {
        uint16_t *p = &px[y * stride + x];
        *p = a >= 32 ? c : blend(*p, c, a);
    }
}

/* additive light: r, g, b in 0..255 (may exceed, clamped) */
static inline uint16_t addc(uint16_t d, int r, int g, int b)
{
    int dr = ((d >> 11) & 31) + (r >> 3), dg = ((d >> 5) & 63) + (g >> 2), db = (d & 31) + (b >> 3);
    if (dr > 31) dr = 31;
    if (dg > 63) dg = 63;
    if (db > 31) db = 31;
    return (uint16_t)((dr << 11) | (dg << 5) | db);
}

static inline void padd(int x, int y, int r, int g, int b)
{
    if ((unsigned)x < W && (unsigned)y < H) { uint16_t *p = &px[y * stride + x]; *p = addc(*p, r, g, b); }
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

static void ring(float cx, float cy, float r, float t, uint16_t c, int a)
{
    int x0 = (int)(cx - r - t) - 1, x1 = (int)(cx + r + t) + 1, y0 = (int)(cy - r - t) - 1, y1 = (int)(cy + r + t) + 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            if (fabsf(sqrtf(dx * dx + dy * dy) - r) < t) pmix(x, y, c, a);
        }
}

/* soft round light, falloff (1-d^2/r^2)^2 * k */
static void glow(float cx, float cy, float rad, int r, int g, int b, float k)
{
    if (rad < 0.5f) return;
    int x0 = (int)(cx - rad), x1 = (int)(cx + rad) + 1, y0 = (int)(cy - rad), y1 = (int)(cy + rad) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W - 1) x1 = W - 1;
    if (y1 > H - 1) y1 = H - 1;
    float inv = 1.0f / (rad * rad);
    for (int y = y0; y <= y1; y++) {
        float dy = y + 0.5f - cy;
        uint16_t *row = &px[y * stride];
        for (int x = x0; x <= x1; x++) {
            float dx = x + 0.5f - cx;
            float q = 1.0f - (dx * dx + dy * dy) * inv;
            if (q <= 0) continue;
            float f = q * q * k;
            row[x] = addc(row[x], (int)(r * f), (int)(g * f), (int)(b * f));
        }
    }
}

/* glowing line (constellations) */
static void light_line(float x0, float y0, float x1, float y1, int r, int g, int b)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    int n = (int)len + 1;
    for (int i = 0; i <= n; i++) {
        float t = (float)i / n;
        int x = (int)(x0 + dx * t), y = (int)(y0 + dy * t);
        padd(x, y, r, g, b);
        padd(x + 1, y, r / 3, g / 3, b / 3);
        padd(x, y + 1, r / 3, g / 3, b / 3);
    }
}

/* ══ random / noise ═══════════════════════════════════════════════════════ */
static uint32_t seed = 0x13579BDF;
static float frand(void) { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; }
static float frange(float a, float b) { return a + (b - a) * frand(); }
static uint32_t hash2(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static int noise_salt;
static float lat(int x, int y, int period)
{
    x = ((x % period) + period) % period;
    return (hash2(x + noise_salt * 7919, y + noise_salt * 104729) & 0xFFFF) / 65535.0f;
}

/* value noise, tileable along x with the given period (in cells) */
static float vnoise(float x, float y, int period)
{
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    float a = lat(xi, yi, period), b = lat(xi + 1, yi, period), c = lat(xi, yi + 1, period), d = lat(xi + 1, yi + 1, period);
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

static float fbm(float x, float y)
{
    float s = 0, amp = 0.5f;
    int cell = 80;
    for (int o = 0; o < 5; o++) {
        s += amp * vnoise(x / cell, y / cell, W / cell);
        amp *= 0.5f;
        cell /= 2;
    }
    return s;
}

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

static void draw_char(int x, int y, char ch, uint16_t c, int sc, int a)
{
    const char *p = strchr(FONT_CHARS, ch);
    if (!p || !ch) return;
    const uint8_t *g = FONT[p - FONT_CHARS];
    for (int r = 0; r < 7; r++)
        for (int k = 0; k < 5; k++)
            if (g[r] & (0x10 >> k)) fill_rect(x + k * sc, y + r * sc, sc, sc, c, a);
}

static int text_w(const char *s, int sc) { return (int)strlen(s) * 6 * sc - sc; }

static void text_a(int x, int y, const char *s, uint16_t c, int sc, int a)
{
    for (int i = 0; s[i]; i++) {
        draw_char(x + i * 6 * sc + 1, y + 1, s[i], rgb(4, 4, 14), sc, a);
        draw_char(x + i * 6 * sc, y, s[i], c, sc, a);
    }
}
static void text(int x, int y, const char *s, uint16_t c, int sc) { text_a(x, y, s, c, sc, 32); }
static void text_c(int y, const char *s, uint16_t c, int sc, int a) { text_a((W - text_w(s, sc)) / 2, y, s, c, sc, a); }

/* ══ nebula themes ════════════════════════════════════════════════════════ */
typedef struct { uint8_t deep[3], mid[3], hot[3], seed[3]; } theme_t;
static const theme_t THEMES[] = {
    { {8, 6, 26},  {82, 34, 124}, {70, 215, 225}, {120, 255, 240} },   /* violet lagoon */
    { {18, 5, 20}, {128, 32, 84}, {255, 176, 96},  {255, 220, 150} },  /* rose dawn */
    { {4, 10, 26}, {22, 66, 130}, {120, 225, 255}, {170, 240, 255} },  /* deep ocean */
    { {4, 15, 15}, {22, 96, 76},  {190, 255, 170}, {200, 255, 200} },  /* aurora */
    { {18, 6, 6},  {120, 42, 22}, {255, 196, 96},  {255, 230, 140} },  /* ember */
};
#define NTHEME (int)(sizeof THEMES / sizeof THEMES[0])

static uint16_t bg[W * H];
static int theme;

static void make_nebula(int t)
{
    const theme_t *th = &THEMES[t % NTHEME];
    noise_salt = t * 3 + 1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float n = fbm((float)x, (float)y * 1.15f);
            float m = fbm((float)x + 1000.0f, (float)y * 0.9f + 500.0f);
            float v = (n - 0.32f) * 2.1f;
            if (v < 0) v = 0;
            if (v > 1) v = 1;
            float hot = v * v * v * (0.5f + m);
            float vy = (float)y / H - 0.5f;
            float vign = 1.0f - vy * vy * 1.4f;
            float r = th->deep[0] + (th->mid[0] - th->deep[0]) * v + th->hot[0] * hot * 0.55f;
            float g = th->deep[1] + (th->mid[1] - th->deep[1]) * v + th->hot[1] * hot * 0.55f;
            float b = th->deep[2] + (th->mid[2] - th->deep[2]) * v + th->hot[2] * hot * 0.55f;
            /* dark dust lanes */
            float dust = fbm((float)x + 2500.0f, (float)y * 1.6f + 900.0f);
            if (dust > 0.56f) { float k = 1.0f - (dust - 0.56f) * 3.0f; if (k < 0.35f) k = 0.35f; r *= k; g *= k; b *= k; }
            bg[y * W + x] = rgb((int)(r * vign), (int)(g * vign), (int)(b * vign));
        }
}

/* ══ world ════════════════════════════════════════════════════════════════ */
#define NSTAR 150
typedef struct { float x, y, sp, ph; uint8_t layer; } star_t;
static star_t stars[NSTAR];

#define NPART 420
typedef struct { float x, y, vx, vy, size; int life, max; uint8_t r, g, b; } part_t;
static part_t parts[NPART];
static int part_next;

#define NORB 14
enum { ORB_SEED, ORB_GOLD, ORB_MAGNET, ORB_SHIELD };
typedef struct { float x, y, vx, base, ph; int type, alive; uint8_t r, g, b; } orb_t;
static orb_t orbs[NORB];

#define NMET 10
typedef struct { float x, y, vx, vy, r, rot, vr; int alive; } met_t;
static met_t mets[NMET];

#define NCON 48
typedef struct { float x, y; int alive, link, tw; uint8_t r, g, b; } con_t;
static con_t cons[NCON];
static int con_last = -1;

#define NPOP 8
typedef struct { float x, y; int life; char txt[12]; uint8_t r, g, b; } pop_t;
static pop_t pops[NPOP];

static const uint8_t JEWELS[][3] = { {120, 255, 240}, {255, 140, 220}, {150, 170, 255}, {255, 230, 120}, {170, 255, 150} };

enum { ST_TITLE, ST_PLAY, ST_OVER };
static int state, state_t, frame;
static bool paused;
static float cx = 90, cy = 120, cvx, cvy;           /* comet */
static int score, best, shields, combo, combo_t, wave, got, need, invul, shake, magnet_t, flash, wave_banner;
static int orb_timer, met_timer;
static float shx, shy;

static float speed_mul(void) { return 1.0f + wave * 0.13f; }

/* ══ audio: soft pad + chimes, all synthesized ════════════════════════════ */
static volatile bool a_run;
static volatile int a_q[16];
static volatile unsigned a_qh, a_qt;
static papp_thread_t *a_thread;

/* note > 0: chime (MIDI), -1: meteor hit, -2: game over */
static void snd(int note) { a_q[a_qh & 15] = note; a_qh++; }

static const uint8_t PENTA[] = { 69, 72, 74, 76, 79, 81, 84, 86, 88, 91, 93, 96 };
static float mtof(int m) { return 440.0f * powf(2.0f, (m - 69) / 12.0f); }

static int audio_thread(void *arg)
{
    (void)arg;
    const int R = 22050;
    papp_audio_cfg_t ac = { .rate = R, .channels = 1, .format = PAPP_AUDIO_S16, .latency_ms = 80 };
    papp_audio_stream_t *as = papp_audio_open(&ac);
    if (!as) return 0;
    static const uint8_t CH[4][3] = { {57, 60, 64}, {53, 57, 60}, {48, 55, 64}, {55, 59, 62} };
    int16_t buf[256];
    float vph[6] = { 0 }, vf[6] = { 0 }, venv[6] = { 0 };
    int vnext = 0;
    float pph[3] = { 0 }, bph = 0, lfo = 0, hit = 0, hitlp = 0;
    long t = 0;
    const long CHORD = (long)(R * 4.8f);
    uint32_t noise = 0x9E3779B9;
    while (a_run) {
        while (a_qt != a_qh) {
            int n = a_q[a_qt & 15]; a_qt++;
            if (n == -1) hit = 1.0f;
            else if (n == -2) { for (int k = 0; k < 3; k++) { vf[vnext] = mtof(64 - k * 5); venv[vnext] = 0.9f; vnext = (vnext + 1) % 6; } }
            else if (n > 0) { vf[vnext] = mtof(n); venv[vnext] = 1.0f; vph[vnext] = 0; vnext = (vnext + 1) % 6; }
        }
        for (int i = 0; i < 256; i++, t++) {
            int ci = (int)((t / CHORD) & 3);
            float pos = (float)(t % CHORD) / CHORD;
            float env = pos < 0.15f ? pos / 0.15f : pos > 0.85f ? (1.0f - pos) / 0.15f : 1.0f;
            lfo += 0.35f / R; if (lfo >= 1) lfo -= 1;
            float trem = 0.85f + 0.15f * sinf(lfo * 6.2831853f);
            float s = 0;
            for (int k = 0; k < 3; k++) {
                pph[k] += mtof(CH[ci][k]) / R; if (pph[k] >= 1) pph[k] -= 1;
                s += (4.0f * fabsf(pph[k] - 0.5f) - 1.0f) * 0.045f * env * trem;
            }
            bph += mtof(CH[ci][0] - 12) / R; if (bph >= 1) bph -= 1;
            s += sinf(bph * 6.2831853f) * 0.07f * env;
            for (int v = 0; v < 6; v++) {
                if (venv[v] < 0.001f) continue;
                vph[v] += vf[v] / R; if (vph[v] >= 1) vph[v] -= 1;
                float ph = vph[v] * 6.2831853f;
                s += (sinf(ph) + 0.35f * sinf(ph * 2.0f) + 0.12f * sinf(ph * 3.0f)) * 0.11f * venv[v];
                venv[v] *= 0.99982f;
            }
            if (hit > 0.001f) {
                noise ^= noise << 13; noise ^= noise >> 17; noise ^= noise << 5;
                float w = (noise & 0xFFFF) / 32768.0f - 1.0f;
                hitlp += (w - hitlp) * 0.08f;
                s += hitlp * 0.9f * hit;
                hit *= 0.99975f;
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
#define SAVE_DIR  "/sd/data/abed.comet"
#define SAVE_FILE SAVE_DIR "/save.txt"

static void load_save(void)
{
    best = 0;
    FILE *f = fopen(SAVE_FILE, "rb");
    if (!f) return;
    char buf[32] = { 0 };
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    for (const char *p = buf; *p >= '0' && *p <= '9'; p++) best = best * 10 + (*p - '0');
}

static void save_best(void)
{
    mkdir("/sd/data", 0777);
    mkdir(SAVE_DIR, 0777);
    FILE *f = fopen(SAVE_FILE, "wb");
    if (!f) return;
    char buf[16];
    sfmt(buf, "", best, 0, "\n");
    fwrite(buf, 1, strlen(buf), f);
    fclose(f);
}

/* ══ effects ══════════════════════════════════════════════════════════════ */
static void spark(float x, float y, float vx, float vy, int life, float size, const uint8_t *c)
{
    part_t *p = &parts[part_next];
    part_next = (part_next + 1) % NPART;
    p->x = x; p->y = y; p->vx = vx; p->vy = vy; p->life = p->max = life; p->size = size;
    p->r = c[0]; p->g = c[1]; p->b = c[2];
}

static void burst(float x, float y, int n, float sp, int life, const uint8_t *c)
{
    for (int i = 0; i < n; i++) {
        float a = frand() * 6.2831853f, v = frange(0.3f, sp);
        spark(x, y, cosf(a) * v, sinf(a) * v, life + (int)frange(0, 14), frange(1.5f, 3.2f), c);
    }
}

static void pop(float x, float y, const char *t, const uint8_t *c)
{
    for (int i = 0; i < NPOP; i++)
        if (pops[i].life <= 0) {
            pops[i].x = x; pops[i].y = y; pops[i].life = 50;
            strncpy(pops[i].txt, t, sizeof pops[i].txt - 1);
            pops[i].txt[sizeof pops[i].txt - 1] = 0;
            pops[i].r = c[0]; pops[i].g = c[1]; pops[i].b = c[2];
            return;
        }
}

/* ══ game flow ════════════════════════════════════════════════════════════ */
static void init_stars(void)
{
    for (int i = 0; i < NSTAR; i++) {
        stars[i].x = frange(0, W); stars[i].y = frange(0, H);
        stars[i].layer = (uint8_t)(i % 3);
        stars[i].sp = stars[i].layer == 0 ? 0.15f : stars[i].layer == 1 ? 0.4f : 0.9f;
        stars[i].ph = frange(0, 6.28f);
    }
}

static void clear_world(void)
{
    memset(orbs, 0, sizeof orbs);
    memset(mets, 0, sizeof mets);
    memset(cons, 0, sizeof cons);
    memset(pops, 0, sizeof pops);
    for (int i = 0; i < NPART; i++) parts[i].life = 0;
    con_last = -1;
}

static void new_game(void)
{
    clear_world();
    score = 0; shields = 3; combo = 0; combo_t = 0; wave = 0; got = 0; need = 10;
    invul = 60; shake = 0; magnet_t = 0; flash = 0; wave_banner = 120;
    orb_timer = 20; met_timer = 140;
    cx = 90; cy = H / 2; cvx = cvy = 0;
    if (theme != 0) { theme = 0; make_nebula(theme); }
    state = ST_PLAY; state_t = 0; paused = false;
}

static void next_wave(void)
{
    wave++;
    got = 0;
    need = 10 + wave * 2;
    if (need > 22) need = 22;
    theme = wave % NTHEME;
    make_nebula(theme);
    flash = 24;
    wave_banner = 120;
    for (int k = 0; k < 4; k++) snd(PENTA[4 + k * 2 > 11 ? 11 : 4 + k * 2]);
    for (int i = 0; i < NCON; i++) cons[i].link = 0;   /* the finished constellation stays, unlinked from the next */
    con_last = -1;
    score += 50 * wave;
}

static void spawn_orb(void)
{
    for (int i = 0; i < NORB; i++)
        if (!orbs[i].alive) {
            orb_t *o = &orbs[i];
            o->alive = 1;
            o->x = W + 12;
            o->base = frange(30, H - 30);
            o->y = o->base;
            o->vx = -frange(1.1f, 1.9f) * speed_mul();
            o->ph = frange(0, 6.28f);
            float r = frand();
            o->type = r < 0.07f ? ORB_GOLD : r < 0.11f ? ORB_MAGNET : (r < 0.14f && shields < 3) ? ORB_SHIELD : ORB_SEED;
            const uint8_t *c = o->type == ORB_GOLD ? (const uint8_t *)"\xff\xd8\x60" : o->type == ORB_SHIELD ? (const uint8_t *)"\x80\xc0\xff"
                             : o->type == ORB_MAGNET ? (const uint8_t *)"\xff\xff\xff" : JEWELS[(int)(frand() * 5) % 5];
            o->r = c[0]; o->g = c[1]; o->b = c[2];
            return;
        }
}

static void spawn_met(void)
{
    for (int i = 0; i < NMET; i++)
        if (!mets[i].alive) {
            met_t *m = &mets[i];
            m->alive = 1;
            m->r = frange(6, 12 + (wave > 3 ? 3 : wave));
            m->x = W + m->r + 4;
            m->y = frange(16, H - 16);
            m->vx = -frange(1.6f, 2.8f) * speed_mul();
            m->vy = frange(-0.5f, 0.5f) * (wave > 1 ? 1.0f : 0.3f);
            m->rot = frange(0, 6.28f);
            m->vr = frange(-0.05f, 0.05f);
            return;
        }
}

static void collect(orb_t *o)
{
    o->alive = 0;
    uint8_t c[3] = { o->r, o->g, o->b };
    burst(o->x, o->y, 18, 2.6f, 22, c);
    if (o->type == ORB_SHIELD) {
        if (shields < 3) shields++;
        pop(o->x, o->y - 10, "SHIELD", c);
        snd(PENTA[2]); snd(PENTA[6]);
        return;
    }
    if (o->type == ORB_MAGNET) {
        magnet_t = 360;
        pop(o->x, o->y - 10, "MAGNET", c);
        snd(PENTA[5]); snd(PENTA[9]);
        return;
    }
    combo = combo_t > 0 ? combo + 1 : 1;
    if (combo > 9) combo = 9;
    combo_t = 80;
    int mult = combo < 5 ? combo : 5;
    int pts = (o->type == ORB_GOLD ? 5 : 1) * 10 * mult;
    score += pts;
    char b[12];
    sfmt(b, "", pts, 0, NULL);
    pop(o->x, o->y - 10, b, c);
    snd(PENTA[(combo - 1) % 12]);
    if (o->type == ORB_GOLD) snd(PENTA[(combo + 3) % 12]);
    /* the seed becomes a star of the constellation */
    int slot = -1;
    for (int i = 0; i < NCON; i++) if (!cons[i].alive) { slot = i; break; }
    if (slot < 0) slot = (int)(frand() * NCON) % NCON;
    con_t *s = &cons[slot];
    s->alive = 1; s->x = o->x; s->y = o->y; s->tw = (int)frange(0, 60);
    s->r = c[0]; s->g = c[1]; s->b = c[2];
    s->link = con_last >= 0 && cons[con_last].alive ? con_last + 1 : 0;   /* index+1 of the previous star */
    con_last = slot;
    if (++got >= need) next_wave();
}

static void hurt(void)
{
    shields--;
    invul = 100;
    shake = 16;
    combo = 0; combo_t = 0;
    snd(-1);
    static const uint8_t fire[3] = { 255, 150, 70 };
    burst(cx, cy, 30, 3.2f, 30, fire);
    if (shields <= 0) {
        state = ST_OVER; state_t = 0;
        static const uint8_t white[3] = { 255, 255, 255 };
        burst(cx, cy, 60, 4.0f, 50, white);
        snd(-2);
        if (score > best) { best = score; save_best(); }
    }
}

static uint32_t btn, pressed;
static int drag_dx, drag_dy;   /* finger movement this frame (surface px) */

static void update(void)
{
    frame++;
    state_t++;
    /* sky drifts */
    for (int i = 0; i < NSTAR; i++) {
        stars[i].x -= stars[i].sp * (state == ST_PLAY ? speed_mul() : 1.0f);
        if (stars[i].x < -2) { stars[i].x += W + 4; stars[i].y = frange(0, H); }
    }
    for (int i = 0; i < NPART; i++) {
        part_t *p = &parts[i];
        if (p->life <= 0) continue;
        p->life--; p->x += p->vx; p->y += p->vy; p->vx *= 0.96f; p->vy *= 0.96f;
    }
    for (int i = 0; i < NPOP; i++) if (pops[i].life > 0) { pops[i].life--; pops[i].y -= 0.5f; }
    for (int i = 0; i < NCON; i++) if (cons[i].alive) { cons[i].x -= 0.35f; if (cons[i].x < -10) cons[i].alive = 0; }
    if (flash) flash--;
    if (shake) shake--;
    shx = shake ? frange(-3, 3) * shake / 16.0f : 0;
    shy = shake ? frange(-3, 3) * shake / 16.0f : 0;

    if (state == ST_TITLE) {
        float t = frame / 60.0f;
        cx = 200 + sinf(t * 0.9f) * 130; cy = 120 + sinf(t * 1.7f) * 55;
    }
    if (state != ST_PLAY) {
        static const uint8_t tc[3] = { 160, 240, 255 };
        if (state == ST_TITLE) spark(cx, cy, frange(-1.2f, -0.4f), frange(-0.3f, 0.3f), 34, frange(2.0f, 3.5f), tc);
        return;
    }

    /* comet: finger drag (relative), gamepad moves */
    float ax = 0, ay = 0;
    if (btn & PAPP_BTN_LEFT) ax -= 1;
    if (btn & PAPP_BTN_RIGHT) ax += 1;
    if (btn & PAPP_BTN_UP) ay -= 1;
    if (btn & PAPP_BTN_DOWN) ay += 1;
    cvx = cvx * 0.8f + ax * 0.9f;
    cvy = cvy * 0.8f + ay * 0.9f;
    cx += cvx + drag_dx * 1.3f;
    cy += cvy + drag_dy * 1.3f;
    drag_dx = drag_dy = 0;
    if (cx < 10) cx = 10;
    if (cx > W - 10) cx = W - 10;
    if (cy < 10) cy = 10;
    if (cy > H - 10) cy = H - 10;

    /* trail */
    const theme_t *th = &THEMES[theme];
    for (int k = 0; k < 3; k++)
        spark(cx + frange(-2, 2), cy + frange(-2, 2), frange(-1.8f, -0.7f), frange(-0.35f, 0.35f), 42, frange(2.4f, 4.4f), th->seed);
    if (frame % 3 == 0) { static const uint8_t w[3] = { 255, 255, 255 }; spark(cx, cy, frange(-2.2f, -1.2f), frange(-0.2f, 0.2f), 18, 1.4f, w); }

    if (invul) invul--;
    if (magnet_t) magnet_t--;
    if (combo_t) combo_t--; else combo = 0;
    if (wave_banner) wave_banner--;

    if (--orb_timer <= 0) { spawn_orb(); orb_timer = (int)frange(22, 40) - wave * 2; if (orb_timer < 12) orb_timer = 12; }
    if (--met_timer <= 0) { spawn_met(); met_timer = (int)frange(70, 120) - wave * 11; if (met_timer < 22) met_timer = 22; }

    for (int i = 0; i < NORB; i++) {
        orb_t *o = &orbs[i];
        if (!o->alive) continue;
        o->ph += 0.05f;
        o->x += o->vx;
        o->base += 0;
        float ty = o->base + sinf(o->ph) * 12;
        if (magnet_t) {
            float dx = cx - o->x, dy = cy - o->y, d = sqrtf(dx * dx + dy * dy);
            if (d < 150 && d > 1) { o->x += dx / d * 3.2f; o->base += dy / d * 3.2f; ty = o->base + sinf(o->ph) * 12; }
        }
        o->y = ty;
        if (o->x < -14) { o->alive = 0; continue; }
        float dx = cx - o->x, dy = cy - o->y;
        if (dx * dx + dy * dy < 15 * 15) collect(o);
    }
    for (int i = 0; i < NMET; i++) {
        met_t *m = &mets[i];
        if (!m->alive) continue;
        m->x += m->vx; m->y += m->vy; m->rot += m->vr;
        if (m->y < m->r || m->y > H - m->r) m->vy = -m->vy;
        if (frame % 2 == 0) { static const uint8_t em[3] = { 255, 120, 50 }; spark(m->x + m->r * 0.6f, m->y + frange(-m->r, m->r) * 0.5f, frange(0.4f, 1.2f), frange(-0.2f, 0.2f), 20, frange(1.5f, 2.8f), em); }
        if (m->x < -m->r - 6) { m->alive = 0; continue; }
        float dx = cx - m->x, dy = cy - m->y, rr = m->r * 0.85f + 4;
        if (!invul && dx * dx + dy * dy < rr * rr) { m->alive = 0; hurt(); if (state != ST_PLAY) return; }
    }
}

/* ══ render ═══════════════════════════════════════════════════════════════ */
static void draw_bg(void)
{
    int off = (int)(frame * 0.25f) % W;
    int sy = (int)shy, sx = (int)shx;
    for (int y = 0; y < H; y++) {
        int yy = y + sy; if (yy < 0) yy = 0; if (yy >= H) yy = H - 1;
        uint16_t *dst = &px[y * stride];
        const uint16_t *src = &bg[yy * W];
        int o = (off + sx + W) % W;
        memcpy(dst, src + o, (W - o) * 2);
        memcpy(dst + (W - o), src, o * 2);
    }
}

static void draw_stars(int layer)
{
    for (int i = 0; i < NSTAR; i++) {
        star_t *s = &stars[i];
        if (s->layer != layer) continue;
        float tw = 0.55f + 0.45f * sinf(frame * 0.07f + s->ph);
        int x = (int)(s->x + shx), y = (int)(s->y + shy);
        int v = (int)((layer == 0 ? 90 : layer == 1 ? 160 : 255) * tw);
        padd(x, y, v, v, v + 20);
        if (layer == 2) { padd(x + 1, y, v / 3, v / 3, v / 3); padd(x - 1, y, v / 3, v / 3, v / 3); padd(x, y + 1, v / 3, v / 3, v / 3); padd(x, y - 1, v / 3, v / 3, v / 3); }
    }
}

static void draw_constellations(void)
{
    for (int i = 0; i < NCON; i++) {
        con_t *s = &cons[i];
        if (!s->alive) continue;
        if (s->link > 0 && cons[s->link - 1].alive) {
            con_t *p = &cons[s->link - 1];
            light_line(p->x + shx, p->y + shy, s->x + shx, s->y + shy, s->r / 2, s->g / 2, s->b / 2);
        }
    }
    for (int i = 0; i < NCON; i++) {
        con_t *s = &cons[i];
        if (!s->alive) continue;
        float tw = 0.7f + 0.3f * sinf((frame + s->tw) * 0.09f);
        glow(s->x + shx, s->y + shy, 5, s->r, s->g, s->b, 0.55f * tw);
        padd((int)(s->x + shx), (int)(s->y + shy), 255, 255, 255);
    }
}

static void draw_orbs(void)
{
    for (int i = 0; i < NORB; i++) {
        orb_t *o = &orbs[i];
        if (!o->alive) continue;
        float x = o->x + shx, y = o->y + shy, pulse = 0.8f + 0.2f * sinf(o->ph * 3);
        if (o->type == ORB_MAGNET) {
            for (int k = 0; k < 3; k++) {
                const uint8_t *c = JEWELS[(frame / 6 + k) % 5];
                float a = o->ph * 2 + k * 2.094f;
                glow(x + cosf(a) * 5, y + sinf(a) * 5, 7, c[0], c[1], c[2], 0.9f);
            }
        } else {
            float r = o->type == ORB_GOLD ? 13 : 10;
            glow(x, y, r * pulse, o->r, o->g, o->b, o->type == ORB_GOLD ? 1.2f : 0.9f);
        }
        if (o->type == ORB_SHIELD) ring(x, y, 5.5f, 0.7f, rgb(200, 230, 255), 26);
        disc(x, y, 2.2f, rgb(255, 255, 255), 32);
        if (o->type == ORB_GOLD) {   /* four-point sparkle */
            float k = 7 * pulse;
            light_line(x - k, y, x + k, y, 200, 170, 80);
            light_line(x, y - k, x, y + k, 200, 170, 80);
        }
    }
}

static void draw_meteors(void)
{
    for (int i = 0; i < NMET; i++) {
        met_t *m = &mets[i];
        if (!m->alive) continue;
        float x = m->x + shx, y = m->y + shy;
        glow(x + 2, y, m->r * 1.9f, 255, 90, 30, 0.75f);
        disc(x, y, m->r, rgb(46, 30, 34), 32);
        /* lumpy rim + craters, rotating */
        for (int k = 0; k < 5; k++) {
            float a = m->rot + k * 1.2566f;
            disc(x + cosf(a) * m->r * 0.55f, y + sinf(a) * m->r * 0.55f, m->r * 0.42f, rgb(64, 42, 44), 32);
        }
        disc(x + cosf(m->rot) * m->r * 0.35f, y + sinf(m->rot) * m->r * 0.35f, m->r * 0.22f, rgb(30, 20, 24), 32);
        disc(x - cosf(m->rot + 2) * m->r * 0.4f, y - sinf(m->rot + 2) * m->r * 0.4f, m->r * 0.16f, rgb(30, 20, 24), 32);
        ring(x, y, m->r - 0.5f, 0.9f, rgb(255, 120, 60), 18);   /* hot edge */
    }
}

static void draw_parts(void)
{
    for (int i = 0; i < NPART; i++) {
        part_t *p = &parts[i];
        if (p->life <= 0) continue;
        float f = (float)p->life / p->max;
        glow(p->x + shx, p->y + shy, p->size * (0.4f + f), p->r, p->g, p->b, f * 0.9f);
    }
}

static void draw_comet(void)
{
    if (state == ST_OVER) return;
    if (invul && (invul / 4) % 2) return;
    const theme_t *th = &THEMES[theme];
    float x = cx + shx, y = cy + shy;
    glow(x, y, 22, th->seed[0], th->seed[1], th->seed[2], 0.55f);
    glow(x, y, 11, 255, 255, 255, 0.9f);
    if (magnet_t) ring(x, y, 16 + 2 * sinf(frame * 0.2f), 0.8f, rgb(255, 255, 255), (magnet_t > 60 || (frame / 5) % 2) ? 16 : 4);
    disc(x, y, 3.2f, rgb(255, 255, 255), 32);
}

static void draw_x_button(void)
{
    disc(W - 16, 16, 12, rgb(10, 16, 30), 14);
    ring(W - 16, 16, 12, 0.8f, rgb(255, 255, 255), 22);
    text(W - 16 - 2, 16 - 3, "x", rgb(255, 255, 255), 1);
}

static void draw_hud(void)
{
    char b[24];
    sfmt(b, "SCORE ", score, 6, NULL);
    text(8, 8, b, rgb(255, 255, 255), 1);
    sfmt(b, "BEST ", best > score ? best : score, 6, NULL);
    text(8, 18, b, rgb(170, 190, 220), 1);
    for (int i = 0; i < 3; i++) {
        float x = W / 2 - 14 + i * 14, y = 12;
        if (i < shields) { glow(x, y, 7, 120, 200, 255, 0.9f); disc(x, y, 2.2f, rgb(255, 255, 255), 32); }
        else ring(x, y, 3.5f, 0.6f, rgb(120, 140, 170), 20);
    }
    sfmt(b, "WAVE ", wave + 1, 0, NULL);
    text(W - 40 - text_w(b, 1), 12, b, rgb(220, 230, 255), 1);
    if (combo > 1) {
        sfmt(b, "x", combo < 5 ? combo : 5, 0, " COMBO");
        text((int)cx - text_w(b, 1) / 2, (int)cy - 22, b, rgb(255, 240, 180), 1);
    }
    /* wave progress */
    const theme_t *th = &THEMES[theme];
    int w = (W - 40) * got / (need > 0 ? need : 1);
    fill_rect(20, H - 6, W - 40, 2, rgb(30, 34, 60), 20);
    for (int x = 0; x < w; x++) padd(20 + x, H - 6, th->seed[0] / 2, th->seed[1] / 2, th->seed[2] / 2), padd(20 + x, H - 5, th->seed[0] / 3, th->seed[1] / 3, th->seed[2] / 3);
    if (w > 0) glow(20 + w, H - 5.5f, 5, 255, 255, 255, 0.8f);
}

static void draw_pops(void)
{
    for (int i = 0; i < NPOP; i++) {
        pop_t *p = &pops[i];
        if (p->life <= 0) continue;
        int a = p->life > 20 ? 32 : p->life * 32 / 20;
        text_a((int)p->x - text_w(p->txt, 1) / 2, (int)p->y, p->txt, rgb(p->r, p->g, p->b), 1, a);
    }
}

static void title_glow_text(int y, const char *s, int sc)
{
    int x = (W - text_w(s, sc)) / 2;
    const theme_t *th = &THEMES[theme];
    /* soft halo under the letters */
    for (int i = 0; s[i]; i++)
        if (s[i] != ' ') glow(x + i * 6 * sc + 2.5f * sc, y + 3.5f * sc, 5.5f * sc, th->hot[0], th->hot[1], th->hot[2], 0.35f);
    text_a(x, y, s, rgb(255, 255, 255), sc, 32);
}

static void render(void)
{
    draw_bg();
    draw_stars(0);
    draw_constellations();
    draw_stars(1);
    draw_orbs();
    draw_meteors();
    draw_parts();
    draw_comet();
    draw_stars(2);
    draw_pops();
    if (flash) fill_rect(0, 0, W, H, rgb(255, 255, 255), flash);

    if (state == ST_TITLE) {
        title_glow_text(52, "COMET GLIDE", 4);
        text_c(92, "BY ABED MIHILIA", rgb(200, 220, 255), 1, 32);
        int a = 16 + (int)(16 * sinf(frame * 0.08f));
        text_c(150, "TOUCH TO START", rgb(255, 255, 255), 2, a > 32 ? 32 : a);
        text_c(178, "DRAG TO FLY - CATCH THE LIGHT - DODGE THE METEORS", rgb(170, 190, 220), 1, 32);
        char b[24];
        sfmt(b, "BEST ", best, 6, NULL);
        text_c(198, b, rgb(255, 230, 150), 1, 32);
    } else if (state == ST_PLAY) {
        draw_hud();
        if (wave_banner > 0) {
            char b[16];
            sfmt(b, "WAVE ", wave + 1, 0, NULL);
            int a = wave_banner > 100 ? (120 - wave_banner) * 32 / 20 : wave_banner < 30 ? wave_banner * 32 / 30 : 32;
            text_c(96, b, rgb(255, 255, 255), 3, a);
        }
        if (paused) {
            fill_rect(0, 0, W, H, rgb(4, 4, 16), 18);
            title_glow_text(80, "PAUSED", 4);
            text_c(130, "TAP TO CONTINUE", rgb(255, 255, 255), 2, 32);
            text_c(156, "X AGAIN TO EXIT", rgb(200, 210, 240), 1, 32);
        }
    } else {
        draw_hud();
        int a = state_t > 40 ? 32 : state_t * 32 / 40;
        fill_rect(0, 0, W, H, rgb(4, 4, 16), a / 3);
        title_glow_text(70, "GAME OVER", 4);
        char b[24];
        sfmt(b, "SCORE ", score, 0, NULL);
        text_c(116, b, rgb(255, 255, 255), 2, a);
        if (score >= best && score > 0) text_c(140, "NEW BEST!", rgb(255, 230, 120), 2, a);
        else { sfmt(b, "BEST ", best, 0, NULL); text_c(140, b, rgb(255, 230, 150), 2, a); }
        if (state_t > 70) text_c(180, "TAP TO PLAY AGAIN", rgb(220, 230, 255), 1, 16 + (int)(16 * sinf(frame * 0.08f)));
    }
    draw_x_button();
}

/* ══ input ════════════════════════════════════════════════════════════════ */
static bool t_down;
static int t_lx, t_ly;

/* finger drag -> drag_dx/dy; *exit_tap when the X is tapped, *tap on a new touch elsewhere */
static void read_touch(papp_surface_t *s, bool *exit_tap, bool *tap)
{
    static bool was_x;
    papp_touch_point_t tp[5];
    int n = papp_disp_touch(s, tp, 5);
    bool on_x = false, any = false;
    int fx = 0, fy = 0;
    for (int i = 0; i < n; i++) {
        int x = tp[i].x, y = tp[i].y;
        if (x > W - 40 && y < 40) { on_x = true; continue; }
        if (!any) { any = true; fx = x; fy = y; }
    }
    *exit_tap = on_x && !was_x;
    was_x = on_x;
    *tap = any && !t_down;
    if (any) {
        if (t_down) { drag_dx += fx - t_lx; drag_dy += fy - t_ly; }
        t_lx = fx; t_ly = fy;
    }
    t_down = any;
}

/* ══ main ═════════════════════════════════════════════════════════════════ */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    papp_surface_cfg_t cfg = { .width = W, .height = H, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 2 };
    papp_surface_t *s = papp_disp_surface_create(&cfg);
    if (!s) { papp_log("comet: no surface\n"); return 2; }
    seed ^= (uint32_t)papp_time_us();
    load_save();
    init_stars();
    theme = 0;
    make_nebula(0);
    state = ST_TITLE;

    a_run = true;
    papp_thread_cfg_t tc = { .name = "comet_audio", .stack_size = 8192, .prio = PAPP_PRIO_HIGH, .core = -1 };
    a_thread = papp_thread_create(audio_thread, NULL, &tc);

    uint32_t prev = 0;
    int64_t last = papp_time_us();
    double acc = 0;
    while (!papp_exit_requested()) {
        uint32_t b = 0;
        papp_gamepad_t pad;
        for (int p = 0; p < 2; p++)
            if (papp_input_gamepad(p, &pad)) {
                b |= pad.buttons;
                if (pad.lx < -14000) b |= PAPP_BTN_LEFT;
                if (pad.lx > 14000) b |= PAPP_BTN_RIGHT;
                if (pad.ly < -14000) b |= PAPP_BTN_UP;
                if (pad.ly > 14000) b |= PAPP_BTN_DOWN;
            }
        bool exit_tap, tap;
        read_touch(s, &exit_tap, &tap);
        uint32_t pr = b & ~prev;
        prev = b;

        if (exit_tap) {
            if (state == ST_PLAY && !paused) paused = true;
            else break;
        }
        bool go = tap || (pr & (PAPP_BTN_A | PAPP_BTN_START));
        if (state == ST_TITLE && go) { new_game(); drag_dx = drag_dy = 0; go = false; }
        else if (state == ST_OVER && state_t > 70 && go) { new_game(); drag_dx = drag_dy = 0; go = false; }
        else if (state == ST_PLAY && paused && go) { paused = false; drag_dx = drag_dy = 0; }
        else if (state == ST_PLAY && (pr & PAPP_BTN_START)) paused = true;

        int64_t now = papp_time_us();
        acc += (now - last) / 1e6;
        last = now;
        if (acc > 0.1) acc = 0.1;
        while (acc >= 1.0 / 60) {
            btn = b;
            pressed = pr;
            if (!(state == ST_PLAY && paused)) update();
            else drag_dx = drag_dy = 0;
            pr = 0;
            acc -= 1.0 / 60;
        }

        px = papp_disp_lock(s, &stride);
        render();
        papp_disp_present(s);
    }

    a_run = false;
    if (a_thread) papp_thread_join(a_thread, 1000, NULL);
    papp_disp_surface_destroy(s);
    return 0;
}
